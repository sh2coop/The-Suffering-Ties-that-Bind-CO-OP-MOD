// pad 0 from a script, then GameInfo::pad as on Windows
// SCRIPT file: lines of "frame buttons [frames held]", held 8 frames by default, # comments; buttons joined by +:
// UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR (left stick up, down, left, right)
// cmd in the run folder: "buttons [frames held]" pressed once from the frame it is read, then removed
#include <cstdio>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "harness.h"

namespace input {

namespace {

struct Press {
    double at;
    std::string buttons;
    double len;
    bool relative = false;  // at counts from harness::g_script_base
};

std::vector<Press> LoadScript() {
    std::vector<Press> v;
    const char* p = std::getenv("SCRIPT");
    if (FILE* f = p ? std::fopen(p, "r") : nullptr) {
        char line[160];
        while (std::fgets(line, sizeof line, f)) {
            char b[64];
            double t, len = 8;
            bool rel = line[0] == '+';
            if (line[0] != '#' && std::sscanf(rel ? line + 1 : line, "%lf %63s %lf", &t, b, &len) >= 2) v.push_back({t, b, len, rel});
        }
        std::fclose(f);
        LOGI("harness: pad script %s, %zu presses", p, v.size());
    }
    return v;
}

void Apply(const std::string& s, Pad& p) {
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
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
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

}  // namespace

Pad Poll(int port) {
    static const std::vector<Press> script = LoadScript();
    static Press live{-1, "", 0};
    Pad p;
    p.connected = port == 0;
    uint64_t base = harness::g_script_base.load();
    auto now = [&](const Press& s) {  // the frame a press is measured against: -1 while its base is unset
        if (!s.relative) return double(harness::g_frames.load());
        return base ? double(harness::g_frames.load()) - double(base) : -1.0;
    };
    if (port == 1) {  // "P2:buttons" lines press pad 2
        for (const Press& s : script)
            if (s.buttons.rfind("P2:", 0) == 0) {
                p.connected = true;
                double t = now(s);
                if (t >= s.at && t <= s.at + s.len) Apply(s.buttons.substr(3), p);
            }
    }
    if (port == 0) {
        double t = double(harness::g_frames.load());
        std::string cmd = (app::ExeDir() / "cmd").string();
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            char b[64] = {};
            double len = 3;
            if (std::fscanf(f, "%63s %lf", b, &len) >= 1) live = {t, b, len};
            std::fclose(f);
            std::remove(cmd.c_str());
        }
        for (const Press& s : script) {
            double ts = now(s);
            if (ts >= s.at && ts <= s.at + s.len && s.buttons.rfind("P2:", 0) != 0) Apply(s.buttons, p);
        }
        if (live.at >= 0 && t >= live.at && t <= live.at + live.len) Apply(live.buttons, p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}

}  // namespace input
