// Freeze report. The game finishes a frame through one call (counted in hooks.cpp), so when that count stands
// still for a long time the game has stopped: either a long load or a freeze. After kQuietSeconds without frames
// every thread's call stack goes to the log, named (recompiled game functions by their export, sub_XXXXXXXX = the
// address in the Xbox image; the runtime's from Suffering2.pdb when it is there). It reports once per stop and
// says when frames come back, so a slow load shows up as a report followed by "frames again".
// Always on: it costs one wake-up a second.
//
// The report starts with the scheduler's view of every thread (base priority, the priority it has right now, its
// state): a stopped game is not always a lock, it can be a thread that never gets the processor. That part asks
// only Windows, so it is in the log even when a thread cannot be stopped for its stack.
//
// Priority watch: a thread of the game that Windows keeps in the real-time range (16 and up) for seconds although
// its own priority is an ordinary one is reported with its stack (checked twice a second). Windows lends a
// real-time thread's priority to the owner of a lock that thread waits for; a title thread that stays up there
// while it spins took a whole processor away from the others when they all shared one (the start-up hang of
// 2026-11-24, see sdk\runtime\kernel\gate.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace game_frames { uint64_t Count(); }  // hooks.cpp: frames the game has finished

namespace game_watchdog {
namespace {
constexpr int kQuietSeconds = 20;   // the longest level load seen takes about 8 s
constexpr int kStartSeconds = 30;   // the first frame comes about 2 s after start (the logos), or after a level load
constexpr int kMaxFrames = 48;

struct Stack {
    DWORD tid = 0;
    int n = 0;
    DWORD64 at[kMaxFrames];
};

// one thread's return addresses, taken while it is suspended. No allocation and no logging in here: the thread
// may hold the heap's or the log's lock.
void Capture(HANDLE thread, Stack& s) {
    if (SuspendThread(thread) == DWORD(-1)) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &c)) {
        while (s.n < kMaxFrames && c.Rip) {
            s.at[s.n++] = c.Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (fn) {
                void* handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler, &frame, nullptr);
            } else {  // a leaf: the return address is on top of the stack
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(reinterpret_cast<void*>(c.Rsp), &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
        }
    }
    ResumeThread(thread);
}

// What Windows' scheduler says about this process's threads (NtQuerySystemInformation, class 5).
struct Sched {
    DWORD tid = 0;
    int base = 0, now = 0;
    DWORD state = 0, reason = 0, switches = 0;
    double cpu_ms = 0;
};
std::vector<Sched> SchedulerView() {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const Query query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    std::vector<Sched> out;
    if (!query) return out;
    static std::vector<uint8_t> buf(1 << 20);
    for (int tries = 0; tries < 4; tries++) {
        ULONG need = 0;
        LONG st = query(5, buf.data(), ULONG(buf.size()), &need);
        if (st == 0) break;
        if (tries == 3) return out;
        buf.resize(std::max<size_t>(need, buf.size()) + (1 << 16));
    }
    const DWORD64 self = GetCurrentProcessId();
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, buf.data() + at, 4); return v; };
    auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, buf.data() + at, 8); return v; };
    for (size_t off = 0;;) {
        uint32_t next = u32(off), count = u32(off + 4);
        if (u64(off + 80) == self) {  // UniqueProcessId
            for (uint32_t i = 0; i < count; i++) {
                size_t t = off + 256 + size_t(i) * 80;  // SYSTEM_THREAD_INFORMATION
                Sched s;
                s.cpu_ms = double(u64(t) + u64(t + 8)) / 1e4;
                s.tid = DWORD(u64(t + 48));
                s.now = int(u32(t + 56));
                s.base = int(u32(t + 60));
                s.switches = u32(t + 64);
                s.state = u32(t + 68);
                s.reason = u32(t + 72);
                out.push_back(s);
            }
            break;
        }
        if (!next) break;
        off += next;
    }
    return out;
}
const char* StateName(DWORD state) {
    static const char* names[] = {"initialized", "ready", "running", "standby", "terminated", "waiting", "transition", "deferred ready", "gate wait"};
    return state < 9 ? names[state] : "?";
}
DWORD_PTR Affinity(DWORD tid) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return 0;
    struct { LONG status; PVOID teb; HANDLE pid, tid; ULONG_PTR affinity; LONG priority, base; } info{};
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const Query query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    DWORD_PTR mask = query && query(h, 0, &info, sizeof info, nullptr) == 0 ? info.affinity : 0;
    CloseHandle(h);
    return mask;
}
void LogSchedulerView(const char* what) {
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    for (const Sched& s : SchedulerView()) {
        DWORD_PTR a = Affinity(s.tid);
        char where[40] = "any processor";
        if (a && a != process) std::snprintf(where, sizeof where, "processors %llX", static_cast<unsigned long long>(a));
        char state[48];
        if (s.state == 5) std::snprintf(state, sizeof state, "waiting (reason %lu)", s.reason);
        else std::snprintf(state, sizeof state, "%s", StateName(s.state));
        LOGE("%s: scheduler: thread %lu priority %d (its own %d), %s, %s, %.0f ms of processor so far", what, s.tid, s.now,
             s.base, state, where, s.cpu_ms);
    }
}

