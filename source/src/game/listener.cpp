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
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game_coop {
extern uint32_t g_player_object;  // Player 1's engine object (coop.cpp)
}

// A sound "heard everywhere" is asked for with the "no place" vector (0x3932AC, the origin) as its place: it is meant
// to sit on the listener, which with no listening object is the origin too. Once the game hears from Player 1 (below),
// such a sound at the origin is far from him and the game never starts it: version 62 to 66 lost the guns' bang
// (003D000E, 003D0080, flags 4) after Load Game; run gun_trace4: 16 shots asked for, 0 started; gun_nofix (this
// switched off): 16 of 16. So while the game hears from Player 1, coop.cpp's play hook (0x191070) gives such a sound
// this stand-in place instead: a vector in guest memory kept at Player 1's place.
namespace game_listener {
uint32_t g_here = 0;     // the stand-in vector (guest memory)
bool g_standing = false; // the game hears from Player 1 now
uint32_t NoPlaceStandIn() { return g_standing && g_here ? g_here : 0; }
}

X86_EXTERN_FUNC(__imp__sub_001FEF80);
X86_FUNC(sub_001FEF80) {
    uint32_t device = ctx.ecx, out = kernel::R32(ctx.esp + 4);
    __imp__sub_001FEF80(ctx, base);
    static bool from_player = false;
    static const bool off = std::getenv("S2_NO_LISTENER_FIX") != nullptr;  // test switch: the game's own answer
    if (off) return;
    bool none = device && kernel::R16(device + 0x42) == 0;
    uint32_t player = game_coop::g_player_object;
    bool use = none && out && player && mem::IsCommitted(player + 0x50) && mem::IsCommitted(out + 12);
    if (use) {
        for (int i = 0; i < 3; i++) kernel::W32(out + i * 4, kernel::R32(player + 0x44 + i * 4));
        if (!game_listener::g_here) game_listener::g_here = mem::AllocSystem(16);
        for (int i = 0; i < 3; i++) kernel::W32(game_listener::g_here + i * 4, kernel::R32(player + 0x44 + i * 4));
    }
    game_listener::g_standing = use;
    if (use != from_player) {
        from_player = use;
        LOGI("listener: %s", use ? "the game has no listening object: it hears from Player 1" : "the game hears from its listening object again");
    }
}
