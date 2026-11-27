// Where the game hears from, when its listening object is gone.
//
// The sound device ([0x398CB0]) keeps the handle of the object it hears from at +0x42; 0x1FEF80 gives that object's
// place (+0x44), and when the handle is 0 or its table entry is empty, the "no place" vector 0x3932AC: the origin.
// 0x191070's audibility test then refuses every sound that has a place in the level as too far away: footsteps,
// hits, the voices and noises of a vision (whose slow motion waits for them: the owner's stuck vision in the
// apartment, version 59). coop.cpp's Ears() puts the game's Camera back as the listener every frame, but after Load
// Game the game's camera pointer is to no object of the table (runs hdload_0 with HD sound off and hdload_1 with it
// on, 03a: 120 to 155 sounds refused every 10 s after Load Game; the owner's sessions of 2026-11-27, version 61).
// Making some other object the listener is not safe: only a Camera's end clears the listener, and a freed Player 1 as
// the listener crashed the next Load Game (run hdload_fix3). So the handle is left alone and only the place is
// given: with no listening object, the game hears from Player 1.
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game_coop {
extern uint32_t g_player_object;  // Player 1's engine object (coop.cpp)
}

X86_EXTERN_FUNC(__imp__sub_001FEF80);
X86_FUNC(sub_001FEF80) {
    uint32_t device = ctx.ecx, out = kernel::R32(ctx.esp + 4);
    __imp__sub_001FEF80(ctx, base);
    static bool from_player = false;
    bool none = device && kernel::R16(device + 0x42) == 0;
    uint32_t player = game_coop::g_player_object;
    bool use = none && out && player && mem::IsCommitted(player + 0x50) && mem::IsCommitted(out + 12);
    if (use) {
        for (int i = 0; i < 3; i++) kernel::W32(out + i * 4, kernel::R32(player + 0x44 + i * 4));
    }
    if (use != from_player) {
        from_player = use;
        LOGI("listener: %s", use ? "the game has no listening object: it hears from Player 1" : "the game hears from its listening object again");
    }
}
