// Hooks for default.xbe (The Suffering: Ties That Bind, Xbox, USA/Europe). A strong X86_FUNC(sub_XXXXXXXX) replaces
// the recompiled function at that address; __imp__sub_XXXXXXXX is the original.
#include <atomic>
#include <cstring>
#include <string>

#include "core/log.h"
#include "xdk/hle.h"

namespace {
std::string Line(uint32_t text) {
    std::string s = text ? kernel::ReadCString(text) : std::string();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
}  // namespace

// ---- debug text ------------------------------------------------------------------------------------------------
// OutputDebugStringA (XAPI, 0x225729, stdcall, 1 argument). The library hands the text to the kernel debugger with
// the debug service interrupt (mov eax, 1; lea ecx, string; int 2Dh; int3). With no debugger attached the console's
// kernel steps over both instructions and the function returns. The recompiler ends the function at the int3 and
// the runtime traps there (first seen right after the game opened tech\XboxLiveUIX\Suffering2_WideScreen.uix), so
// the host takes the call. The text goes to the log, where a debugger would have shown it.
X86_FUNC(sub_00225729) {
    (void)base;
    LOGR("game: %s", Line(kernel::R32(ctx.esp + 4)).c_str());
    kernel::Return(ctx, 0, 1);
}

// _assert (C runtime, 0x24B895, cdecl: expression, file, line). It builds "Assertion failed" text and raises it
// through the same debug service (service 5, at 0x24BCE2), then acts on the debugger's answer. With no debugger
// there is no answer: the text is logged as an error and the game carries on, as "ignore" would.
X86_FUNC(sub_0024B895) {
    (void)base;
    uint32_t expr = kernel::R32(ctx.esp + 4), file = kernel::R32(ctx.esp + 8), line = kernel::R32(ctx.esp + 12);
    LOGE("game assert: %s, %s line %u, called from %08X", Line(expr).c_str(), Line(file).c_str(), line,
         kernel::R32(ctx.esp));
    kernel::Return(ctx, 0, 0);
}

// ---- Xbox Live ---------------------------------------------------------------------------------------------------
// The runtime has no Xbox Live: its XOnlineStartup answers "done" without building the library's state
// ([0x4BD2BC] stays null), so every XOnline call the game still runs answers XONLINE_E_NOT_INITIALIZED
// (0x80150005). The game's Live UI library (UIX, created at start in 0x1CC690 from
// tech\XboxLiveUIX\Suffering2.uix) does not expect that from the calls below and the game then uses a null
// engine. They answer here as a console that was never signed up for Live does.

// XOnlineGetUsers(XONLINE_USER users[8], DWORD* count) (0x2FD676, stdcall): the accounts stored on the console.
// None: the buffer (8 users of 0xE0 bytes) is cleared as the library does, the count is 0.
X86_FUNC(sub_002FD676) {
    (void)base;
    uint32_t users = kernel::R32(ctx.esp + 4), count = kernel::R32(ctx.esp + 8);
    if (users) std::memset(kernel::HostPtr(users), 0, 0x700);
    if (count) kernel::W32(count, 0);
    kernel::Return(ctx, 0, 2);
}

// ---- Direct3D ----------------------------------------------------------------------------------------------------
// D3DDevice_InsertCallback(type, callback, context) (0x27B8F0): asks the graphics chip to call back when it reaches
// this point of its command stream. The game puts one before every Swap (0x1E6A00) with 0x1E6920, which only keeps
// two time stamps for its own frame timing display. The runtime does not run a command stream, so the library's
// code would write commands nobody reads: nothing is queued and nothing waits on it.
// It is also the one call the game makes exactly once a frame, right before the frame is shown: the frame counter
// the freeze report watches (watchdog.cpp).
namespace game_frames {
std::atomic<uint64_t> g_count{0};
uint64_t Count() { return g_count.load(); }
}  // namespace game_frames
namespace game_view { void Frame(X86Context& ctx); }
namespace game_coop { void Frame(X86Context& ctx); }
namespace game_measure { void Frame(); }
namespace game_skins { void Frame(X86Context& ctx); }
HLE_STDCALL(D3DDevice_InsertCallback, 3) {
    game_frames::g_count++;
    game_view::Frame(ctx);  // the picture's shape, checked once a frame on the game's thread
    game_coop::Frame(ctx);  // co-op (coop.cpp)
    game_skins::Frame(ctx);  // each player's character: Torque or Blackmore (skins.cpp)
    game_measure::Frame();  // tests: Player 1's place over time (measure.cpp)
    return 0;
}

// ---- controllers -------------------------------------------------------------------------------------------------
// XGetDevices(type) (XAPI, 0x33024F): the ports with a device of that type, and it marks them all as seen. The
// game's pad set-up (0x201330) calls XInitDevices and then XGetDevices once, throws the answer away, and from then
// on opens a pad only when XGetDeviceChanges reports it as newly plugged in (0x201440). That only works if the
// pads turn up after that first call: on the console XInitDevices starts looking for devices and they are found a
// moment later. The runtime's XInitDevices has the pads there at once, so this first call marked them seen and the
// game never opened one (the main menu ignored the controller). The first call answers "none yet" and leaves the
// record alone; the pad then arrives as a change on the next frame.
X86_EXTERN_FUNC(__imp__sub_0033024F);
X86_FUNC(sub_0033024F) {
    static bool first = true;
    if (first) {
        first = false;
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_0033024F(ctx, base);
}

// ---- picture shape -----------------------------------------------------------------------------------------------
// Every angle of view the game sets goes through this one function (0x1A6A30(float* angle), nine callers): its
// widescreen mode (renderer flag 0x200000 at [0x399624]+0x54) multiplies the angle by 4/3 here. The port sets the
// angle for the shape the picture is really shown in, whatever that flag says (view.cpp has the rule).
namespace game_view {
float Angle(float game_angle);
void Frame(X86Context& ctx);
}  // namespace game_view
X86_FUNC(sub_001A6A30) {
    (void)base;
    uint32_t value = kernel::R32(ctx.esp + 4);
    float angle;
    std::memcpy(&angle, kernel::HostPtr(value), 4);
    angle = game_view::Angle(angle);
    std::memcpy(kernel::HostPtr(value), &angle, 4);
    kernel::Return(ctx, value, 1);
}
