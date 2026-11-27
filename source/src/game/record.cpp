// The session record: what the PC and its controllers did, into the session log, so that a play session can be
// understood from the log alone (asked for 2026-11-26: "as MUCH information as technically possible"). On every PC,
// host or joined, from the start of the program:
//   clock:   the wall clock (UTC) once a minute: the two players' logs are lined up by it
//   system:  every 30 s the program's memory, the PC's memory load, the processor share of the program and of the
//            whole PC, handles and threads; mains or battery when that changes
//   window:  the game window in front or not, minimized, its size, whenever one of them changes
//   input:   every controller: plugged in or out, each button and trigger going down and up, each stick's direction
//            (eight directions and the middle) when it changes
//   pictures: a small picture of the game's own window every three seconds (S2_PICTURE_SECONDS=<n>, 0 for none), and
//            a full-size one when F9 is pressed or a check in the game asks for one ("mark:" lines; coop.cpp then
//            writes the state of both players and cameras at that moment). They go to the session's pictures folder
//            (detail.cpp). Only the game's window is ever taken, never the rest of the screen.
// The keyboard is not recorded: a log is sent to someone else and must not hold what was typed. (F9 is the one key
// looked at, and only whether it is down.)
// The game's own events are logged where they happen (coop.cpp, net.cpp, menu.cpp), frame pacing by the runtime
// ("pace:" lines, sdk/runtime/xdk/d3d8.cpp).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION  // (built here: detail.cpp uses its deflate too)
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

namespace game_detail {  // detail.cpp
bool On();
std::string PicturesDir();
void Line(const char* fmt, ...);
}

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

// ---- pictures ---------------------------------------------------------------------------------------------------
struct Shot {
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
};

// the game window's picture area as it is shown (the window draws itself into a bitmap: nothing else on the screen
// is taken, and the window need not be in front)
bool Grab(HWND hwnd, Shot& s) {
    RECT rc{};
    if (!hwnd || IsIconic(hwnd) || !GetClientRect(hwnd, &rc) || rc.right < 16 || rc.bottom < 16) return false;
    int w = rc.right, h = rc.bottom;
    HDC wdc = GetDC(hwnd);
    if (!wdc) return false;
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mdc ? CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        constexpr UINT kClientOnly = 1, kFullContent = 2;  // (PW_CLIENTONLY, PW_RENDERFULLCONTENT)
        ok = PrintWindow(hwnd, mdc, kClientOnly | kFullContent) != 0;
        GdiFlush();
        if (ok) {
            const uint8_t* p = static_cast<const uint8_t*>(bits);
            s.w = w, s.h = h;
            s.rgb.resize(size_t(w) * h * 3);
            for (size_t i = 0, n = size_t(w) * h; i < n; i++) s.rgb[i * 3] = p[i * 4 + 2], s.rgb[i * 3 + 1] = p[i * 4 + 1], s.rgb[i * 3 + 2] = p[i * 4];
        }
        SelectObject(mdc, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mdc) DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return ok;
}

// a picture made smaller: each new pixel is the average of the ones it covers
void Smaller(const Shot& in, int width, Shot& out) {
    if (in.w <= width) {
        out = in;
        return;
    }
    out.w = width, out.h = std::max(1, int(int64_t(in.h) * width / in.w));
    out.rgb.resize(size_t(out.w) * out.h * 3);
    for (int y = 0; y < out.h; y++) {
        int y0 = int(int64_t(y) * in.h / out.h), y1 = std::max(y0 + 1, int(int64_t(y + 1) * in.h / out.h));
        for (int x = 0; x < out.w; x++) {
            int x0 = int(int64_t(x) * in.w / out.w), x1 = std::max(x0 + 1, int(int64_t(x + 1) * in.w / out.w));
            uint32_t sum[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < in.h; yy++) {
                const uint8_t* p = &in.rgb[(size_t(yy) * in.w + x0) * 3];
                for (int xx = x0; xx < x1 && xx < in.w; xx++, p += 3) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], n++;
            }
            uint8_t* d = &out.rgb[(size_t(y) * out.w + x) * 3];
            for (int c = 0; c < 3; c++) d[c] = uint8_t(n ? sum[c] / n : 0);
        }
    }
}

bool SaveJpeg(const std::string& path, const Shot& s, int quality) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto put = [](void* file, void* data, int size) { std::fwrite(data, 1, size_t(size), static_cast<FILE*>(file)); };
    int ok = stbi_write_jpg_to_func(put, f, s.w, s.h, 3, s.rgb.data(), quality);
    std::fclose(f);
    return ok != 0;
}

