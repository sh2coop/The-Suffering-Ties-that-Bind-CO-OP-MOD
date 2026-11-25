// "The Suffering 2.exe": the one program in the finished folder.
//
//   The Suffering 2 PC\
//     The Suffering 2.exe     this
//     drop xbox iso here\      the player puts his own Xbox disc image of The Suffering: Ties That Bind here
//     Game Files\             the port: Suffering2.exe, its compiler folder, assets
//
// First start: it finds the image in "drop xbox iso here" (an .iso, or the disc's files with default.xbe), puts it where the
// port looks for it (Game Files\assets: a hard link, which takes no time and no room, or a copy when the two are on
// different drives), and starts the port, whose own first start builds the game's code from that image behind a
// progress window and then runs the game. Every later start just starts the game. Nothing is typed anywhere.
// No game data is in this program or in the folder it comes in.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t g_root[MAX_PATH * 2];
static HWND g_window;
static wchar_t g_line[400];

static void Path(wchar_t* out, size_t count, const wchar_t* rest) { _snwprintf(out, count, L"%ls\\%ls", g_root, rest); out[count - 1] = 0; }
static int Exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static int IsDir(const wchar_t* path) { DWORD a = GetFileAttributesW(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

// What the program says goes into Logs\launcher.log as well. With S2_QUIET set (tests) nothing is shown and a
// question is answered with yes.
static int Tell(const wchar_t* text, UINT kind) {
    wchar_t path[MAX_PATH * 2];
    Path(path, MAX_PATH * 2, L"Logs");
    CreateDirectoryW(path, NULL);
    Path(path, MAX_PATH * 2, L"Logs\\launcher.log");
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
        fclose(f);
    }
    if (_wgetenv(L"S2_QUIET")) return (kind & MB_YESNO) ? IDYES : IDOK;
    return MessageBoxW(g_window, text, L"The Suffering 2", kind);
}

// the first file in dir that ends in ext (case does not matter), "" when none
static int FindWithExt(const wchar_t* dir, const wchar_t* ext, wchar_t* out, size_t count) {
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*%ls", dir, ext);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snwprintf(out, count, L"%ls\\%ls", dir, f.cFileName);
        found = 1;
    } while (!found && FindNextFileW(h, &f));
    FindClose(h);
    return found;
}

// An image, or the disc's files, one folder deep at most (people unpack an archive into a folder of its own).
// -> 1 an .iso (out is the file), 2 files (out is the folder with default.xbe), 0 nothing, 3 only an archive
static int FindGame(const wchar_t* dir, wchar_t* out, size_t count, int depth) {
    wchar_t probe[MAX_PATH * 2];
    if (FindWithExt(dir, L".iso", out, count)) return 1;
    _snwprintf(probe, MAX_PATH * 2, L"%ls\\default.xbe", dir);
    if (Exists(probe)) { _snwprintf(out, count, L"%ls", dir); return 2; }
    int archive = FindWithExt(dir, L".7z", probe, MAX_PATH * 2) || FindWithExt(dir, L".zip", probe, MAX_PATH * 2) ||
                  FindWithExt(dir, L".rar", probe, MAX_PATH * 2);
    if (depth > 0) {
        wchar_t pattern[MAX_PATH * 2];
        _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileW(pattern, &f);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == L'.') continue;
                wchar_t sub[MAX_PATH * 2];
                _snwprintf(sub, MAX_PATH * 2, L"%ls\\%ls", dir, f.cFileName);
                int kind = FindGame(sub, out, count, depth - 1);
                if (kind == 1 || kind == 2) { FindClose(h); return kind; }
                if (kind == 3) archive = 1;
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
    }
    return archive ? 3 : 0;
}