// The recompiled game code (Suffering2.dll) has no PDB: its functions are named from its export table, where
// sub_XXXXXXXX is the function at that address of the Xbox image (the __imp__ twin is the same function's body).
struct GameCode {
    DWORD64 base = 0, end = 0;
    std::vector<std::pair<DWORD, const char*>> exports;  // by address
};
const GameCode& Game() {
    static GameCode g = [] {
        GameCode c;
        HMODULE m = GetModuleHandleW(L"Suffering2.dll");
        if (!m) return c;
        auto* b = reinterpret_cast<const uint8_t*>(m);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        c.base = reinterpret_cast<DWORD64>(m);
        c.end = c.base + nt->OptionalHeader.SizeOfImage;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return c;
        auto* e = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + dir.VirtualAddress);
        auto* names = reinterpret_cast<const DWORD*>(b + e->AddressOfNames);
        auto* ordinals = reinterpret_cast<const WORD*>(b + e->AddressOfNameOrdinals);
        auto* functions = reinterpret_cast<const DWORD*>(b + e->AddressOfFunctions);
        c.exports.reserve(e->NumberOfNames);
        for (DWORD i = 0; i < e->NumberOfNames; i++)
            c.exports.emplace_back(functions[ordinals[i]], reinterpret_cast<const char*>(b + names[i]));
        std::sort(c.exports.begin(), c.exports.end());
        return c;
    }();
    return g;
}

std::string Name(HANDLE process, DWORD64 addr) {
    const GameCode& game = Game();
    if (addr >= game.base && addr < game.end) {
        DWORD rva = DWORD(addr - game.base);
        auto it = std::upper_bound(game.exports.begin(), game.exports.end(), std::make_pair(rva, static_cast<const char*>(nullptr)),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
        char out[96];
        if (it == game.exports.begin()) {
            std::snprintf(out, sizeof out, "game!%X", rva);
        } else {
            --it;
            const char* name = it->second;
            if (!std::strncmp(name, "__imp__", 7)) name += 7;
            std::snprintf(out, sizeof out, "%s+%X", name, rva - it->first);
        }
        return out;
    }
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    char out[400];
    IMAGEHLP_MODULE64 mod{};
    mod.SizeOfStruct = sizeof mod;
    const char* module = SymGetModuleInfo64(process, addr, &mod) ? mod.ModuleName : "?";
    if (SymFromAddr(process, addr, &disp, si)) std::snprintf(out, sizeof out, "%s!%s+%llX", module, si->Name, static_cast<unsigned long long>(disp));
    else std::snprintf(out, sizeof out, "%s!%llX", module, static_cast<unsigned long long>(addr));
    return out;
}

std::string Line(HANDLE process, const Stack& s, bool symbols) {
    std::string line;
    for (int i = 0; i < s.n; i++) {
        std::string name = symbols ? Name(process, s.at[i]) : std::to_string(s.at[i]);
        if (!line.empty()) line += " < ";
        line += name;
        if (line.size() > 3400) break;
    }
    return line;
}

std::mutex g_report_lock;  // dbghelp is single threaded: the report and the priority watch take turns
bool Symbols(HANDLE process) {
    static bool symbols = false;
    if (!symbols) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        symbols = SymInitialize(process, app::ExeDir().string().c_str(), TRUE) != FALSE;
    } else {
        SymRefreshModuleList(process);
    }
    return symbols;
}

void Report(double quiet) {
    std::lock_guard<std::mutex> lock(g_report_lock);
    HANDLE process = GetCurrentProcess();
    bool symbols = Symbols(process);
    // First what cannot get stuck: the header and the scheduler's view.
    LOGE("freeze report: no frame for %.0f s. Scheduler's view, then each thread's stack, innermost first:", quiet);
    LogSchedulerView("freeze report");
    logx::Flush();
    // The process's own thread list, one handle at a time (NtGetNextThread). The system-wide snapshot is not used:
    // a process stuck in the kernel can make it hang, and that is the moment this report is for. Each stack is
    // logged as soon as it is taken: a thread that cannot be stopped (it never gets the processor, or it is deep in
    // a system call) holds the report up at that thread, and the log then shows the ones before it.
    using NextThread = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
    static const NextThread next = reinterpret_cast<NextThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread")));
    DWORD self = GetCurrentThreadId();
    HANDLE prev = nullptr;
    int count = 0;
    while (next) {
        HANDLE t = nullptr;
        if (next(process, prev, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &t) != 0) break;
        if (prev) CloseHandle(prev);
        prev = t;
        DWORD tid = GetThreadId(t);
        if (tid == self) continue;
        Stack st;
        st.tid = tid;
        Capture(t, st);
        count++;
        if (!st.n) continue;
        LOGE("freeze report: thread %lu: %s", st.tid, Line(process, st, symbols).c_str());
        logx::Flush();
    }
    if (prev) CloseHandle(prev);
    LOGE("freeze report: end, %d threads", count);
    logx::Flush();
}