std::mutex g_mark_lock;
std::vector<std::string> g_mark_asked;  // full-size pictures asked for, each with why
std::atomic<uint32_t> g_marks{0};       // how many were taken (the game writes its state when this moves)
std::string g_mark_why;                 // ... and why the newest one was

void Pictures() {
    int every = 3;
    if (const char* v = std::getenv("S2_PICTURE_SECONDS")) every = std::max(0, std::atoi(v));
    std::string dir = game_detail::PicturesDir();
    if (dir.empty()) return;
    LOGR("pictures: a small picture of the game's window every %d s and a full one at F9 (or when a check asks), into the session's pictures folder", every);
    Clock::time_point last = Clock::now();
    bool f9_before = false;
    uint32_t small_count = 0, mark_count = 0, failed = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        HWND hwnd = static_cast<HWND>(app::Window());
        if (!hwnd) continue;
        std::vector<std::string> marks;
        bool f9 = GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9_before) marks.push_back("F9 was pressed");
        f9_before = f9;
        {   // tests: S2_TEST_MARK_AT=<seconds>[,<seconds>...] asks for a mark at those times of the session (the times
            // of a controller script: menus, where the game's frames are not counted)
            static std::vector<double> at = [] {
                std::vector<double> list;
                for (const char* e = std::getenv("S2_TEST_MARK_AT"); e && *e; e = std::strchr(e, ',') ? std::strchr(e, ',') + 1 : nullptr)
                    list.push_back(std::atof(e));
                return list;
            }();
            double t = logx::Seconds();
            for (double& s : at)
                if (s > 0 && t >= s) marks.push_back("a test asked at " + std::to_string(int(s)) + " s"), s = 0;
        }
        {
            std::lock_guard l(g_mark_lock);
            for (auto& m : g_mark_asked) marks.push_back(std::move(m));
            g_mark_asked.clear();
        }
        Clock::time_point now = Clock::now();
        bool due = every > 0 && now - last >= std::chrono::seconds(every);
        if (marks.empty() && !due) continue;
        Shot full;
        double at = logx::Seconds();
        if (!Grab(hwnd, full)) {
            if (failed++ < 3) LOGI("pictures: the window's picture could not be taken (it is minimized, or Windows refused)");
            if (due) last = now;
            for (const auto& m : marks) LOGR("mark: %s (no picture: the window could not be taken)", m.c_str());
            if (!marks.empty()) {
                std::lock_guard l(g_mark_lock);
                g_mark_why = marks.back();
                g_marks++;
            }
            continue;
        }
        char name[64];
        if (!marks.empty() && mark_count < 400) {
            std::snprintf(name, sizeof name, "mark_%07.1f.jpg", at);
            bool ok = SaveJpeg(dir + "/" + name, full, 88);
            mark_count++;
            for (const auto& m : marks) LOGR("mark: %s: the picture is %s (%d x %d)%s", m.c_str(), name, full.w, full.h, ok ? "" : ", which could not be written");
            std::lock_guard l(g_mark_lock);
            g_mark_why = marks.back();
            g_marks++;
        }
        if (due) {
            last = now;
            Shot small_one;
            Smaller(full, 480, small_one);
            std::snprintf(name, sizeof name, "%07.1f.jpg", at);
            SaveJpeg(dir + "/" + name, small_one, 68);
            // (how bright it is, into the detail record: a sudden change shows there without opening the pictures)
            uint64_t sum[3] = {0, 0, 0};
            for (size_t i = 0; i + 2 < small_one.rgb.size(); i += 3) sum[0] += small_one.rgb[i], sum[1] += small_one.rgb[i + 1], sum[2] += small_one.rgb[i + 2];
            double n = double(small_one.rgb.size() / 3 + 1);
            game_detail::Line("pic %s window %dx%d mean %.1f %.1f %.1f", name, full.w, full.h, sum[0] / n, sum[1] / n, sum[2] / n);
            if (++small_count == 1) LOGI("pictures: the first one is %s (%d x %d from a window of %d x %d)", name, small_one.w, small_one.h, full.w, full.h);
        }
    }
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
    if (game_detail::On()) std::thread(Pictures).detach();
#endif
}

// a full-size picture of the window now, with why (any thread: a check in the game found something odd)
void Mark(const std::string& why) {
#ifdef _WIN32
    std::lock_guard l(g_mark_lock);
    if (g_mark_asked.size() < 8) g_mark_asked.push_back(why);
#else
    (void)why;
#endif
}
// how many marks there have been, and why the newest (the game writes its state into the log when the count moves)
uint32_t Marks(std::string* why) {
#ifdef _WIN32
    std::lock_guard l(g_mark_lock);
    if (why) *why = g_mark_why;
    return g_marks.load();
#else
    (void)why;
    return 0;
#endif
}
}  // namespace game_record
