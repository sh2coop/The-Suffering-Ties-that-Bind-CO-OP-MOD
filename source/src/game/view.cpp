// Picture shape: the game's view follows the shape of the picture it is shown in (the monitor's, by default).
//
// How the game builds a view (read from the code and checked on the running game):
//   - The view object ([0x3934DC], 0xAC bytes, methods at 0x34A110) owns cameras of 0x2C4 bytes each (array at
//     +0x40, count at +0x48, current at +0x4C; methods at 0x34B5F4). +0x54 is the angle a new camera starts with.
//   - A camera keeps d = 0.3125 / tan(angle / 2) at +0x280 (and at +0x274): the distance at which the picture is
//     0.625 wide. Method 1 (0x1BB6D0) sets it from an angle in degrees, method 0 (0x1BAD50: x, y, width, height)
//     works out everything else, among it the picture's height at that distance, +0x258 = shape * 0.3125.
//   - "shape" comes from the display object at camera +0x2C0, method +0x34 = 0x2031D0(width, height):
//         height / width * (widescreen ? 0.75 : 1) / 0.86
//     The 0.86 is the console's: its 640 pixels do not span the whole television picture, so the game draws for a
//     picture 1.147 wide per 1 high on a 4:3 set and 1.529 on a widescreen set.
//   - Its widescreen switch (renderer flag 0x200000 at [0x399624]+0x54) does one more thing: every angle goes
//     through 0x1A6A30, which multiplies it by 4/3.
// So on the console a view of angle a shows, top to bottom, tan = tan(a / 2) * 0.872 on a 4:3 set and
// tan(a * 4/3 / 2) * 0.654 on a widescreen set (a = 48 degrees in play: 42.4 and 44.5 degrees high).
//
// On the PC the frame is shown at exactly the shape asked for, so the 0.86 has to go (with it everything was drawn
// 16 percent too wide) and the shape can be any. The rule here: top to bottom the view is the console's (the 4:3
// one up to 4:3, the widescreen one from 16:9 on, a smooth step between the two), and the width follows the
// picture's shape. A wider monitor shows more at the sides and never less at the top and bottom; a monitor
// narrower than 4:3 keeps the 4:3 width and shows more above and below.
//
// S2_ASPECT=<width/height, such as 2.4> forces a shape (tests). Without it the shape is that of the output: the
// window in windowed mode, the monitor otherwise.
//
// The Display settings' Picture shape chooses another shape (the picture then has bars at the sides or above and
// below). Not in split screen: there the picture always fills the output, the two views share all of it. (The owner,
// 2026-11-27, with 16:9 chosen on a 3840 x 1600 monitor: "theres still black bars where there could be more camera
// space for both players", and then "make sure that splitscreen uses as much space on the screen as possible in all
// resolutions".) In split screen the shape is the game window's own: Borderless puts the window over the whole
// monitor whatever the resolution says (a 1920 x 1080 resolution on a 3840 x 1600 monitor had bars at the sides),
// Windowed is the resolution's size until the player drags its edges, Full screen is the resolution.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "xdk/hle.h"