void Run() {
    using clock = std::chrono::steady_clock;
    uint64_t last = game_frames::Count();
    auto moving = clock::now();
    bool seen_frames = false, reported = false;
    // S2_FREEZE_REPORT_AT=<seconds>: one report at that time whatever the game is doing, to check the report itself
    const char* test = std::getenv("S2_FREEZE_REPORT_AT");
    double test_at = test ? std::atof(test) : 0;
    const auto start = clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (test_at > 0 && std::chrono::duration<double>(clock::now() - start).count() >= test_at) {
            test_at = 0;
            Report(0);
        }
        uint64_t now = game_frames::Count();
        bool frames = now != last;
        last = now;
        if (frames) {
            if (reported) LOGE("freeze report: frames again after %.0f s", std::chrono::duration<double>(clock::now() - moving).count());
            moving = clock::now();
            seen_frames = true;
            reported = false;
            continue;
        }
        // The first start builds the game's code before anything runs (minutes, behind the SDK's progress window):
        // nothing is frozen then. The clock starts when the built code is there.
        if (!seen_frames) {
            std::error_code ec;
            if (!std::filesystem::exists(app::ExeDir() / "Suffering2.dll", ec)) {
                moving = clock::now();
                continue;
            }
        }
        double quiet = std::chrono::duration<double>(clock::now() - moving).count();
        // a game that has drawn stops for kQuietSeconds, or one that never draws at all for kStartSeconds
        if (!reported && quiet >= (seen_frames ? kQuietSeconds : kStartSeconds)) {
            reported = true;
            Report(quiet);
        }
    }
}

// Priority watch (see the top of the file). Windows raises a lock's owner into the real-time range for a moment
// whenever a real-time thread waits for that lock (seen many times a second, a few milliseconds each): only a
// raise that is still there after kLastingSeconds is reported. S2_PRIORITY_WATCH=1 reports every raise, checked
// every 4 ms, to see where they come from.
void Watch() {
    using clock = std::chrono::steady_clock;
    constexpr double kLastingSeconds = 2;
    const bool every = std::getenv("S2_PRIORITY_WATCH") != nullptr;
    struct Raised {
        DWORD tid;
        clock::time_point since;
        bool reported;
    };
    std::vector<Raised> raised;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(every ? 4 : 500));
        std::vector<Sched> view = SchedulerView();
        auto now = clock::now();
        for (const Sched& s : view) {
            bool up = s.now >= 16 && s.base < 16;
            auto it = std::find_if(raised.begin(), raised.end(), [&](const Raised& r) { return r.tid == s.tid; });
            if (!up) {
                if (it != raised.end()) {
                    if (it->reported) LOGE("priority watch: thread %lu is back at priority %d (its own %d) after %.1f s", s.tid, s.now,
                                           s.base, std::chrono::duration<double>(now - it->since).count());
                    raised.erase(it);
                }
                continue;
            }
            if (it == raised.end()) {
                raised.push_back({s.tid, now, false});
                it = raised.end() - 1;
            }
            if (it->reported || (!every && std::chrono::duration<double>(now - it->since).count() < kLastingSeconds)) continue;
            it->reported = true;
            std::lock_guard<std::mutex> lock(g_report_lock);
            HANDLE process = GetCurrentProcess();
            bool symbols = Symbols(process);
            Stack st;
            st.tid = s.tid;
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.tid)) {
                Capture(h, st);
                CloseHandle(h);
            }
            LOGE("priority watch: thread %lu runs at priority %d, its own is %d (%s). Stack: %s", s.tid, s.now, s.base,
                 StateName(s.state), st.n ? Line(process, st, symbols).c_str() : "not taken");
            if (!every) LogSchedulerView("priority watch");
            logx::Flush();
        }
    }
}
}  // namespace

// GameInfo::init
void Init() {
    std::thread(Run).detach();
    std::thread(Watch).detach();
}
}  // namespace game_watchdog
