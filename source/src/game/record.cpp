// The session record: what the PC and its controllers did, into the session log, so that a play session can be
// understood from the log alone (asked for 2026-11-26: "as MUCH information as technically possible"). On every PC,
// host or joined, from the start of the program:
//   clock:   the wall clock (UTC) once a minute: the two players' logs are lined up by it
//   system:  every 30 s the program's memory, the PC's memory load, the processor share of the program and of the
//            whole PC, handles and threads; mains or battery when that changes
//   window:  the game window in front or not, minimized, its size, whenever one of them changes
//   input:   every controller: plugged in or out, each button and trigger going down and up, each stick's direction
//            (eight directions and the middle) when it changes
// The keyboard is not recorded: a log is sent to someone else and must not hold what was typed.
// The game's own events are logged where they happen (coop.cpp, net.cpp, menu.cpp), frame pacing by the runtime
// ("pace:" lines, sdk/runtime/xdk/d3d8.cpp).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"

namespace game_record {
namespace {
#ifdef _WIN32
using Clock = std::chrono::steady_clock;

// XInput's state, read through the library itself (the runtime's own reader hands the game one controller a port)
struct XState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, XState*);

GetStateFn XInput() {
    for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(name))
            if (auto f = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"))) return f;
    return nullptr;
}

const char* Direction(int x, int y, int before) {
    // (a stick is "out" past 40% and "back" under 25%: no chatter at the edge)
    static const char* const kNames[9] = {"the middle", "up", "up and right", "right", "down and right", "down", "down and left", "left", "up and left"};
    double r = std::sqrt(double(x) * x + double(y) * y) / 32767.0;
    if (r < (before ? 0.25 : 0.40)) return kNames[0];
    double a = std::atan2(double(x), double(y)) * 57.29578;  // 0 up, 90 right
    if (a < 0) a += 360;
    return kNames[1 + int((a + 22.5) / 45.0) % 8];
}

struct Controller {
    bool in = false;
    WORD buttons = 0;
    bool lt = false, rt = false;
    const char* left = nullptr;
    const char* right = nullptr;
};

void Controllers(GetStateFn get, Controller c[4]) {
    static const struct {
        WORD bit;
        const char* name;
    } kButtons[] = {{0x0001, "D-pad up"}, {0x0002, "D-pad down"}, {0x0004, "D-pad left"}, {0x0008, "D-pad right"}, {0x0010, "START"},
                    {0x0020, "BACK"},     {0x0040, "left stick click"}, {0x0080, "right stick click"}, {0x0100, "left bumper"},
                    {0x0200, "right bumper"}, {0x1000, "A"}, {0x2000, "B"}, {0x4000, "X"}, {0x8000, "Y"}};
    static int tick = 0;
    tick++;
    for (DWORD i = 0; i < 4; i++) {
        if (!c[i].in && tick % 60 != int(i)) continue;  // (an empty port is slow to ask: once a second)
        XState s{};
        bool in = get(i, &s) == ERROR_SUCCESS;
        if (in != c[i].in) {
            LOGR("input: controller %lu %s", i + 1, in ? "is plugged in" : "is gone");
            c[i] = {};
            c[i].in = in;
        }
        if (!in) continue;
        WORD changed = WORD(s.buttons ^ c[i].buttons);
        for (const auto& b : kButtons)
            if (changed & b.bit) LOGI("input: controller %lu: %s %s", i + 1, b.name, (s.buttons & b.bit) ? "down" : "up");
        c[i].buttons = s.buttons;
        bool lt = s.lt > (c[i].lt ? 30 : 60), rt = s.rt > (c[i].rt ? 30 : 60);
        if (lt != c[i].lt) LOGI("input: controller %lu: left trigger %s", i + 1, lt ? "down" : "up");
        if (rt != c[i].rt) LOGI("input: controller %lu: right trigger %s", i + 1, rt ? "down" : "up");
        c[i].lt = lt, c[i].rt = rt;
        const char* middle = Direction(0, 0, 1);
        const char* left = Direction(s.lx, s.ly, c[i].left && c[i].left != middle);
        const char* right = Direction(s.rx, s.ry, c[i].right && c[i].right != middle);
        if (c[i].left && left != c[i].left) LOGI("input: controller %lu: left stick %s", i + 1, left);
        if (c[i].right && right != c[i].right) LOGI("input: controller %lu: right stick %s", i + 1, right);
        c[i].left = left, c[i].right = right;
    }
}

uint64_t Ticks(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

void System() {
    static uint64_t proc_before = 0, idle_before = 0, all_before = 0;
    FILETIME made, gone, kernel, user, idle, sys_kernel, sys_user;
    double mine = -1, pc = -1;
    if (GetProcessTimes(GetCurrentProcess(), &made, &gone, &kernel, &user) && GetSystemTimes(&idle, &sys_kernel, &sys_user)) {
        uint64_t proc = Ticks(kernel) + Ticks(user), all = Ticks(sys_kernel) + Ticks(sys_user), idl = Ticks(idle);
        if (all_before && all > all_before) {
            mine = 100.0 * double(proc - proc_before) / double(all - all_before);
            pc = 100.0 * (1.0 - double(idl - idle_before) / double(all - all_before));
        }
        proc_before = proc, idle_before = idl, all_before = all;
    }
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    MEMORYSTATUSEX mem{sizeof mem};
    GlobalMemoryStatusEx(&mem);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    if (mine < 0) return;  // (the first reading has nothing to compare with)
    LOGI("system: the game uses %.0f MB (%.0f MB of its own), %.0f%% of the processor; the PC: %.0f%% of the processor, %lu%% of its memory, "
         "%.1f GB free; %lu handles",
         pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0, mine, pc, mem.dwMemoryLoad, mem.ullAvailPhys / 1073741824.0, handles);
}

void Power() {
    static int before = -2;
    SYSTEM_POWER_STATUS p{};
    if (!GetSystemPowerStatus(&p)) return;
    int now = p.ACLineStatus == 0 ? int(p.BatteryLifePercent) : -1;  // -1: on mains (or a desktop PC)
    if ((now < 0) == (before < 0) && before != -2) return;
    if (now < 0) {
        if (before != -2) LOGR("system: the PC is on mains power again");
    } else {
        LOGR("system: the PC runs on its battery (%d%%): it may slow itself down", now);
    }
    before = now;
}

void Window() {
    static int front_before = -1, mini_before = -1;
    static LONG w_before = 0, h_before = 0;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!hwnd) return;
    int front = GetForegroundWindow() == hwnd, mini = IsIconic(hwnd) != 0;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (front != front_before) LOGI("window: the game is %s", front ? "the window in front" : "not the window in front (another program is)");
    if (mini != mini_before && (mini || mini_before >= 0)) LOGI("window: the game is %s", mini ? "minimized" : "no longer minimized");
    if (!mini && (rc.right != w_before || rc.bottom != h_before)) {
        LOGI("window: the picture area is %ld x %ld", rc.right, rc.bottom);
        w_before = rc.right, h_before = rc.bottom;
    }
    front_before = front, mini_before = mini;
}

void WallClock() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    LOGI("clock: %04u-%02u-%02u %02u:%02u:%02u.%03u UTC", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

void Run() {
    GetStateFn get = XInput();
    if (!get) LOGW("input: no XInput library on this PC, controllers are not recorded");
    Controller controllers[4];
    Clock::time_point clock_at{}, system_at = Clock::now() - std::chrono::seconds(25);
    LOGR("record: this log holds the clock, the PC's load, the window, every controller's buttons and sticks, frame pacing, the "
         "game's and the network's events. The keyboard is not recorded.");
    for (;;) {
        Clock::time_point now = Clock::now();
        if (now - clock_at >= std::chrono::seconds(60)) clock_at = now, WallClock();
        if (now - system_at >= std::chrono::seconds(30)) system_at = now, System(), Power();
        Window();
        if (get) Controllers(get, controllers);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
#endif
}  // namespace

// GameInfo::init
void Init() {
#ifdef _WIN32
    if (std::getenv("S2_NO_RECORD")) return;
    std::thread(Run).detach();
#endif
}
}  // namespace game_record
