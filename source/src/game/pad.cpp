// Pad 0 from the keyboard, a script file and a command file (Windows build). Test tooling, after the Batman Begins
// port's, but timed in seconds: the runtime polls every port four times a second on its own, so poll counts are
// not frames.
//   Keyboard (while the game window has the focus): arrows = d-pad, Enter = START, Backspace = BACK,
//             Space or J = A, K = B, U = X, I = Y, O = white, P = black, W/A/S/D = left stick, Q = left trigger,
//             E = right trigger, Numpad 8/2/4/6 = right stick.
//   pad_script2.txt beside the exe: the same for a second controller on port 1 (it is plugged in when the file
//     has presses).
//   pad_script.txt beside the exe: lines "seconds buttons [seconds held]" (0.15 by default), # comments, buttons
//     joined by +: UP DOWN LEFT RIGHT START BACK A B X Y WHITE BLACK LT RT LS LSD LSL LSR RS RSD RSL RSR L3 R3.
//     Seconds count from the start of the game process.
//   cmd beside the exe, read ten times a second and emptied: "CAPTURE" saves a frame (needs XBE_CAPTURE_DIR),
//     "PEEK address [count]" (hex) logs dwords of the game's memory, "POKES address text" writes a string there,
//     "buttons [seconds held]" presses them from now.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"

namespace game_coop {
void Controller2(input::Pad& p);  // coop.cpp
bool SecondStart();               // Player 2 is pressing START: the pause is both players'
void SecondLook(input::Pad& p);   // Player 2's own look settings, on his right stick
bool PausedBySecond();            // the pause menu is up and Player 2 opened it: it is his to use
input::Pad SecondRaw();
}
namespace game_menu {
bool PauseItemP1Only();  // Player 2 has the pause menu and its focus is on Save, Load or Quit
}
namespace game_pad {
namespace {
struct Press { double at; std::string buttons; double len; };

const auto g_start = std::chrono::steady_clock::now();
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

std::mutex g_lock;
Press g_live{-1, "", 0};          // the cmd file's press
std::atomic<uint64_t> g_polls{0};  // port 0 polls so far

std::vector<Press> LoadScript(const char* name = "pad_script.txt") {
    std::vector<Press> v;
    std::string path = (app::ExeDir() / name).string();
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[160];
        while (std::fgets(line, sizeof line, f)) {
            char b[64]; double t, len = 0.15;
            if (line[0] != '#' && std::sscanf(line, "%lf %63s %lf", &t, b, &len) >= 2) v.push_back({t, b, len});
        }
        std::fclose(f);
    }
    if (!v.empty()) LOGR("pad: %s with %zu presses", name, v.size());
    return v;
}

void Apply(const std::string& s, input::Pad& p) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        if (b == "UP") p.buttons |= 1;
        else if (b == "DOWN") p.buttons |= 2;
        else if (b == "LEFT") p.buttons |= 4;
        else if (b == "RIGHT") p.buttons |= 8;
        else if (b == "START") p.buttons |= 16;
        else if (b == "BACK") p.buttons |= 32;
        else if (b == "L3") p.buttons |= 64;
        else if (b == "R3") p.buttons |= 128;
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
        else if (b == "BLACK") p.analog[4] = 255;
        else if (b == "WHITE") p.analog[5] = 255;
        else if (b == "LT") p.analog[6] = 255;
        else if (b == "RT") p.analog[7] = 255;
        else if (b == "LS") p.ly = 32767;
        else if (b == "LSD") p.ly = -32767;
        else if (b == "LSL") p.lx = -32767;
        else if (b == "LSR") p.lx = 32767;
        else if (b == "RS") p.ry = 32767;
        else if (b == "RSD") p.ry = -32767;
        else if (b == "RSL") p.rx = -32767;
        else if (b == "RSR") p.rx = 32767;
    }
}

#ifdef _WIN32
bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
void Keyboard(input::Pad& p) {
    if (GetForegroundWindow() != (HWND)app::Window()) return;  // only while the game has the keyboard
    if (Key(VK_UP)) p.buttons |= 1;
    if (Key(VK_DOWN)) p.buttons |= 2;
    if (Key(VK_LEFT)) p.buttons |= 4;
    if (Key(VK_RIGHT)) p.buttons |= 8;
    if (Key(VK_RETURN)) p.buttons |= 16;
    if (Key(VK_BACK)) p.buttons |= 32;
    if (Key(VK_SPACE) || Key('J')) p.analog[0] = 255;
    if (Key('K')) p.analog[1] = 255;
    if (Key('U')) p.analog[2] = 255;
    if (Key('I')) p.analog[3] = 255;
    if (Key('P')) p.analog[4] = 255;
    if (Key('O')) p.analog[5] = 255;
    if (Key('Q')) p.analog[6] = 255;
    if (Key('E')) p.analog[7] = 255;
    if (Key('W')) p.ly = 32767;
    if (Key('S')) p.ly = -32767;
    if (Key('A')) p.lx = -32767;
    if (Key('D')) p.lx = 32767;
    if (Key(VK_NUMPAD8)) p.ry = 32767;
    if (Key(VK_NUMPAD2)) p.ry = -32767;
    if (Key(VK_NUMPAD4)) p.rx = -32767;
    if (Key(VK_NUMPAD6)) p.rx = 32767;
}
#endif

