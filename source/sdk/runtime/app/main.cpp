// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cwchar>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2;
bool g_mode_changed = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running

// crash dump in CrashDumps beside the exe (see DumpDir)
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* e) {
    // the dump holds no guest memory: the guest registers and return addresses go to the log
    if (kernel::Thread* t = kernel::Current(); e && t && t->ctx) {
        auto* r = e->ExceptionRecord;
        LOGE("exception %08lX at %p on guest thread %X", r->ExceptionCode, r->ExceptionAddress, t->id);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1], g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  %s %llX, guest %08llX", r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)va,
                 (unsigned long long)(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 256);
    }
    auto dir = DumpDir();
    std::error_code dir_error;
    std::filesystem::create_directories(dir, dir_error);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    logx::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// window thread only
void ApplyNow() {
    const Settings& s = Get();
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (g_mode_changed) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        g_mode_changed = false;
    }
    Mode out = Output();
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;  // a window held back for a splash or handover stays so
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop);
    // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        g_mode_changed = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
        GetMonitorInfoW(mon, &mi);
    }
    // [local change, The Suffering: Ties That Bind] XBE_TEST_HIDDEN=1 (test runs): a window of the size asked for,
    // far off every screen, never in front and not on the taskbar, so that a test does not get in the way of
    // whoever is using the PC. It still draws, and can be captured (PrintWindow).
    static const bool hidden = std::getenv("XBE_TEST_HIDDEN") != nullptr;
    if (hidden) {
        // (S2_TEST_WINDOW=<width>x<height>: the window that size whatever the resolution says, as a Borderless window is
        // on a monitor of another size)
        uint32_t ww = out.width, wh = out.height;
        if (const char* size = std::getenv("S2_TEST_WINDOW")) {
            uint32_t a = 0, b = 0;
            const char* c = size;
            while (*c >= '0' && *c <= '9') a = a * 10 + uint32_t(*c++ - '0');
            if (*c == 'x') c++;
            while (*c >= '0' && *c <= '9') b = b * 10 + uint32_t(*c++ - '0');
            if (a >= 64 && b >= 64) ww = a, wh = b;
        }
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        SetWindowPos(g_hwnd, HWND_BOTTOM, -30000, -30000, int(ww), int(wh), SWP_FRAMECHANGED | SWP_NOACTIVATE);
        return;
    }
    if (s.display_mode == DisplayMode::Windowed) {
        RECT r{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& a = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h,
                     SWP_FRAMECHANGED);
    } else {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_F3) DumpThreads();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
                    g_mode_changed = false;
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (Get().display_mode == DisplayMode::Fullscreen && Get().width && Get().height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        case kApplyDisplay:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ApplyNow();
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            if (std::getenv("XBE_TEST_HIDDEN")) {  // [local change] a test run: shown, never brought to the front
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                return 0;
            }
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::filesystem::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

// [local change, The Suffering: Ties That Bind] Where the session logs and the crash dumps go. In the folder that is
// handed out the executable sits in "Game Files" under the folder with the one program the player starts; there the
// logs go to "Logs" beside that program, crash dumps to "Logs\crash dumps": one folder to find, zip and send
// (asked for on 2026-11-25). Anywhere else: "logs" and "CrashDumps" beside the executable, as before.
bool HandedOut() { return _wcsicmp(ExeDir().filename().c_str(), L"Game Files") == 0; }
std::filesystem::path LogDir() { return HandedOut() ? ExeDir().parent_path() / "Logs" : ExeDir() / "logs"; }
std::filesystem::path DumpDir() { return HandedOut() ? LogDir() / "crash dumps" : ExeDir() / "CrashDumps"; }


int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }

namespace {

// the game window's monitor's refresh rate, read again each second (a mode change, a move to another monitor); 0
// when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static ULONGLONG next = 0;
    if (ULONGLONG now = GetTickCount64(); now >= next) {
        next = now + 1000;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        rate = GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
                       EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                   ? mode.dmDisplayFrequency
                   : 0;
    }
    return rate;
}

}  // namespace

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    // a few under the refresh rate, so a variable refresh display stays in its range and vsync never waits a whole
    // refresh
    constexpr uint32_t kRefreshMargin = 3;
    if (cap == kFpsCapRefresh) cap = RefreshRate() > kRefreshMargin ? RefreshRate() - kRefreshMargin : 0;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(app::WriteCrashDump);
    std::signal(SIGABRT, [](int) { app::WriteCrashDump(nullptr); });
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init(app::LogDir().string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!app::PrepareGameCode(game)) return 1;

    const bool test_hidden = std::getenv("XBE_TEST_HIDDEN") != nullptr;  // [local change] no splash in a hidden test run
    bool splash = !test_hidden && app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, 0, 0, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, test_hidden ? SW_SHOWNOACTIVATE : SW_SHOW);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::wstring hdd = app::Instance() > 1 ? L"hdd" + std::to_wstring(app::Instance()) : L"hdd";
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        MessageBoxW(app::g_hwnd, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app::g_mode_changed) ChangeDisplaySettingsW(nullptr, 0);
    logx::Flush();
    kernel::Shutdown(0);
}