// ---- the small window shown while a copy runs --------------------------------------------------------------------
static LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));
        HFONT font = CreateFontW(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(dc, font);
        InflateRect(&r, -24, -24);
        DrawTextW(dc, g_line, -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_CLOSE) return 0;  // a copy in progress is not left half done by a click
    return DefWindowProcW(w, m, wp, lp);
}
static void ShowLine(const wchar_t* text) {
    if (!g_window) {
        WNDCLASSW c = {0};
        c.lpfnWndProc = WindowProc;
        c.hInstance = GetModuleHandleW(NULL);
        c.lpszClassName = L"Suffering2Launcher";
        c.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_WAIT);
        c.hIcon = LoadIconW(c.hInstance, MAKEINTRESOURCEW(1));
        RegisterClassW(&c);
        int w = 720, h = 220;
        g_window = CreateWindowExW(0, c.lpszClassName, L"The Suffering 2", WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE,
                                   (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, NULL, NULL, c.hInstance, NULL);
    }
    wcsncpy(g_line, text, 399);
    InvalidateRect(g_window, NULL, TRUE);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
static DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER a, LARGE_INTEGER b, DWORD c, DWORD d, HANDLE e, HANDLE f, LPVOID g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 200) return PROGRESS_CONTINUE;
    last = now;
    wchar_t text[400];
    _snwprintf(text, 400, L"Copying your game image into Game Files\n\n%.1f of %.1f GB", done.QuadPart / 1073741824.0, total.QuadPart / 1073741824.0);
    ShowLine(text);
    return PROGRESS_CONTINUE;
}

static int RunQuiet(wchar_t* command) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