namespace game_coop { extern uint32_t g_no_texture; }  // coop.cpp: the stand-in texture of Player 2's view camera
namespace game_view {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFourThree = 4.0 / 3.0, kSixteenNine = 16.0 / 9.0;
constexpr double kShape43 = 0.75 / 0.86, kShapeWide = 0.75 * 0.75 / 0.86;  // height per width the console draws for
constexpr double kMaxAngle = 170.0;

double Rad(double deg) { return deg * kPi / 180.0; }
double Deg(double rad) { return rad * 180.0 / kPi; }

// tan of half the width of the view for a game angle (the one it uses on a 4:3 set) in a picture of this shape.
// share: the part of the picture's width one view has (1, or 0.5 for two views side by side).
double HalfWidth(double angle, double aspect, double share = 1.0) {
    double t43 = std::tan(Rad(std::min(angle, kMaxAngle)) / 2);
    double tv43 = t43 * kShape43;
    double tv169 = std::tan(Rad(std::min(angle * kFourThree, kMaxAngle)) / 2) * kShapeWide;
    aspect *= share;
    // A view narrower than 4:3 because it shares the picture: half way between keeping the 4:3 width (everything
    // small) and keeping the 4:3 height (a narrow look ahead).
    if (share < 1.0 && aspect < kFourThree) return tv43 * std::sqrt(kFourThree * aspect);
    if (aspect <= kFourThree) return tv43 * kFourThree;
    if (aspect >= kSixteenNine) return tv169 * aspect;
    double k = std::log(aspect / kFourThree) / std::log(kSixteenNine / kFourThree);
    return tv43 * std::pow(tv169 / tv43, k) * aspect;
}

// the game angle that gives this half width in a picture of this shape (HalfWidth rises with the angle)
double GameAngle(double half_width, double aspect, double share = 1.0) {
    double lo = 0.5, hi = kMaxAngle;
    for (int i = 0; i < 48; i++) {
        double mid = (lo + hi) / 2;
        if (HalfWidth(mid, aspect, share) < half_width) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

std::atomic<float> g_applied{0};  // the shape the cameras were last set up for
std::atomic<float> g_share{1.0f};         // the part of the picture's width one view has now
std::atomic<float> g_applied_share{1.0f};  // ... that the cameras were last set up for
}  // namespace

// co-op: two views side by side (each has half the picture's width), or one
void SetSplit(bool on) { g_share = on ? 0.5f : 1.0f; }

// the Display Settings page's "Picture shape": width * 1000 / height, 0 for the output's own shape (settings.toml)
uint32_t g_shape_setting = 0;
}  // namespace game_view
namespace game_coop {
extern uint32_t g_mode_setting;  // 0 off, 1 split screen (coop.cpp)
}
namespace game_view {

namespace {
float Wanted() {
    static const float forced = [] {
        const char* v = std::getenv("S2_ASPECT");
        return v ? float(std::atof(v)) : 0.0f;
    }();
    float a = forced;
    bool split = game_coop::g_mode_setting == 1;
    if (a <= 0 && g_shape_setting && !split) a = float(g_shape_setting) / 1000.0f;
#ifdef _WIN32
    if (a <= 0 && split) {  // (the window's picture area; not while it is minimized)
        HWND hwnd = static_cast<HWND>(app::Window());
        RECT rc{};
        if (hwnd && !IsIconic(hwnd) && GetClientRect(hwnd, &rc) && rc.right - rc.left >= 64 && rc.bottom - rc.top >= 64)
            a = float(rc.right - rc.left) / float(rc.bottom - rc.top);
    }
#endif
    if (a <= 0) {
        app::Mode m = app::Output();
        a = m.height ? float(m.width) / float(m.height) : float(kSixteenNine);
    }
    return std::clamp(a, 1.0f, 4.0f);
}

float ReadF(uint32_t at) {
    float f;
    std::memcpy(&f, kernel::HostPtr(at), 4);
    return f;
}
void WriteF(uint32_t at, float f) { std::memcpy(kernel::HostPtr(at), &f, 4); }
}  // namespace

// width / height of the picture the game's frame is shown in
float Aspect() {
    float a = g_applied.load();
    return a > 0 ? a : Wanted();
}

// the angle the game should set for one it would use on a 4:3 set, in degrees
float Angle(float game_angle) { return float(Deg(2 * std::atan(HalfWidth(game_angle, Aspect(), g_applied_share.load())))); }

// Once a frame, on the game's thread. The first time, and whenever the shape changes (another resolution, another
// window size, another setting), the frame is shown in the new shape and the cameras that exist are worked over:
// their angles were set for the old shape.
void Frame(X86Context& ctx) {
    constexpr uint32_t kView = 0x3934DC, kCameras = 0x40, kCount = 0x48, kStartAngle = 0x54, kCameraSize = 0x2C4;
    constexpr uint32_t kDistance = 0x280, kDistanceToo = 0x274, kX = 0x29C, kY = 0x2A0, kWidth = 0x2A4, kHeight = 0x2A8;
    constexpr double kHalf = 0.3125;
    float want = Wanted(), old = g_applied.load();
    float share = g_share.load(), old_share = g_applied_share.load();
    if (std::fabs(want - old) < 0.002f && share == old_share) return;
    g_applied = want;
    g_applied_share = share;
    app::g_frame_aspect = want;
    // the 2D layer (menus, HUD, text, movies) is laid out for a 4:3 picture and keeps that shape, in the middle
    gpu::g_flat2d.sx = std::min(1.0f, float(kFourThree) / want);
    gpu::g_flat2d.sy = std::min(1.0f, want / float(kFourThree));
    gpu::g_flat2d.on = std::fabs(want - float(kFourThree)) > 0.004f;
    // ... and in one of two views side by side, the same for a picture half as wide
    gpu::g_flat2d.half_sx = std::min(1.0f, float(kFourThree) / (want * 0.5f));
    gpu::g_flat2d.half_sy = std::min(1.0f, want * 0.5f / float(kFourThree));
    // the 640 x 480 frame is stretched sideways to the picture: draw it large enough that no column is made up
    // (S2_RENDER_BOOST=<factor> sets another factor, 1 for none)
    static const float forced_boost = [] {
        const char* v = std::getenv("S2_RENDER_BOOST");
        return v ? float(std::atof(v)) : 0.0f;
    }();
    app::g_render_boost = forced_boost > 0 ? forced_boost : std::max(1.0f, want / float(kFourThree));
    uint32_t view = kernel::R32(kView);
    if (old <= 0 || !view) {
        LOGI("view: picture shape %.3f", want);
        return;
    }
    auto convert = [&](double half_width) { return HalfWidth(GameAngle(half_width, old, old_share), want, share); };
    float start = ReadF(view + kStartAngle);
    if (start > 1 && start < 179) WriteF(view + kStartAngle, float(Deg(2 * std::atan(convert(std::tan(Rad(start) / 2))))));
    uint32_t cameras = kernel::R32(view + kCameras), count = kernel::R32(view + kCount);
    uint32_t ecx = ctx.ecx;
    for (uint32_t i = 0; cameras && i < count && i < 16; i++) {
        uint32_t cam = cameras + i * kCameraSize;
        // a camera that draws into a texture of its own (a monitor's, the theatre's projector in 03b) is not the
        // screen's: its angle is left as the game has it (2026-11-28: it was widened or narrowed again at every
        // change between one view and two, and the doctor on the theatre's screen fell out of its picture)
        uint32_t texture = kernel::R32(cam + 0x04);
        if (i > 0 && texture && texture != game_coop::g_no_texture) continue;
        // Player 1's camera (the first) going from two views into one for a scene: its angles are kept, and when
        // the two views come back they are put back as they were (the owner, 2026-11-28: "after some cutscenes,
        // player 1's camera gets zoomed in": a scene sets the camera's angle itself, and the change back was worked
        // from the scene's angle, not from the one the game had before)
        static float kept[2] = {0, 0};
        static bool have_kept = false;
        bool into_one = old_share < 1 && share >= 1 && std::fabs(want - old) < 0.002f;
        bool back_to_two = old_share >= 1 && share < 1 && std::fabs(want - old) < 0.002f;
        if (i == 0 && into_one) kept[0] = ReadF(cam + kDistance), kept[1] = ReadF(cam + kDistanceToo), have_kept = true;
        if (i == 0 && back_to_two && have_kept && kept[0] > 1e-4f) {
            WriteF(cam + kDistance, kept[0]), WriteF(cam + kDistanceToo, kept[1]);
            have_kept = false;
        } else {
            if (i == 0 && !into_one) have_kept = false;
            for (uint32_t field : {kDistance, kDistanceToo}) {
                float d = ReadF(cam + field);
                if (d > 1e-4f) WriteF(cam + field, float(kHalf / convert(kHalf / d)));
            }
        }
        // method 0 (x, y, width, height) works the rest out again from the distance and the shape
        ctx.ecx = cam;
        kernel::CallGuest(ctx, kernel::R32(kernel::R32(cam)),
                          {kernel::R32(cam + kX), kernel::R32(cam + kY), kernel::R32(cam + kWidth), kernel::R32(cam + kHeight)});
    }
    ctx.ecx = ecx;
    LOGI("view: picture shape %.3f (was %.3f), %s, %u cameras set up again", want, old, share < 1 ? "two views side by side" : "one view", count);
}
}  // namespace game_view

// The display's shape call, 0x2031D0(width, height), see the top of the file. For the whole 640 x 480 frame it
// answers height / width of the picture as shown; a camera with a view of its own shape gets its share of that.
X86_FUNC(sub_002031D0) {
    (void)base;
    float w, h;
    std::memcpy(&w, kernel::HostPtr(ctx.esp + 4), 4);
    std::memcpy(&h, kernel::HostPtr(ctx.esp + 8), 4);
    double shape = w > 0 ? double(h) / double(w) * (640.0 / 480.0) / double(game_view::Aspect()) : 1.0;
    x87_push(ctx, shape);
    kernel::Return(ctx, ctx.eax, 2);
}
