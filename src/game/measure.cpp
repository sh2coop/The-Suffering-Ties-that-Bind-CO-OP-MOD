// Measurements for the frame-rate work: the same controller script must move Torque the same way in the same time
// at 60 frames a second and at 120 (walking and turning speed, the height and length of a jump, how fast he falls).
//
// S2_MEASURE=1 writes Player 1's place and the way he faces to the log twenty times a second by the wall clock,
// with the count of frames and the game's own frame time:
//     measure: t 20.150 frame 1209 dt 0.01667 pos 110.25 88.00 237.50 yaw -93.0 carried 243
// tools\measure.py compares two runs' lines over the windows of scripts\measure.txt.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game_coop {
extern uint32_t g_player_object;  // coop.cpp: Player 1's engine object, found again each frame (0: no level in play)
extern uint32_t g_player;         // ... and the Player itself
}

namespace game_measure {
namespace {
constexpr uint32_t kMatrix = 0x14, kPosition = 0x44;  // an engine object's matrix and place
constexpr uint32_t kTimer = 0x384B78;                 // the game's timer object: its first float is the frame's time

// the clock starts with the process, as the controller script's does (pad.cpp): a line's time is the script's time
const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

float ReadF(uint32_t at) {
    float f;
    std::memcpy(&f, kernel::HostPtr(at), 4);
    return f;
}
}  // namespace

// once a frame (hooks.cpp)
void Frame() {
    static const bool on = std::getenv("S2_MEASURE") != nullptr;
    if (!on) return;
    static uint64_t frames = 0;
    static double next = 0;
    frames++;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    uint32_t object = game_coop::g_player_object;
    if (t < next || !object || !mem::IsCommitted(object + 0x110)) return;
    next = std::floor(t * 20.0 + 1.0) / 20.0;
    uint32_t timer = kernel::R32(kTimer);
    float yaw = std::atan2(ReadF(object + kMatrix + 0x20), ReadF(object + kMatrix + 0x28)) * 57.29578f;
    // what he carries, all counts added up (a Player's inventory at +0x128: entries of 12 bytes at [+0], their
    // number at +8, a count at +8 of each): it goes down when a weapon is loaded again, so the times of its steps
    // are the times of the reloads while he fires
    uint32_t player = game_coop::g_player, carried = 0;
    if (player && mem::IsCommitted(player + 0x134)) {
        uint32_t list = kernel::R32(player + 0x128), n = kernel::R32(player + 0x130);
        for (uint32_t i = 0; list && i < n && i < 64; i++) carried += kernel::R32(list + i * 12 + 8);
    }
    LOGI("measure: t %.3f frame %llu dt %.5f pos %.3f %.3f %.3f yaw %.1f carried %u", t, static_cast<unsigned long long>(frames),
         timer ? ReadF(timer) : 0.0f, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8), yaw, carried);
}
}  // namespace game_measure