#include "update.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)inst; (void)prev; (void)show;
    GetModuleFileNameW(NULL, g_root, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(g_root, L'\\');
    // One source, two programs, told apart by name: "Build Recomp.exe" puts the image in place and builds the game;
    // "The Suffering 2.exe" plays it, and says so when it has not been built yet (asked for on 2026-11-25).
    int build = slash && (wcsstr(slash, L"Build") || wcsstr(slash, L"build"));
    int update = slash && wcsstr(slash, L"Update") != NULL;
    if (slash) *slash = 0;
    if (update) return Update();  // "Update.exe" (update.h)

    wchar_t files[MAX_PATH * 2], game[MAX_PATH * 2], assets[MAX_PATH * 2], og[MAX_PATH * 2], found[MAX_PATH * 2], probe[MAX_PATH * 2];
    Path(files, MAX_PATH * 2, L"Game Files");
    Path(game, MAX_PATH * 2, L"Game Files\\Suffering2.exe");
    Path(assets, MAX_PATH * 2, L"Game Files\\assets");
    Path(og, MAX_PATH * 2, L"drop xbox iso here");
    if (!Exists(game)) {
        Tell(L"The \"Game Files\" folder beside this program is missing or incomplete.\n\nUnpack the whole download again and start \"The Suffering 2\" from inside its folder.", MB_ICONERROR);
        return 1;
    }
    CreateDirectoryW(og, NULL);
    CreateDirectoryW(assets, NULL);

    // The two big compiler programs come zipped when the folder is the repository's own download (GitHub takes no
    // file over 100 MB, and clang.exe is 108): Build Recomp unpacks them once, with Windows' own tar.
    if (build) {
        static const wchar_t* const tools[] = {L"clang", L"lld-link"};
        for (int i = 0; i < 2; i++) {
            wchar_t exe[MAX_PATH * 2], zip[MAX_PATH * 2], dir[MAX_PATH * 2], command[MAX_PATH * 6];
            _snwprintf(exe, MAX_PATH * 2, L"%ls\\compiler\\%ls.exe", files, tools[i]);
            _snwprintf(zip, MAX_PATH * 2, L"%ls\\compiler\\%ls.zip", files, tools[i]);
            _snwprintf(dir, MAX_PATH * 2, L"%ls\\compiler", files);
            if (Exists(exe) || !Exists(zip)) continue;
            ShowLine(L"Unpacking the build tools");
            _snwprintf(command, MAX_PATH * 6, L"\"%ls\\System32\\tar.exe\" -xf \"%ls\" -C \"%ls\"", _wgetenv(L"SystemRoot") ? _wgetenv(L"SystemRoot") : L"C:\\Windows", zip, dir);
            if (RunQuiet(command) != 0 || !Exists(exe)) {
                Tell(L"The build tools could not be unpacked.\n\nCheck that the drive has about 1 GB free and that the folder can be written to, then double-click \"Build Recomp\" again.", MB_ICONERROR);
                return 1;
            }
        }
    }

    // the game is in place already?
    _snwprintf(probe, MAX_PATH * 2, L"%ls\\default.xbe", assets);
    int placed = Exists(probe) || FindWithExt(assets, L".iso", found, MAX_PATH * 2);
    wchar_t dll[MAX_PATH * 2];
    Path(dll, MAX_PATH * 2, L"Game Files\\Suffering2.dll");
    if (!build && !Exists(dll)) {
        Tell(L"The game has not been built yet.\n\n1. Put your Xbox disc image (the .iso file) into the folder \"drop xbox iso here\".\n2. Double-click \"Build Recomp\".\n\nThen start \"The Suffering 2\" again.", MB_ICONINFORMATION);
        return 0;
    }
    if (build && placed && Exists(dll) &&
        Tell(L"The game is already built.\n\nStart \"The Suffering 2\" to play. Build it again anyway?", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return 0;
    if (build && placed && Exists(dll)) DeleteFileW(dll);  // the port builds when its code is missing
    if (!placed) {
        int kind = FindGame(og, found, MAX_PATH * 2, 2);
        if (kind == 0 || kind == 3) {
            Tell(kind == 3 ? L"The \"drop xbox iso here\" folder holds an archive (.7z, .zip or .rar).\n\nUnpack it first, so that the folder holds the disc image itself (a file ending in .iso), then double-click \"Build Recomp\" again."
                           : L"Put your own Xbox disc image of The Suffering: Ties That Bind (a file ending in .iso) into the \"drop xbox iso here\" folder, then double-click \"Build Recomp\" again.\n\nThe folder opens now.",
                 MB_ICONINFORMATION);
            ShellExecuteW(NULL, L"open", og, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (kind == 1) {
            wchar_t to[MAX_PATH * 2];
            _snwprintf(to, MAX_PATH * 2, L"%ls\\game.iso", assets);
            if (!CreateHardLinkW(to, found, NULL)) {  // another drive, or a file system without links: a copy
                ShowLine(L"Copying your game image into Game Files");
                if (!CopyFileExW(found, to, CopyProgress, NULL, NULL, 0)) {
                    DWORD error = GetLastError();
                    DeleteFileW(to);
                    wchar_t text[600];
                    _snwprintf(text, 600, L"The game image could not be copied into Game Files (Windows error %lu).\n\nCheck that the drive has about 6 GB free and that the folder can be written to, then double-click \"Build Recomp\" again.", error);
                    Tell(text, MB_ICONERROR);
                    return 1;
                }
            }
        } else {  // the disc's files: Game Files\assets becomes a link to that folder
            RemoveDirectoryW(assets);
            wchar_t command[MAX_PATH * 5];
            _snwprintf(command, MAX_PATH * 5, L"cmd.exe /c mklink /J \"%ls\" \"%ls\"", assets, found);
            if (Exists(assets) || RunQuiet(command) != 0 || !IsDir(assets)) {
                CreateDirectoryW(assets, NULL);
                Tell(L"The game's files in \"drop xbox iso here\" could not be linked into Game Files.\n\nMove them into \"Game Files\\assets\" by hand (default.xbe must end up directly in that folder), then double-click \"Build Recomp\" again.", MB_ICONERROR);
                return 1;
            }
        }
    }
    if (g_window) DestroyWindow(g_window);

    // the port: its first start builds the game's code from the image (a progress window), then it is the game
    wchar_t command[MAX_PATH * 4];
    _snwprintf(command, MAX_PATH * 4, L"\"%ls\" %ls", game, args ? args : L"");
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, command, NULL, NULL, FALSE, 0, NULL, files, &si, &pi)) {
        wchar_t text[400];
        _snwprintf(text, 400, L"The game could not be started (Windows error %lu).", GetLastError());
        Tell(text, MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