// the cmd file, off the game's threads so it works whether or not the game reads the pad
void Watch() {
    std::string cmd = (app::ExeDir() / "cmd").string();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        FILE* f = std::fopen(cmd.c_str(), "r");
        if (!f) continue;
        char line[160] = {}, b[64] = {}; double len = 0.15;
        bool got = std::fgets(line, sizeof line, f) && std::sscanf(line, "%63s %lf", b, &len) >= 1;
        std::fclose(f);
        if (!got) continue;
        if (FILE* w = std::fopen(cmd.c_str(), "w")) std::fclose(w);
        if (std::string(b) == "PEEK") {
            // "PEEK address [count]" in hex: that many dwords of the game's memory, into the log
            unsigned addr = 0, count = 8;
            std::sscanf(line, "%*s %x %x", &addr, &count);
            count = std::min(count, 64u);
            std::string out;
            char t[16];
            for (unsigned i = 0; i < count; i++) {
                std::snprintf(t, sizeof t, " %08X", kernel::R32(addr + i * 4));
                out += t;
            }
            LOGR("peek %08X:%s", addr, out.c_str());
        } else if (std::string(b) == "POKES") {
            // "POKES address text": the text and its terminating zero into the game's memory (a test's level name)
            unsigned addr = 0;
            int at = 0;
            if (std::sscanf(line, "%*s %x %n", &addr, &at) >= 1 && at > 0 && addr) {
                std::string text(line + at);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
                std::memcpy(kernel::HostPtr(addr), text.c_str(), text.size() + 1);
                LOGR("poke %08X: \"%s\"", addr, text.c_str());
            }
        } else if (std::string(b) == "CAPTURE") {
            gpu::g_capture = true;
            LOGR("pad: capture asked at %.1f s, %llu polls of port 0 so far", Now(), (unsigned long long)g_polls.load());
        } else {
            std::lock_guard<std::mutex> lock(g_lock);
            g_live = {Now(), b, len};
        }
    }
}
}  // namespace

// GameInfo::init
// (S2_NO_CMD=1: this game leaves the cmd file alone: the second of two games started from one folder in a test)
// (S2_CAPTURE_AT=<seconds>: one CAPTURE at that time, for a game that does not read the cmd file)
void Init() {
    if (!std::getenv("S2_NO_CMD")) std::thread(Watch).detach();
    if (const char* at = std::getenv("S2_CAPTURE_AT")) {
        double when = std::atof(at);
        std::thread([when] {
            while (Now() < when) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            gpu::g_capture = true;
            LOGR("pad: capture asked at %.1f s", Now());
        }).detach();
    }
}

// reads of port 0 so far: one a frame while the game runs (watchdog.cpp)
uint64_t Polls() { return g_polls.load(); }

void Hook(int port, input::Pad& p) {
    // A hidden test run (XBE_TEST_HIDDEN, tools/run.ps1) plays its scripts and nothing else: the
    // controllers plugged into this PC are the owner's, in his hands for whatever he is doing meanwhile. (Found
    // 2026-11-27: his presses of START, A, B and Y reached a hidden test of the apartment level and paused it.)
    static const bool hidden = std::getenv("XBE_TEST_HIDDEN") != nullptr;
    if (hidden) {
        static bool said = false;
        if (!said && (p.buttons || p.analog[0] || p.analog[1] || p.analog[2] || p.analog[3] || p.analog[6] || p.analog[7]))
            said = true, LOGR("pad: a hidden test run: the controllers plugged into this PC are left out (a button was pressed on port %d)", port);
        p = {};
    }
    if (port == 1) {
        // a second controller for tests: pad_script2.txt beside the exe plugs one in on port 1 and plays it
        static const std::vector<Press> script2 = LoadScript("pad_script2.txt");
        if (!script2.empty()) {
            p.connected = true;
            double t = Now();
            for (const Press& s : script2)
                if (t >= s.at && t < s.at + s.len) Apply(s.buttons, p);
        }
        game_coop::SecondLook(p);
        game_coop::Controller2(p);  // co-op takes Player 2's teleport hold, START and BACK for itself
        return;
    }
    if (port != 0) return;
    // (S2_PAD_SCRIPT=<file> names another script file than pad_script.txt)
    static const std::vector<Press> script = LoadScript(std::getenv("S2_PAD_SCRIPT") ? std::getenv("S2_PAD_SCRIPT") : "pad_script.txt");
    g_polls++;
    double t = Now();
    p.connected = true;
    for (const Press& s : script)
        if (t >= s.at && t < s.at + s.len) Apply(s.buttons, p);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_live.at >= 0 && t >= g_live.at && t < g_live.at + g_live.len) Apply(g_live.buttons, p);
    }
#ifdef _WIN32
    Keyboard(p);
#endif
    if (game_coop::PausedBySecond()) {  // Player 2's pause menu: his controller is the menu's, Player 1 waits
        bool connected = p.connected;
        p = game_coop::SecondRaw();
        p.connected = connected;
        if (game_menu::PauseItemP1Only()) p.analog[0] = 0;  // an item that is Player 1's is in focus: A does nothing there
    } else if (game_coop::SecondStart()) {
        p.buttons |= 16;  // Player 2's START pauses for both
    }
}
}  // namespace game_pad
