// The game's frame time, to better than a millisecond.
//
// The game's clock (the object at 0x3934A4) counts the kernel's ticks, whole milliseconds: 0x1E6DD0 (this = clock,
// where the frame time goes) takes the ticks since the last call, times the clock's scale (+0x2C; slow motion) and
// 0.001 (+0x14), and the timer's once-a-frame method (0x18F610, this = [0x384B78]) keeps it between 0.0001 s and the
// timer's limit (+0xC4). At 60 frames a second the game so sees frames of 16 and 17 ms in turn, at 120 of 8 and 9
// (a twelfth of a frame's movement more or less from one frame to the next, on frames that are shown evenly), at
// 144 of 6 and 7. Here the frame time is the time that really went by since the last call, by the host's clock.
// The game's own figure stays when the two are further apart than its rounding explains (the first frame, a
// stall), and with S2_MS_CLOCK=1 (tests: the game's own whole milliseconds).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_001E6DD0);
X86_FUNC(sub_001E6DD0) {
    uint32_t clock = ctx.ecx, out = kernel::R32(ctx.esp + 4);
    __imp__sub_001E6DD0(ctx, base);
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static const bool own = std::getenv("S2_MS_CLOCK") != nullptr;
    Clock::time_point now = Clock::now();
    double real = std::chrono::duration<double>(now - last).count();
    last = now;
    if (own || !out) return;
    float scale, game;
    std::memcpy(&scale, kernel::HostPtr(clock + 0x2C), 4);
    std::memcpy(&game, kernel::HostPtr(out), 4);
    double exact = real * double(scale);
    constexpr double kRounding = 0.0025;  // the ticks are whole milliseconds, and the tick is not on the frame's edge
    if (std::fabs(exact - double(game)) > kRounding * std::max(1.0, double(scale))) return;
    float f = float(exact);
    std::memcpy(kernel::HostPtr(out), &f, 4);
    static bool said = false;
    if (!said) said = true, LOGR("timing: the game's frame time is the real one, not whole milliseconds (first: %.3f ms for the game's %.0f)", exact * 1000, double(game) * 1000);
}
