// Co-op, step 1: probes. Nothing here changes the game unless a switch is set.
//
// What is known of the engine from the PS2 build (work\COOP_STORY_DESIGN.md, work\NOTES.md) and found again here:
//   - Class factory 0x16F3C0 (this = registry, object, class id): Player is class 0xC8BC44E1, 0x890 bytes,
//     constructor 0x1F620(this, object), methods 0x3381D0.
//   - Player methods (0x3381D0): +0x30 = 0x21820 is the largest (663 instructions).
//
//   - Spawn: 0x18C370(spawner object, matrix, parameters or 0, template index), 201 callers; it calls the creation
//     core 0x18C1B0 on the object manager 0x394678. An engine object: methods 0x348D98, a 4 x 4 matrix at +0x14
//     (right, up, forward, position at +0x44, metres), its class object at +0x60, its template index in the low
//     word of +0xE4. The one Player is made at start-up from template 3291 into an object that already exists.
//   - Player methods that run once a frame: +0x18 = 0x25480 and +0x20 = 0x24B00; +0x24 = 0x1B0D0 runs several
//     times a frame.
//
// S2_COOP_SPAWN=<frame>: at that frame (counted from the first one drawn; 1200 is in play for a level started
// with S2_LEVEL) a second object is spawned from Player 1's template one metre to his right. Probe only.
//
// S2_COOP_TRACE=1: logs each Player the factory makes with the game's call stack at that moment, and every two
// seconds how often each of the Player's larger methods ran and for which objects. This is how the creation path
// and the once-a-frame methods are found.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include "gpu/backend.h"

namespace dispatch {
bool InCode(uint32_t va);  // cpu/dispatch.cpp
}
namespace game_view {
void SetSplit(bool on);  // view.cpp
}
namespace game_net {  // net.cpp
bool Host(bool on);
bool Hosting();
bool HostHasPlayer();
}

namespace game_coop {
namespace {
constexpr uint32_t kPlayerClass = 0xC8BC44E1;

bool Trace() {
    static const bool on = std::getenv("S2_COOP_TRACE") != nullptr;
    return on;
}

// the code addresses on the game's stack, nearest first: its return addresses (and a stale one now and then)
std::string Callers(X86Context& ctx, int most = 14) {
    std::string out;
    char one[16];
    for (uint32_t i = 0; i < 3000 && most > 0; i++) {
        uint32_t at = ctx.esp + i * 4;
        if (!mem::IsCommitted(at)) break;
        uint32_t v = kernel::R32(at);
        if (!dispatch::InCode(v)) continue;
        // a return address: the bytes before it are a call (E8 rel32; FF /2 with a register, a short or a long offset)
        const uint8_t* c = static_cast<const uint8_t*>(kernel::HostPtr(v - 6));
        bool call = c[1] == 0xE8 || (c[4] == 0xFF && (c[5] & 0xF8) == 0x10) || (c[3] == 0xFF && (c[4] & 0xF8) == 0x50) ||
                    (c[0] == 0xFF && ((c[1] & 0xF8) == 0x90 || c[1] == 0x15));
        if (!call) continue;
        std::snprintf(one, sizeof one, "%s%06X", out.empty() ? "" : " ", v);
        out += one;
        most--;
    }
    return out;
}

struct Seen {
    uint32_t calls = 0;
    std::map<uint32_t, uint32_t> by_object;
};
std::map<uint32_t, Seen> g_methods;
uint64_t g_frames = 0;

}  // namespace

std::map<uint32_t, uint32_t> g_class_of;  // trace: class object -> class id
uint32_t g_running = 0;  // the Player whose once-a-frame method is running now (0: none)

// trace: who asks the control manager for a value, for which input user, inside which Player's method
void Asked(X86Context& ctx, uint32_t getter) {
    if (!Trace()) return;
    static std::map<uint64_t, uint32_t> seen;
    uint32_t from = kernel::R32(ctx.esp), user = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8);
    uint64_t key = (uint64_t(from ^ (action << 20)) << 32) | (user ^ (g_running * 31));
    if (seen[key]++ || seen.size() > 900) return;
    LOGI("coop trace: getter %06X for user %08X (index %u), action %X, inside the method of Player %08X, called from %s", getter, user,
         mem::IsCommitted(user + 4) ? kernel::R32(user + 4) : 99, action, g_running, Callers(ctx, 6).c_str());
}

std::string CallersOf(X86Context& ctx) { return Callers(ctx, 8); }
extern bool g_layer_inside;

// trace: which of the game's globals a Player's once-a-frame method writes. What Player 2's Run writes there is
// state the game keeps once, for "the player": each is something the two players share without meaning to.
constexpr uint32_t kGlobalsFrom = 0x384000, kGlobalsTo = 0x3B9000;
struct Written {
    uint32_t times = 0, before = 0, after = 0;
};
std::map<uint64_t, Written> g_written;  // (method, which player, address)
struct GlobalsWatch {
    std::vector<uint8_t> before;
    uint32_t method, who;
    GlobalsWatch(uint32_t method_, uint32_t who_, bool on) : method(method_), who(who_) {
        if (!on) return;
        const uint8_t* at = static_cast<const uint8_t*>(kernel::HostPtr(kGlobalsFrom));
        before.assign(at, at + (kGlobalsTo - kGlobalsFrom));
    }
    ~GlobalsWatch() {
        if (before.empty()) return;
        const uint8_t* now = static_cast<const uint8_t*>(kernel::HostPtr(kGlobalsFrom));
        for (uint32_t o = 0; o + 4 <= before.size(); o += 4) {
            uint32_t a, b;
            std::memcpy(&a, &before[o], 4);
            std::memcpy(&b, now + o, 4);
            if (a == b) continue;
            Written& w = g_written[(uint64_t(method) << 40) | (uint64_t(who) << 32) | (kGlobalsFrom + o)];
            w.times++;
            w.before = a;
            w.after = b;
        }
    }
};
bool g_watch_globals = false;  // set for a few frames by Frame

void Count(uint32_t method, uint32_t self) {
    if (!Trace()) return;
    Seen& s = g_methods[method];
    s.calls++;
    s.by_object[self]++;
}

constexpr uint32_t kSpawn = 0x18C370;
constexpr uint32_t kMatrix = 0x14, kPosition = 0x44, kClassObject = 0x60, kTemplateWord = 0xE4;

uint32_t g_player = 0, g_player_object = 0;  // the first Player the factory made, and its engine object
uint32_t g_second_object = 0, g_second = 0;  // the probe's second Player: engine object and Player
uint32_t g_manager = 0;                      // the control manager, seen in its lock check

// The walk stick is not read by the Player. One object (0x3932F8, an input user of its own: methods, controller
// index, the two action pairs 3F7/3F8 and 3FE/3FF, then the stick now and before and its smoothing) reads it once
// a frame (0x17F130), and every Player takes his walk from it. Player 2 gets a copy of that object that reads
// controller 2; it stands in for the game's own while his once-a-frame methods run.
constexpr uint32_t kMoveStick = 0x3932F8, kMoveStickBytes = 0x34, kMoveStickUpdate = 0x17F130;
constexpr uint32_t kManagerPointer = 0x384BC8, kManagerMap = 0x6C;  // control manager: player slot -> controller
uint8_t g_stick2[kMoveStickBytes];
bool g_stick2_updating = false;
int g_stick2_in = 0;

void SwapStick() {
    uint8_t t[kMoveStickBytes];
    std::memcpy(t, kernel::HostPtr(kMoveStick), kMoveStickBytes);
    std::memcpy(kernel::HostPtr(kMoveStick), g_stick2, kMoveStickBytes);
    std::memcpy(g_stick2, t, kMoveStickBytes);
}

// ---- the second view -------------------------------------------------------------------------------------------
// The engine draws a list of view cameras each frame (view.cpp: the view object [0x3934DC], cameras of 0x2C4 bytes
// at +0x40, count +0x48, current +0x4C), from the last to the first (0x1B83E0). The first is the screen. The
// others belong to Camera objects that show on a monitor in the level: such a view camera has a texture at +0x04
// and its Camera's engine object at +0xA0, whose matrix it takes; the renderer is pointed at the texture
// (0x1FB080), and when that fails the engine draws the camera into the current target with the camera's own
// rectangle and clear. That is the split screen: a view camera for the right half whose "texture" always fails.
//
// What follows the player is a Camera object too (class 0x6BB9D081, template 269; once a frame 0x108060): in mode
// 3 it works its matrix out from the object it tracks (handle at +0x38, set with 0x103740; by default the first
// listed player). Its set-up (0x105970) makes it the game's camera ([0x3947A8]), ties it to the last view camera
// and makes it the place sound is heard from. Player 2 gets a second one, spawned like him, tracking him; the
// first stays the game's camera.
constexpr uint32_t kViewPointer = 0x3934DC, kViewCameras = 0x40, kViewCount = 0x48, kViewCurrent = 0x4C, kViewCameraBytes = 0x2C4;
constexpr uint32_t kViewMatrixPointer = 0x3934D4;  // the current view camera's matrix part (camera + 8)
constexpr uint32_t kAddViewCamera = 0x1A6C80;      // (this = view, x, y, width, height) -> the view camera
constexpr uint32_t kVcTexture = 0x04, kVcObject = 0xA0;
constexpr uint32_t kGameCamera = 0x3947A8;         // the engine object of the game's Camera
constexpr uint32_t kObjectManager = 0x394678, kManagerState = 0x134;
constexpr uint32_t kCameraTrack = 0x103740;        // (this = Camera, object to follow)
constexpr uint32_t kSoundPointer = 0x384BBC, kSetListener = 0x1A2690;  // (this, object sound is heard from)
uint32_t g_camera2_object = 0;  // Player 2's Camera: its engine object
uint32_t g_no_texture = 0;      // the stand-in texture of Player 2's view camera
extern const char* g_scene_why;
bool g_one_view = false;        // a scene: the picture is one view, Player 1's (see Views)
// Online, Player 2 is somebody else's: his view is not for this screen. Both view cameras then have the whole frame.
// The engine draws the cameras last to first, each clearing its rectangle: Player 2's view is drawn, the runtime
// takes the frame as it is then for the stream (gpu::SnapshotForStream at the end of his pass), and Player 1's
// view is drawn over it. The host plays on a whole screen and the joiner gets a whole picture.
bool g_stream_view = false;
extern uint64_t g_down_until;
extern std::map<uint32_t, int> g_hurt_us;
void Downs(X86Context& ctx);
bool g_screen_pass = false;     // the engine is drawing Player 2's view camera

// Player 2's view camera, wherever it is in the list now (0: none)
uint32_t ViewCamera2() {
    uint32_t view = kernel::R32(kViewPointer);
    if (!view || !g_no_texture) return 0;
    uint32_t cameras = kernel::R32(view + kViewCameras), count = kernel::R32(view + kViewCount);
    for (uint32_t i = 1; cameras && i < count && i < 16; i++)
        if (kernel::R32(cameras + i * kViewCameraBytes + kVcTexture) == g_no_texture) return cameras + i * kViewCameraBytes;
    return 0;
}

// The two view cameras' rectangles for the state now: side by side; one view (a scene: Player 2's is left out, its
// texture's word says "nothing shows it"); or both the whole frame (online, see g_stream_view).
void Rects(X86Context& ctx) {
    uint32_t view = kernel::R32(kViewPointer), second = ViewCamera2();
    if (!view || !second) return;
    uint32_t w = kernel::R32(view + 0xA4), h = kernel::R32(view + 0xA8);
    bool whole = g_one_view || g_stream_view;
    uint32_t first = kernel::R32(view + kViewCameras), ecx = ctx.ecx;
    ctx.ecx = first;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(first)), {0, 0, whole ? w : w / 2, h});
    ctx.ecx = second;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(second)), {g_stream_view ? 0 : w / 2, 0, g_stream_view ? w : w / 2, h});
    ctx.ecx = ecx;
    kernel::W16(g_no_texture + 8, g_one_view ? 1 : 2);
    game_view::SetSplit(!whole);
    gpu::g_stream_snapshot = g_stream_view && !g_one_view;
    if (g_one_view) LOGI("coop: one view because: %s", g_scene_why);
    LOGI("coop: views: %s", g_one_view ? "one (a scene)" : g_stream_view ? "Player 2's for the stream, Player 1's on the screen" : "side by side");
}

void SplitOn(X86Context& ctx) {
    uint32_t view = kernel::R32(kViewPointer), camera1 = kernel::R32(kGameCamera);
    if (!view || !camera1 || !g_second_object || ViewCamera2()) return;
    uint32_t ecx = ctx.ecx;
    if (!g_no_texture) {
        g_no_texture = mem::AllocSystem(0x80);
        std::memset(kernel::HostPtr(g_no_texture), 0, 0x80);
        kernel::W16(g_no_texture + 8, 2);  // "in use": the engine skips a view camera whose texture nothing shows
    }
    // Player 1's view: the left half
    uint32_t first = kernel::R32(view + kViewCameras);
    ctx.ecx = first;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(first)), {0, 0, 320, 480});
    // Player 2's view: the right half
    ctx.ecx = view;
    uint32_t second = kernel::CallGuest(ctx, kAddViewCamera, {320, 0, 320, 480});
    kernel::W32(second + kVcTexture, g_no_texture);
    // his Camera, beside the game's
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(camera1 + kMatrix), 64);
    uint32_t tmpl = kernel::R32(camera1 + kTemplateWord) & 0xFFFF;
    // (The Camera's template is marked 0x400, "not made again while the level runs": the object manager's state
    // bit 2 at +0x134 says the level runs. Cleared for this one spawn.)
    uint32_t state = kernel::R32(kObjectManager + kManagerState);
    kernel::W32(kObjectManager + kManagerState, state & ~2u);
    g_camera2_object = kernel::CallGuest(ctx, kSpawn, {g_second_object, matrix, 0, tmpl});
    kernel::W32(kObjectManager + kManagerState, (kernel::R32(kObjectManager + kManagerState) & ~2u) | (state & 2));
    uint32_t camera2 = g_camera2_object ? kernel::R32(g_camera2_object + kClassObject) : 0;
    LOGI("coop: view camera %08X for the right half; Camera object %08X (class object %08X) from template %u; the game's camera %08X, now %08X",
         second, g_camera2_object, camera2, tmpl, camera1, kernel::R32(kGameCamera));
    // what its set-up took goes back to the game's camera
    kernel::W32(kGameCamera, camera1);
    second = ViewCamera2();
    if (second) kernel::W32(second + kVcObject, g_camera2_object);
    ctx.ecx = kernel::R32(kSoundPointer);
    if (ctx.ecx) kernel::CallGuest(ctx, kSetListener, {camera1});
    if (camera2) {
        ctx.ecx = camera2;
        kernel::CallGuest(ctx, kCameraTrack, {g_second_object});
    }
    ctx.ecx = ecx;
    g_one_view = false;
    Rects(ctx);
    logx::Flush();
}

// "The player", to 162 functions of the game, is the first in the object manager's list of players (+0x124, count
// +0x12C). While Player 2's own methods run he is the first: what his update does to "the player" (the aim along
// the camera, for one: with the list left alone Player 1 raised his gun wherever Player 2 looked) is done to him.
constexpr uint32_t kPlayerList = 0x124, kPlayerCount = 0x12C;
uint32_t ListSecondFirst() {  // -> his place in the list (0: not moved)
    uint32_t list = kernel::R32(kObjectManager + kPlayerList), count = kernel::R32(kObjectManager + kPlayerCount);
    for (uint32_t i = 1; list && i < count && i < 8; i++) {
        if (kernel::R32(list + i * 4) != g_second_object) continue;
        kernel::W32(list + i * 4, kernel::R32(list));
        kernel::W32(list, g_second_object);
        return i;
    }
    return 0;
}
void ListSecondBack(uint32_t place) {
    uint32_t list = kernel::R32(kObjectManager + kPlayerList), count = kernel::R32(kObjectManager + kPlayerCount);
    if (!place || !list || place >= count || kernel::R32(list) != g_second_object) return;
    kernel::W32(list, kernel::R32(list + place * 4));
    kernel::W32(list + place * 4, g_second_object);
}

// Player 2's once-a-frame methods run with his own walk stick, as "the player", and with his view camera as the
// current one (the Player sets the view's angle and asks it where things are on the screen)
struct AsSecond {
    bool on;
    uint32_t current = 0, matrix = 0, camera = 0, listed = 0;
    explicit AsSecond(uint32_t player) : on(g_second && player == g_second) {
        if (!on || g_stick2_in++) return;
        SwapStick();
        listed = ListSecondFirst();
        uint32_t view = kernel::R32(kViewPointer), mine = ViewCamera2();
        static const bool noswap = std::getenv("S2_COOP_NOCAMSWAP") != nullptr;  // bisecting
        if (!mine || !g_camera2_object || noswap) return;
        // "the game's camera" is his Camera meanwhile: the Player switches it off and on and points it at things
        camera = kernel::R32(kGameCamera);
        kernel::W32(kGameCamera, g_camera2_object);
        current = kernel::R32(view + kViewCurrent);
        matrix = kernel::R32(kViewMatrixPointer);
        kernel::W32(view + kViewCurrent, (mine - kernel::R32(view + kViewCameras)) / kViewCameraBytes);
        kernel::W32(kViewMatrixPointer, mine + 8);
    }
    ~AsSecond() {
        if (!on || --g_stick2_in) return;
        SwapStick();
        ListSecondBack(listed);
        if (!matrix) return;
        kernel::W32(kGameCamera, camera);
        kernel::W32(kernel::R32(kViewPointer) + kViewCurrent, current);
        kernel::W32(kViewMatrixPointer, matrix);
    }
};
constexpr uint32_t kUser = 0x3A, kUserDevice = 0x3E;  // the Player's input user part, and its controller index


float ReadF(uint32_t at) {
    float f;
    std::memcpy(&f, kernel::HostPtr(at), 4);
    return f;
}

// Player 2 is never saved (the rule of 2026-11-23, after the Silent Hill 2 co-op mod). The game saves in one place,
// 0x1B4D50 (this = the game; three callers: the player's own save 0x1BD8BD, the save at a level's start 0x1BCD50,
// and 0x1BC860), and the first thing it does there is free the objects that have been ended (0x18DDB0). So he and
// his Camera are ended right before it, and are not in the save. He is made again where he stood, with the health
// he had, as soon as the game runs again. (A save made with him in it brought him back as a third Torque nobody
// plays: the save's own second Player, and the one made for the co-op session.)
struct Return {
    bool valid = false;
    uint8_t matrix[64] = {};
    float health = 0;
};
Return g_return;

void SpawnSecond(X86Context& ctx) {
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
    float pos[3];
    for (int i = 0; i < 3; i++) pos[i] = ReadF(g_player_object + kPosition + i * 4) + ReadF(g_player_object + kMatrix + i * 4);  // + right
    std::memcpy(kernel::HostPtr(matrix + 0x30), pos, 12);
    bool back = g_return.valid;
    if (back) {  // back where he stood when the game saved
        std::memcpy(kernel::HostPtr(matrix), g_return.matrix, 64);
        std::memcpy(pos, g_return.matrix + 0x30, 12);
    }
    uint32_t tmpl = kernel::R32(g_player_object + kTemplateWord) & 0xFFFF;
    LOGI("coop: spawning a second object from template %u at %.2f %.2f %.2f", tmpl, pos[0], pos[1], pos[2]);
    logx::Flush();
    g_second_object = kernel::CallGuest(ctx, kSpawn, {g_player_object, matrix, 0, tmpl});
    g_second = g_second_object ? kernel::R32(g_second_object + kClassObject) : 0;
    LOGI("coop: second object %08X, its class object %08X", g_second_object, g_second);
    // A Player is an input user (the part at +0x3A: methods, then the controller it reads at +0x3E). Player 2
    // reads controller 2.
    if (g_second) {
        kernel::W32(g_second + kUserDevice, 1);
        std::memcpy(g_stick2, kernel::HostPtr(kMoveStick), kMoveStickBytes);
        std::memset(g_stick2 + 0x18, 0, kMoveStickBytes - 0x18);
        uint32_t one = 1;
        std::memcpy(g_stick2 + 4, &one, 4);
        if (back && g_return.health > 0) std::memcpy(kernel::HostPtr(g_second + 0x16), &g_return.health, 4);
    }
    g_return.valid = false;
    logx::Flush();
}

void Dump(const char* what, uint32_t at, uint32_t bytes) {
    for (uint32_t o = 0; o < bytes; o += 32) {
        char line[200];
        int n = std::snprintf(line, sizeof line, "coop trace: %s +%03X:", what, o);
        for (uint32_t i = 0; i < 32 && o + i < bytes; i += 4) n += std::snprintf(line + n, sizeof line - n, " %08X", kernel::R32(at + o + i));
        LOGI("%s", line);
    }
}

// ---- the session ---------------------------------------------------------------------------------------------------
// Co-op is a setting ("coop" in the settings file, the Co-op row of the Display and Co-op page; 0 off, 1 split
// screen on this PC). While it is on and a level is in play, Player 2 is in: spawned beside Player 1 with his view and Camera, and
// again after every level change or load, which end the level's objects. Each frame the players are found again
// from the object manager's list, so nothing here outlives the object it points at.
//   S2_COOP=1             on, whatever the setting (tests)
//   S2_COOP_SPAWN=<frame> on from that frame (tests with timed controller scripts)
uint32_t g_mode_setting = 0;  // 0 off, 1 split screen on this PC
// Host online (a joiner is sent Player 2's view): the menu's choice for this run. It is not kept in the settings
// file: a game that opens a port on the router and looks its address up each time it starts, because of a choice
// made weeks ago, is not what a player expects.
bool g_host_setting = false;
bool g_wanted = false;  // co-op is on now (the setting, or a test's switch)
bool g_host_failed = false;  // hosting could not start (the port is taken): not tried again until the setting changes

// net.cpp asks and tells
void RemotePlayer(bool connected) { LOGI("coop: the remote player %s", connected ? "is in" : "is gone"); }
bool SplitActive() { return g_second_object && ViewCamera2() && !g_one_view && !g_stream_view; }
bool StreamView() { return g_second_object && ViewCamera2() && !g_one_view && g_stream_view; }
constexpr uint32_t kPlayerMethods = 0x3381D0;
constexpr uint32_t kObjectEnd = 0x1849B0;  // (this = engine object)

bool IsPlayerObject(uint32_t object) {
    if (!object || !mem::IsCommitted(object + 0x110)) return false;
    uint32_t player = kernel::R32(object + kClassObject);
    return player && mem::IsCommitted(player + 0x88F) && kernel::R32(player) == kPlayerMethods && kernel::R32(player + 4) == object;
}

// Player 2 and everything made for him is forgotten (the level ended them), and Player 1 has the whole screen again
void SecondGone(X86Context& ctx, const char* why) {
    LOGI("coop: Player 2 is out (%s)", why);
    uint32_t view = kernel::R32(kViewPointer);
    if (view && ViewCamera2() && g_camera2_object) {  // his view camera: the engine's own way out (by its object)
        uint32_t ecx = ctx.ecx;
        ctx.ecx = view;
        kernel::CallGuest(ctx, 0x1A6E10, {g_camera2_object});
        ctx.ecx = ecx;
    }
    g_second = g_second_object = g_camera2_object = 0;
    g_down_until = 0;
    g_one_view = false;
    game_view::SetSplit(false);
    gpu::g_stream_snapshot = false;
    uint32_t first = view ? kernel::R32(view + kViewCameras) : 0;
    if (first && kernel::R32(view + kViewCount) && kernel::R32(first + 0x2A4) != kernel::R32(view + 0xA4)) {
        uint32_t ecx = ctx.ecx;
        ctx.ecx = first;
        kernel::CallGuest(ctx, kernel::R32(kernel::R32(first)), {0, 0, kernel::R32(view + 0xA4), kernel::R32(view + 0xA8)});
        ctx.ecx = ecx;
    }
    logx::Flush();
}

void SpawnSecond(X86Context& ctx);
void SplitOn(X86Context& ctx);

// His Camera and he are ended the engine's way (0x1849B0, this = engine object: marks it ended, frees its place in
// the object table and queues it; the object manager frees it after the objects' run, or the game's save does
// before it writes, which ends its class object too and takes a Player off the list).
void EndSecond(X86Context& ctx, const char* why) {
    uint32_t ecx = ctx.ecx;
    for (uint32_t object : {g_camera2_object, g_second_object}) {
        if (!object || int32_t(kernel::R32(object + 8)) < 0) continue;
        ctx.ecx = object;
        kernel::CallGuest(ctx, kObjectEnd, {});
    }
    ctx.ecx = ecx;
    SecondGone(ctx, why);
}

// right before the game saves (the hook on 0x1B4D50)
void BeforeSave(X86Context& ctx) {
    if (!g_second_object || !IsPlayerObject(g_second_object)) return;
    g_return.valid = true;
    std::memcpy(g_return.matrix, kernel::HostPtr(g_second_object + kMatrix), 64);
    g_return.health = g_down_until ? 0.0f : ReadF(g_second + 0x16);
    EndSecond(ctx, "the game is saving: he is not saved");
}

// Player 2 comes in with a copy of what Player 1 carries. A Player's inventory is the list at +0x128 (12 bytes an
// entry: ?, item, how many; a second list of items at +0x0C of it): 0x11380 (this = inventory, another) makes one
// a copy of another, 0x110A0 (item) how many, 0x11220 (item, n) takes away. Then the weapon Player 1 holds is put
// in his hands the way the weapon button does it (0x171F0: 0x17050 picks the next one from the inventory, going on
// from the one held, so it finds none for empty hands; 0x14F30 and 0x16320 put it in his hands).
// (Everything is copied for now, quest items too: the Player 1 only rule for those is still to come.)
constexpr uint32_t kInventory = 0x128, kInventoryCopy = 0x11380;
constexpr uint32_t kEquip = 0x14F30, kEquipped = 0x16320;  // (this = Player, weapon's template, slot): as 0x171F0 calls them
uint64_t g_arm_at = 0;
void Arm(X86Context& ctx) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = g_second + kInventory;
    kernel::CallGuest(ctx, kInventoryCopy, {g_player + kInventory});
    {
        // the weapon Player 1 holds (the object in his right hand, handle at +0x3AC; an item is its template's number)
        uint32_t table = kernel::R32(kObjectManager + 0x0C), handle = kernel::R32(g_player + 0x3AC) & 0xFFFF;
        uint32_t held = table && handle ? kernel::R32(table + handle * 6) : 0;
        if (held) {
            uint32_t weapon = kernel::R32(held + kTemplateWord) & 0xFFFF;
            AsSecond as(g_second);
            ctx.ecx = g_second;
            kernel::CallGuest(ctx, kEquip, {weapon, 1});
            ctx.ecx = g_second;
            kernel::CallGuest(ctx, kEquipped, {weapon, 1});
        }
    }
    // what "next weapon" would pick for each (0x17050: this = Player, template out, slot out)
    static uint32_t out = mem::AllocSystem(16);
    for (uint32_t player : {g_player, g_second}) {
        kernel::W32(out, 0xFFFFFFFF), kernel::W32(out + 4, 0xFFFFFFFF);
        ctx.ecx = player;
        uint32_t found = kernel::CallGuest(ctx, 0x17050, {out, out + 4});
        uint32_t table = kernel::R32(kObjectManager + 0x0C);
        uint32_t right = kernel::R32(player + 0x3AC) & 0xFFFF, left = kernel::R32(player + 0x3AE) & 0xFFFF;
        uint32_t ro = table && right ? kernel::R32(table + right * 6) : 0, lo = table && left ? kernel::R32(table + left * 6) : 0;
        std::string items;
        char one[40];
        uint32_t list = kernel::R32(player + kInventory), n = kernel::R32(player + kInventory + 8);
        for (uint32_t i = 0; i < n && i < 24; i++) {
            std::snprintf(one, sizeof one, " %X x%d", kernel::R32(list + i * 12 + 4), int32_t(kernel::R32(list + i * 12 + 8)));
            items += one;
        }
        LOGI("coop: Player %08X: in hands %04X (template %u) and %04X (template %u), last weapon %d, flags +B0 %08X +BC %08X, next weapon %s: template %d slot %d; carries%s",
             player, right, ro ? kernel::R32(ro + kTemplateWord) & 0xFFFF : 0, left, lo ? kernel::R32(lo + kTemplateWord) & 0xFFFF : 0,
             int32_t(kernel::R32(player + 0x854)), kernel::R32(player + 0xB0), kernel::R32(player + 0xBC), found ? "found" : "none",
             int32_t(kernel::R32(out)), int32_t(kernel::R32(out + 4)), items.c_str());
    }
    ctx.ecx = ecx;
    LOGI("coop: Player 2 has a copy of Player 1's inventory (%u entries)", kernel::R32(g_second + kInventory + 8));
}

// A scene: the game's Camera is not following Player 1 (the top of its mode stack is not 3: a script or a cut-scene
// has pushed a mode of its own; or it follows someone else). The picture is one view then, Player 1's, as in the Silent Hill 2 co-op mod; the two
// views come back when the Camera follows again. Player 2's view camera stays in the list and is skipped by the
// engine (its stand-in texture reads as shown by nothing).
const char* g_scene_why = "";  // which sign of a scene InScene saw last (for the log)
bool InScene() {
    uint32_t object = kernel::R32(kGameCamera);
    uint32_t camera = object && mem::IsCommitted(object + 0x110) ? kernel::R32(object + kClassObject) : 0;
    if (!camera || !mem::IsCommitted(camera + 0x40)) return g_scene_why = "no game camera", true;
    uint32_t stack = kernel::R32(camera + 0x20), count = kernel::R32(camera + 0x28);
    static uint32_t mode_seen = 0;
    uint32_t mode = stack && count && count <= 64 ? kernel::R32(stack + count * 8 - 4) : 99;
    if (mode != mode_seen) LOGI("coop: the game camera's mode is %u (was %u; %u on its stack)", mode, mode_seen, count), mode_seen = mode;
    // Mode 3 follows the Player; mode 0 leaves the camera to whoever moves it, and that is the Player himself in
    // first person (d-pad up puts a mode 0 on the stack). Until 2026-11-25 mode 0 counted as a scene: Player 1
    // looking through his own eyes took Player 2's view away and hid him. A scene that uses mode 0 shows by one of
    // the signs below.
    if (mode != 3 && mode != 0) return g_scene_why = "the camera is neither following nor the Player's own", true;
    // ... or a sequence moves it: bit 1 of its flags at +0x2C (its own once-a-frame method does nothing then)
    if (kernel::R32(camera + 0x2C) & 1) return g_scene_why = "a sequence moves the camera", true;
    // ... or it follows something other than Player 1 (a script points it at someone: the handle at +0x38 is a
    // place in the object manager's table, 6 bytes an entry, the object first)
    uint32_t table = kernel::R32(kObjectManager + 0x0C), handle = kernel::R32(camera + 0x38) & 0xFFFF;
    uint32_t tracked = table && handle ? kernel::R32(table + handle * 6) : 0;
    if (tracked && tracked != g_player_object) return g_scene_why = "the camera follows something else", true;
    // ... or Player 1 is in a scripted sequence: bits 0x40000 and 0x80000 of his flags at +0xBC (the HUD's getters
    // answer from their last values while one is set, and the HUD shows the black bars)
    // (Bit 0x80000 is not a sequence: it is the creature form, 0x2B230 sets it. Until 2026-11-25 it counted too, and
    // Player 2 lost his view and his controls for as long as Player 1 was transformed: run form1, 20 s. Bit 0x40000
    // is set while he changes into it and back, about half a second and three seconds.)
    if (g_player && (kernel::R32(g_player + 0xBC) & 0x40000)) return g_scene_why = "Player 1 is in a scripted sequence (or changing form)", true;
    // ... or Player 1 is carried off on something he is mounted on (bit 0x200000 of the same flags is set from the
    // moment he is on the truck's gun in chapter 10; run truck1: the truck drove him 250 m away and Player 2 stood
    // where the level began). A mount that stays where it is leaves Player 2 free: it only counts once he is more
    // than 15 m behind, and then until Player 1 is off the mount. He comes back beside Player 1 as after a scene.
    static bool riding = false;
    bool mounted = g_player && (kernel::R32(g_player + 0xBC) & 0x200000);
    if (!mounted || !g_second_object) riding = false;
    else if (!riding && g_player_object) {
        float d = 0;
        for (int i = 0; i < 3; i++) {
            float v = ReadF(g_player_object + kPosition + i * 4) - ReadF(g_second_object + kPosition + i * 4);
            d += v * v;
        }
        riding = d > 15.0f * 15.0f;
    }
    if (riding) return g_scene_why = "Player 1 is carried off on a mount (a vehicle)", true;
    return false;
}
// Engine objects: 0x183C70 (this = object, shown) shows or hides one with what is attached to it (bit 1 of the
// flags at +0x08); 0x184EB0 (this = object) hands out its matrix at +0x14 to be written and marks it moved.
constexpr uint32_t kObjectShow = 0x183C70;

// Player 2 beside Player 1: one metre along Player 1's right, facing the way he faces. Moved the game's own way,
// as its teleport pads move the player (0x13B6F0): 0x2ED90 (this = Player) ends what he is doing, 0x2F060 (this =
// Player, matrix) writes his object's matrix, his body's place in the physics ([Player +0xF0] +0x84) and his last
// position (+0x186). (Writing the matrix alone left the body behind: after an 11 m move he slid 2.5 m.)
constexpr uint32_t kPlayerStop = 0x2ED90, kPlayerTeleport = 0x2F060;
void BesideFirst(X86Context& ctx) {
    if (!g_second || !g_second_object || !g_player_object) return;
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
    for (int i = 0; i < 3; i++) {
        float v = ReadF(g_player_object + kPosition + i * 4) + ReadF(g_player_object + kMatrix + i * 4);
        std::memcpy(kernel::HostPtr(matrix + 0x30 + i * 4), &v, 4);
    }
    uint32_t ecx = ctx.ecx;
    ctx.ecx = g_second;
    kernel::CallGuest(ctx, kPlayerStop, {});
    ctx.ecx = g_second;
    kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
    ctx.ecx = ecx;
}

// Player 2 down. He is never left to die (the game's death is Player 1's: it ends the level): when his health
// falls to a quarter he goes down instead, out of sight and out of play for five seconds, and comes back beside
// Player 1 with full health. The same when he falls out of the level (more than 40 m under Player 1).
uint64_t g_down_until = 0;  // frame he comes back at (0: he is up)
bool Down() { return g_down_until != 0; }
void Downs(X86Context& ctx) {
    constexpr float kDownAt = 0.25f, kFallen = 40.0f, kDownSeconds = 5.0f;
    // He may not die: the game's own mark for that (+0x0E of anything with health: "take the amount off", 0x16AF00,
    // leaves such a one with 1). One hit for all he had killed him before this code ever saw a quarter: level 08
    // does that to whoever stands where he is made (200 at once), the game went through a Player's death with him,
    // and drawing him again after the scene crashed (a read of 0x2C in the model drawing, 0x1C2273).
    if (kernel::R32(g_second + 0x0E) != 1) kernel::W32(g_second + 0x0E, 1);
    // (in frames at the frame time now: [0x384B78] is the game's timer, its first float the frame's seconds. It was
    // 300 frames, which at 120 frames a second was two and a half seconds.)
    uint32_t timer = kernel::R32(0x384B78);
    const uint64_t kDownFrames = uint64_t(kDownSeconds / std::clamp(timer ? ReadF(timer) : 1.0f / 60, 1.0f / 480, 1.0f / 20));
    float most = ReadF(g_second + 0x1A), health = ReadF(g_second + 0x16);
    auto show = [&](bool shown) {
        uint32_t ecx = ctx.ecx;
        ctx.ecx = g_second_object;
        kernel::CallGuest(ctx, kObjectShow, {shown ? 1u : 0u});
        ctx.ecx = ecx;
    };
    if (!g_down_until) {
        bool fallen = g_player_object && ReadF(g_second_object + kPosition + 4) < ReadF(g_player_object + kPosition + 4) - kFallen;
        if (!(most > 0 && health <= most * kDownAt) && !fallen) return;
        g_down_until = g_frames + (fallen ? 1 : kDownFrames);
        LOGI("coop: Player 2 is down (%s; health %.0f of %.0f)", fallen ? "fell out of the level" : "health", health, most);
        show(false);
    }
    std::memcpy(kernel::HostPtr(g_second + 0x16), &most, 4);  // nothing hurts him meanwhile
    if (g_frames < g_down_until || g_one_view) return;
    g_down_until = 0;
    BesideFirst(ctx);
    show(true);
    LOGI("coop: Player 2 is back beside Player 1");
}

void Views(X86Context& ctx, bool one) {
    uint32_t view = kernel::R32(kViewPointer), second = ViewCamera2();
    if (!view || !second || one == g_one_view) return;
    g_one_view = one;
    // In a scene Player 2 is not to be seen; after it he is back beside Player 1.
    if (g_second_object && !Down()) {
        if (!one) BesideFirst(ctx);
        uint32_t ecx = ctx.ecx;
        ctx.ecx = g_second_object;
        kernel::CallGuest(ctx, kObjectShow, {one ? 0u : 1u});
        ctx.ecx = ecx;
    }
    Rects(ctx);
}

void Session(X86Context& ctx, bool wanted) {
    uint32_t list = kernel::R32(kObjectManager + kPlayerList), count = kernel::R32(kObjectManager + kPlayerCount);
    if (count > 8 || !list) count = 0;
    // Player 2 is still there when his object is still listed and still his
    if (g_second_object) {
        bool listed = false;
        for (uint32_t i = 0; i < count; i++) listed |= kernel::R32(list + i * 4) == g_second_object;
        if (!listed || !IsPlayerObject(g_second_object) || kernel::R32(g_second_object + kClassObject) != g_second) SecondGone(ctx, "his object ended");
    }
    // Player 1: the first listed Player that is not Player 2
    uint32_t first = 0;
    for (uint32_t i = 0; i < count && !first; i++) {
        uint32_t object = kernel::R32(list + i * 4);
        if (object != g_second_object && IsPlayerObject(object)) first = object;
    }
    if (first != g_player_object) {
        g_player_object = first;
        g_player = first ? kernel::R32(first + kClassObject) : 0;
        LOGI("coop: Player 1 is %08X (object %08X), %u listed", g_player, first, count);
        if (g_second_object) SecondGone(ctx, "another Player 1");
        g_return.valid = false;  // (another level, or a load: he comes in beside Player 1)
        g_hurt_us.clear();
    }
    static uint32_t state_before = ~0u;
    uint32_t state = kernel::R32(kObjectManager + kManagerState);
    if (Trace() && state != state_before) LOGI("coop trace: object manager state %X (was %X), %u players listed", state, state_before, count);
    state_before = state;
    // a level in play: a Player 1, the object manager says the level runs (state bit 2), for a second without a break
    static uint32_t steady = 0;
    steady = g_player && (state & 2) ? steady + 1 : 0;
    if (Trace()) {  // the game's Camera, whenever it or what it does changes
        static uint64_t before = 0;
        uint32_t object = kernel::R32(kGameCamera);
        uint32_t camera = object && mem::IsCommitted(object + 0x110) ? kernel::R32(object + kClassObject) : 0;
        uint32_t stack = camera ? kernel::R32(camera + 0x20) : 0, n = camera ? kernel::R32(camera + 0x28) : 0;
        uint32_t mode = stack && n && n < 64 ? kernel::R32(stack + n * 8 - 4) : 99, handle = camera ? kernel::R32(camera + 0x38) & 0xFFFF : 0;
        uint32_t table = kernel::R32(kObjectManager + 0x0C), tracked = table && handle ? kernel::R32(table + handle * 6) : 0;
        uint32_t view = kernel::R32(kViewPointer), vc = view ? kernel::R32(view + kViewCameras) : 0;
        uint32_t shown = vc ? kernel::R32(vc + kVcObject) : 0;
        uint32_t flags = g_player ? kernel::R32(g_player + 0xBC) : 0;
        static uint32_t flags_before = 0;
        if ((flags ^ flags_before) & 0xC0000) LOGI("coop trace: Player 1's flags at +0xBC: %08X (was %08X)", flags, flags_before);
        flags_before = flags;
        uint32_t cflags = camera ? kernel::R32(camera + 0x2C) : 0;
        static uint32_t cflags_before = 0;
        if (cflags != cflags_before) LOGI("coop trace: the game camera's flags at +0x2C: %08X (was %08X)", cflags, cflags_before);
        cflags_before = cflags;
        uint64_t now = (uint64_t(object) << 32) ^ (uint64_t(mode) << 56) ^ tracked ^ (uint64_t(shown) << 20);
        if (now != before)
            LOGI("coop trace: game camera object %08X (template %u) mode %u of %u, follows %08X (Player 1's object %08X); the screen's view camera shows object %08X",
                 object, object ? kernel::R32(object + kTemplateWord) & 0xFFFF : 0, mode, n, tracked, g_player_object, shown);
        before = now;
    }
    if (g_second_object) Downs(ctx);
    if (g_second_object) {
        // one view at once; two views again only after a quarter of a second without a scene (between two scenes
        // there can be a frame that is neither)
        static uint32_t clear = 0;
        // tests: S2_COOP_SCENE=<frame> is 3 s of "scene" from that frame (S2_COOP_SCENE_FRAMES=<n>: that long)
        static const uint64_t fake = std::getenv("S2_COOP_SCENE") ? uint64_t(std::atoll(std::getenv("S2_COOP_SCENE"))) : 0;
        static const uint64_t fake_long = std::getenv("S2_COOP_SCENE_FRAMES") ? uint64_t(std::atoll(std::getenv("S2_COOP_SCENE_FRAMES"))) : 180;
        clear = InScene() || (fake && g_frames >= fake && g_frames < fake + fake_long) ? 0 : clear + 1;
        if (!clear) Views(ctx, true);
        else if (clear > 15) Views(ctx, false);
    }
    if (!wanted && g_second_object) {  // switched off while he is in
        EndSecond(ctx, "co-op switched off");
        return;
    }
    if (!wanted) g_return.valid = false;
    // A Player that is listed and is neither Player 1 nor the one made here came out of a save that was made
    // with a Player 2 in it (before 2026-11-25 he was saved): he is taken out.
    if (!g_second_object && first && steady > 1) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t object = kernel::R32(list + i * 4);
            if (object == first || !IsPlayerObject(object) || int32_t(kernel::R32(object + 8)) < 0) continue;
            LOGR("coop: a second Player (object %08X) came out of the save: taken out", object);
            uint32_t ecx = ctx.ecx;
            ctx.ecx = object;
            kernel::CallGuest(ctx, kObjectEnd, {});
            ctx.ecx = ecx;
        }
    }
    // (after a save he is back as soon as the game's time runs: timer flag bit 0 is "stopped", the pause menu)
    uint32_t timer = kernel::R32(0x384B78);
    bool back_now = g_return.valid && steady > 1 && timer && !(kernel::R32(timer + 0xA0) & 1);
    if (wanted && !g_second_object && (steady > 60 || back_now)) {
        SpawnSecond(ctx);
        if (g_second_object) SplitOn(ctx);
        steady = 0;
        g_arm_at = g_second_object ? g_frames + 5 : 0;
    }
    if (g_arm_at && g_frames >= g_arm_at && g_second && g_player) {
        g_arm_at = 0;
        Arm(ctx);
    }
}

// The teleport button: BACK on controller 2 puts Player 2 beside Player 1 (never the other way). The game does not
// see that button, nor his START, while he is in (the map and the menus are Player 1's).
std::atomic<bool> g_back2{false};
void Controller2(input::Pad& p) {
    constexpr uint16_t kStart = 16, kBack = 32;
    g_back2 = (p.buttons & kBack) != 0;
    if (g_second_object) p.buttons &= ~(kBack | kStart);  // (START is the pause menu: Player 1's)
    // Down, or in a scene (he is hidden then and comes back beside Player 1 after it): no controls. (In the first
    // chapter's opening, 220 s of scene, a test's Player 2 had walked 13 m off unseen.)
    if (g_second_object && (g_down_until || g_one_view)) {
        bool connected = p.connected;
        p = {};
        p.connected = connected;
    }
}

// Tests: S2_COOP_ENEMY=<frame>[,<template>[,<metres>]] makes an enemy that many metres in front of Player 1 at
// that frame (5 without a number; ",2" after the metres: in front of Player 2): from the template given, or else from the first of the level's templates that
// is of an enemy's class (they are all listed in the log). What it does about two players is then in the log
// (the players' line has their health) and in the pictures.
uint32_t ClassOfTemplate(uint32_t index);
uint32_t g_test_enemy = 0;  // its engine object
void TestEnemy(X86Context& ctx) {
    static const char* v = std::getenv("S2_COOP_ENEMY");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    static const struct {
        uint32_t cls;
        const char* name;
    } kEnemies[] = {{0x4E28364F, "Slayer"}, {0x127243E5, "Mainliner"}, {0x285A9A1F, "Burrower"}, {0x137B74CF, "InfernaBurrower"},
                    {0x26EB0742, "Wretch"}, {0xDB1BE225, "CartWheeler"}, {0xE2EBCB51, "Fester"}, {0x0E15C87E, "Unspeakable NPC"},
                    {0x06CE3FE2, "Rat"}, {0xC3F8C321, "NPC"}};
    uint32_t table = kernel::R32(0x384BA4), count = table ? kernel::R32(table + 0x18) : 0;
    uint32_t pick = 0xFFFFFFFF;
    size_t rank = std::size(kEnemies);
    int listed = 0;
    for (uint32_t i = 0; i < count && i < 0x8000; i++) {
        uint32_t cls = ClassOfTemplate(i);
        for (size_t k = 0; k < std::size(kEnemies); k++) {
            if (cls != kEnemies[k].cls) continue;
            if (listed++ < 60) LOGI("coop: test: template %u is a %s", i, kEnemies[k].name);
            if (k < rank) pick = i, rank = k;
        }
    }
    float far = 5.0f;
    uint32_t from = g_player_object;  // (a fourth number 2: in front of Player 2, for tests of what his hits do)
    if (const char* comma = std::strchr(v, ',')) {
        if (std::atoi(comma + 1) > 0) pick = uint32_t(std::atoi(comma + 1));
        if (const char* second = std::strchr(comma + 1, ',')) {
            far = float(std::atof(second + 1));
            const char* third = std::strchr(second + 1, ',');
            if (third && std::atoi(third + 1) == 2 && g_second_object) from = g_second_object;
        }
    }
    if (pick == 0xFFFFFFFF) {
        LOGI("coop: test: this level has no enemy's template (%u templates)", count);
        return;
    }
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(from + kMatrix), 64);
    float pos[3];
    for (int i = 0; i < 3; i++) pos[i] = ReadF(from + kPosition + i * 4) + far * ReadF(from + kMatrix + 0x20 + i * 4);
    std::memcpy(kernel::HostPtr(matrix + 0x30), pos, 12);
    // (a template marked "not made again while the level runs" is made all the same: see SplitOn)
    uint32_t state = kernel::R32(kObjectManager + kManagerState);
    kernel::W32(kObjectManager + kManagerState, state & ~2u);
    g_test_enemy = kernel::CallGuest(ctx, kSpawn, {g_player_object, matrix, 0, pick});
    kernel::W32(kObjectManager + kManagerState, (kernel::R32(kObjectManager + kManagerState) & ~2u) | (state & 2));
    LOGR("coop: test: enemy from template %u (class %08X) at %.2f %.2f %.2f: object %08X", pick, ClassOfTemplate(pick), pos[0], pos[1], pos[2],
         g_test_enemy);
}

// Tests: S2_COOP_FACE=1 keeps Player 2 turned to the enemy S2_COOP_ENEMY made (four times a second, the game's
// own move, without ending what he is doing), so that a script that only holds his trigger hits it.
void TestFace(X86Context& ctx) {
    static const bool on = std::getenv("S2_COOP_FACE") != nullptr;
    if (!on || !g_test_enemy || !g_second || !g_second_object || g_frames % 15) return;
    if (!mem::IsCommitted(g_test_enemy + 0x110) || (kernel::R32(g_test_enemy + 8) & 0x80010000)) return;
    static uint32_t matrix = mem::AllocSystem(64);
    float m[16];
    std::memcpy(m, kernel::HostPtr(g_second_object + kMatrix), 64);
    float fx = ReadF(g_test_enemy + kPosition) - m[12], fz = ReadF(g_test_enemy + kPosition + 8) - m[14];
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 0.3f) return;
    fx /= len, fz /= len;
    // right = up x forward, with the sign the matrix has now
    float rx = m[5] * m[10] - m[6] * m[9], rz = m[4] * m[9] - m[5] * m[8];
    float sign = rx * m[0] + rz * m[2] < 0 ? -1.0f : 1.0f;
    m[8] = fx, m[9] = 0, m[10] = fz;
    m[0] = sign * fz, m[1] = 0, m[2] = -sign * fx;
    m[4] = 0, m[5] = 1, m[6] = 0;
    std::memcpy(kernel::HostPtr(matrix), m, 64);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = g_second;
    kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
    ctx.ecx = ecx;
}

void HitsSecond();  // (below: who hurt whom, a line a second)
void TestExit(X86Context& ctx);
void TestCensus();
void TestMeter(X86Context& ctx);
void TestCall(X86Context& ctx);
void TestGoto(X86Context& ctx);

// The game's own extra view camera (a security monitor's, or the one an effect looks through) has to be the second
// in the view's list: the engine looks for it there, by number, in two places (0x1C2246 takes its texture for the
// surface that shows it; 0x1D1EDB marks that texture as wanted, and a camera whose texture nobody wants is not
// drawn). With Player 2's view camera in that place the game's own, third, was never drawn and the surface showed
// whatever was in its memory (level 08: a cloud of coloured dots round Torque where a haze belongs). So when the
// game has added a camera of its own behind Player 2's, Player 2's goes to the end of the list. (Entries are moved
// by the engine itself when one is taken out, so nothing keeps their addresses.)
void OrderViews() {
    uint32_t view = kernel::R32(kViewPointer), second = ViewCamera2();
    if (!view || !second) return;
    uint32_t cameras = kernel::R32(view + kViewCameras), count = kernel::R32(view + kViewCount);
    uint32_t index = (second - cameras) / kViewCameraBytes;
    if (count < 3 || count > 16 || index + 1 >= count) return;
    uint8_t* at = static_cast<uint8_t*>(kernel::HostPtr(second));
    std::vector<uint8_t> mine(at, at + kViewCameraBytes);
    std::memmove(at, at + kViewCameraBytes, size_t(count - 1 - index) * kViewCameraBytes);
    std::memcpy(at + size_t(count - 1 - index) * kViewCameraBytes, mine.data(), kViewCameraBytes);
    LOGI("coop: the game has %u view camera%s of its own: Player 2's goes to the end of the list (was number %u of %u)", count - 2,
         count - 2 == 1 ? "" : "s", index + 1, count);
}

// Tests: S2_COOP_CLASSES=<frame> makes one of each kind of enemy the level has a template for, 30 m in front of
// Player 1 and 3 m apart, and logs each one's class object and its methods: the once-a-frame one (+0x18), Detect
// (+0x20), "you are hurt" (where 0x71760 sits in the table), for finding what the kinds share.
void TestClasses(X86Context& ctx) {
    static const char* v = std::getenv("S2_COOP_CLASSES");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    static const struct {
        uint32_t cls;
        const char* name;
    } kEnemies[] = {{0x4E28364F, "Slayer"}, {0x127243E5, "Mainliner"}, {0x285A9A1F, "Burrower"}, {0x137B74CF, "InfernaBurrower"},
                    {0x26EB0742, "Wretch"}, {0xDB1BE225, "CartWheeler"}, {0xE2EBCB51, "Fester"}, {0x0E15C87E, "Unspeakable NPC"},
                    {0x06CE3FE2, "Rat"}, {0xC3F8C321, "NPC"}};
    uint32_t table = kernel::R32(0x384BA4), count = table ? kernel::R32(table + 0x18) : 0;
    static uint32_t matrix = mem::AllocSystem(64);
    int made = 0;
    for (const auto& e : kEnemies) {
        uint32_t pick = 0xFFFFFFFF;
        for (uint32_t i = 0; i < count && i < 0x8000 && pick == 0xFFFFFFFF; i++)
            if (ClassOfTemplate(i) == e.cls) pick = i;
        if (pick == 0xFFFFFFFF) continue;
        std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
        float pos[3];
        for (int i = 0; i < 3; i++)
            pos[i] = ReadF(g_player_object + kPosition + i * 4) + 30.0f * ReadF(g_player_object + kMatrix + 0x20 + i * 4) +
                     3.0f * float(made) * ReadF(g_player_object + kMatrix + i * 4);
        std::memcpy(kernel::HostPtr(matrix + 0x30), pos, 12);
        uint32_t state = kernel::R32(kObjectManager + kManagerState);
        kernel::W32(kObjectManager + kManagerState, state & ~2u);
        uint32_t object = kernel::CallGuest(ctx, kSpawn, {g_player_object, matrix, 0, pick});
        kernel::W32(kObjectManager + kManagerState, (kernel::R32(kObjectManager + kManagerState) & ~2u) | (state & 2));
        uint32_t cls = object ? kernel::R32(object + kClassObject) : 0, methods = cls ? kernel::R32(cls) : 0;
        std::string hurt;
        for (uint32_t slot = 0; methods && slot < 0x100; slot += 4)
            if (kernel::R32(methods + slot) == 0x71760) hurt = " (0x71760 at +" + std::to_string(slot) + ")";
        LOGR("coop: test: a %s (template %u): object %08X, class object %08X, methods %08X: +0x18 %08X, +0x1C %08X, +0x20 %08X, +0x24 %08X, +0x28 %08X%s",
             e.name, pick, object, cls, methods, methods ? kernel::R32(methods + 0x18) : 0, methods ? kernel::R32(methods + 0x1C) : 0,
             methods ? kernel::R32(methods + 0x20) : 0, methods ? kernel::R32(methods + 0x24) : 0, methods ? kernel::R32(methods + 0x28) : 0, hurt.c_str());
        made++;
    }
}

// once a frame (hooks.cpp, before the frame is shown)
void Frame(X86Context& ctx) {
    static const uint64_t spawn_at = [] {
        const char* v = std::getenv("S2_COOP_SPAWN");
        return v ? uint64_t(std::atoll(v)) : 0;
    }();
    static const bool forced = std::getenv("S2_COOP") != nullptr;
    ++g_frames;
    static const uint64_t off_at = std::getenv("S2_COOP_OFF") ? uint64_t(std::atoll(std::getenv("S2_COOP_OFF"))) : 0;  // tests
    bool test_on = (forced || (spawn_at && g_frames + 61 >= spawn_at)) && !(off_at && g_frames >= off_at);
    // Hosting online: the game listens while the setting says so, and Player 2 is in while a player is connected.
    static const bool host_test = std::getenv("S2_COOP_HOST") != nullptr;
    bool host = g_host_setting || host_test;
    if (host != game_net::Hosting() && !(host && g_host_failed)) g_host_failed = host && !game_net::Host(host);
    if (!host) g_host_failed = false;
    g_wanted = test_on || (g_mode_setting == 1 && !host) || (host && game_net::HostHasPlayer());
    Session(ctx, g_wanted);
    // tests: S2_TEST_GOD=1: nothing hurts Player 1 (the game's own mark, +0x1E of anything with health), for long
    // scripted runs into a level
    static const bool god = std::getenv("S2_TEST_GOD") != nullptr;
    if (god && g_player && kernel::R32(g_player + 0x1E) != 1) kernel::W32(g_player + 0x1E, 1);
    TestEnemy(ctx);
    TestFace(ctx);
    TestExit(ctx);
    TestCensus();
    TestMeter(ctx);
    TestCall(ctx);
    TestGoto(ctx);
    TestClasses(ctx);
    HitsSecond();
    OrderViews();
    // (S2_COOP_STREAM_SPLIT=1: the first online build's way, the host's screen split and its right half sent)
    static const bool stream_split = std::getenv("S2_COOP_STREAM_SPLIT") != nullptr;
    bool stream = host && game_net::HostHasPlayer() && !stream_split;
    if (stream != g_stream_view) {
        g_stream_view = stream;
        Rects(ctx);
    }
    if (!ViewCamera2()) gpu::g_stream_snapshot = false;
    // tests: S2_COOP_HURT2=<frame>,<health> sets Player 2's health at that frame (0: what his death does)
    static const char* hurt2 = std::getenv("S2_COOP_HURT2");
    if (hurt2 && g_second && g_frames == uint64_t(std::atoll(hurt2))) {
        const char* comma = std::strchr(hurt2, ',');
        float health = comma ? float(std::atof(comma + 1)) : 0.0f;
        LOGI("coop: test: Player 2's health %.1f of %.1f set to %.1f", ReadF(g_second + 0x16), ReadF(g_second + 0x1A), health);
        std::memcpy(kernel::HostPtr(g_second + 0x16), &health, 4);
    }
    static const uint64_t hurt_at = std::getenv("S2_COOP_HURT") ? uint64_t(std::atoll(std::getenv("S2_COOP_HURT"))) : 0;
    if (hurt_at && g_frames == hurt_at && g_player) {
        // (a Player's health is the float at +0x16, of the most at +0x1A: his method +0x4C = 0x16AB80 reads it)
        float health = ReadF(g_player + 0x16) * 0.5f;
        LOGI("coop: test: Player 1's health %.1f of %.1f halved", ReadF(g_player + 0x16), ReadF(g_player + 0x1A));
        std::memcpy(kernel::HostPtr(g_player + 0x16), &health, 4);
    }
    // Controller 1 is Player 1 and controller 2 is Player 2. (Alone, the game gives slot 0 to the first controller
    // that has a button pressed.)
    if (g_second || g_wanted) {
        uint32_t manager = kernel::R32(kManagerPointer);
        if (manager) kernel::W32(manager + kManagerMap, 0), kernel::W32(manager + kManagerMap + 4, 1);
    }
    if (g_camera2_object && (g_frames == spawn_at + 31 || g_frames == spawn_at + 300)) {
        for (uint32_t object : {kernel::R32(kGameCamera), g_camera2_object}) {
            uint32_t cam = kernel::R32(object + kClassObject), stack = kernel::R32(cam + 0x20), count = kernel::R32(cam + 0x28);
            std::string modes;
            char one[40];
            for (uint32_t i = 0; i < count && i < 8; i++) {
                std::snprintf(one, sizeof one, " (id %u, mode %u)", kernel::R32(stack + i * 8), kernel::R32(stack + i * 8 + 4));
                modes += one;
            }
            LOGI("coop: Camera object %08X class object %08X: flags %X, tracks handle %04X, %u modes:%s", object, cam, kernel::R32(cam + 0x2C),
                 kernel::R32(cam + 0x38) & 0xFFFF, count, modes.c_str());
        }
    }
    static bool back_before = false;
    bool back = g_back2.load();
    if (back && !back_before && g_second_object && !g_one_view) {
        BesideFirst(ctx);
        LOGI("coop: Player 2 teleports to Player 1");
    }
    back_before = back;
    if (ViewCamera2() && !g_one_view) {
        if (!g_stream_view) {  // this frame's picture: a bar where the two views meet
            gpu::g_split_vertical = true;
            gpu::g_split_divider = 2;
        }
        // ... and Player 2's health bar, drawn by the runtime where his half's HUD would have it (the game's HUD
        // keeps one set of values, Player 1's); online it is drawn into the picture sent (net.cpp). A Player's
        // health is the float at +0x16, of the most at +0x1A.
        float most = g_second ? ReadF(g_second + 0x1A) : 0;
        gpu::g_second_health = most > 0 ? (g_down_until ? 0.0f : std::clamp(ReadF(g_second + 0x16) / most, 0.0f, 1.0f)) : -1.0f;
    } else {
        gpu::g_second_health = -1.0f;
    }
    if (g_frames % 60 == 0 && g_second_object && g_player_object) {
        auto yaw = [](uint32_t object) { return std::atan2(ReadF(object + kMatrix + 0x20), ReadF(object + kMatrix + 0x28)) * 57.29578f; };
        {
            uint32_t timer = kernel::R32(0x384B78), manager = kernel::R32(0x391D64);
            static uint32_t flags_before = 0xFFFFFFFF, screens_before = 0xFFFFFFFF;
            uint32_t flags = timer ? kernel::R32(timer + 0xA0) : 0, screens = manager ? kernel::R32(manager + 0x58) : 0;
            if (flags != flags_before || screens != screens_before)
                LOGI("coop: the timer's flags are %X, the menu manager has %u screens", flags, screens);
            flags_before = flags, screens_before = screens;
        }
        LOGI("coop: player 1 at %.2f %.2f %.2f facing %.0f health %.0f, player 2 at %.2f %.2f %.2f facing %.0f health %.0f", ReadF(g_player_object + kPosition),
             ReadF(g_player_object + kPosition + 4), ReadF(g_player_object + kPosition + 8), yaw(g_player_object), g_player ? ReadF(g_player + 0x16) : 0.0f,
             ReadF(g_second_object + kPosition), ReadF(g_second_object + kPosition + 4), ReadF(g_second_object + kPosition + 8), yaw(g_second_object),
             g_second ? ReadF(g_second + 0x16) : 0.0f);
        // (tests) the enemy made by S2_COOP_ENEMY, while its object is alive
        if (g_test_enemy && mem::IsCommitted(g_test_enemy + 0x110) && !(kernel::R32(g_test_enemy + 8) & 0x80010000))
            LOGI("coop: test: the enemy at %.2f %.2f %.2f facing %.0f (flags %08X)", ReadF(g_test_enemy + kPosition), ReadF(g_test_enemy + kPosition + 4),
                 ReadF(g_test_enemy + kPosition + 8), yaw(g_test_enemy), kernel::R32(g_test_enemy + 8));
        else if (g_test_enemy)
            LOGI("coop: test: the enemy's object has ended"), g_test_enemy = 0;
    }
    if (!Trace()) return;
    // the globals watch: 20 frames from 3.5 s after the spawn (the test's second controller is turning then), and
    // 20 more from 7.5 s (it fires then)
    if (spawn_at) {
        uint64_t since = g_frames - spawn_at;
        g_watch_globals = g_second && ((since >= 210 && since < 230) || (since >= 450 && since < 470));
        if (since == 480) {
            for (auto& [key, w] : g_written)
                LOGI("coop trace: method %06X of player %u wrote global %08X in %u frames (last %08X -> %08X)", uint32_t(key >> 40),
                     uint32_t(key >> 32) & 0xFF, uint32_t(key), w.times, w.before, w.after);
            logx::Flush();
        }
    }
    if (g_frames == 1500 && g_player) {  // in play by then in a level started directly
        Dump("player", g_player, 0x100);
        Dump("object", g_player_object, 0x140);
    }
    if (g_frames == 600) {
        uint32_t renderer = kernel::R32(0x399624), game = kernel::R32(0x3934E0), view = kernel::R32(0x3934DC);
        LOGI("coop trace: renderer %08X methods %08X, game %08X methods %08X, view %08X with %u cameras at %08X", renderer,
             renderer ? kernel::R32(renderer) : 0, game, game ? kernel::R32(game) : 0, view, kernel::R32(view + 0x48), kernel::R32(view + 0x40));
        Dump("view", view, 0xAC);
        Dump("view camera 0", kernel::R32(view + 0x40), 0x2C4);
    }
    if (g_frames == 1500 || g_frames == 2400) {  // the camera object, and the list of players
        uint32_t camera = kernel::R32(0x3947A8), list = kernel::R32(0x39479C), count = kernel::R32(0x3947A4);
        uint32_t cls = camera ? kernel::R32(camera + kClassObject) : 0;
        LOGI("coop trace: camera object %08X (template %u), class object %08X of class %08X, methods %08X; %u players listed at %08X", camera,
             camera ? kernel::R32(camera + kTemplateWord) & 0xFFFF : 0, cls, g_class_of[cls], cls ? kernel::R32(cls) : 0, count, list);
        for (uint32_t i = 0; i < count && i < 8; i++) LOGI("coop trace: listed player %u: object %08X", i, kernel::R32(list + i * 4));
        if (camera) Dump("camera object", camera, 0x120);
        if (cls) Dump("camera class object", cls, 0x300);
    }
    if (g_manager) {
        // the bindings (two tables at manager +0x24 and +0x28, 0x18 bytes an entry, ended by -1): action, device
        // kind, control, threshold, ...
        if (g_frames == 1500) {
            for (uint32_t t : {0x24u, 0x28u}) {
                uint32_t e = kernel::R32(g_manager + t);
                for (int i = 0; e && i < 200 && kernel::R32(e) != 0xFFFFFFFF; i++, e += 0x18)
                    LOGI("coop trace: binding table +%02X: action %X, kind %d, control %d, threshold %.2f, %08X %08X", t, kernel::R32(e),
                         int32_t(kernel::R32(e + 4)), int32_t(kernel::R32(e + 8)), ReadF(e + 0x0C), kernel::R32(e + 0x10), kernel::R32(e + 0x14));
            }
        }
        // each controller's controls that are not at rest, whenever the set changes
        static std::string before[4];
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t e = 0x385B40 + i * 0x28;
            uint32_t values = kernel::R32(e + 0x0C), count = kernel::R32(e + 8);
            if (!values || count > 200) continue;
            std::string live;
            char one[32];
            for (uint32_t c = 0; c < count; c++) {
                float v = ReadF(values + c * 4);
                if (v == 0) continue;
                std::snprintf(one, sizeof one, " %u=%.2f", c, v);
                live += one;
            }
            if (live != before[i]) LOGI("coop trace: controller %u controls:%s", i, live.empty() ? " (at rest)" : live.c_str());
            before[i] = live;
        }
    }
    if (g_frames % 120) return;
    for (auto& [method, s] : g_methods) {
        std::string objects;
        char one[40];
        for (auto& [object, n] : s.by_object) {
            std::snprintf(one, sizeof one, " %08X x%u", object, n);
            objects += one;
        }
        LOGI("coop trace: method %06X ran %u times in 120 frames:%s", method, s.calls, objects.c_str());
    }
    g_methods.clear();
}
}  // namespace game_coop

// the class factory (this = registry, object, class id), returns the new class object
X86_EXTERN_FUNC(__imp__sub_0016F3C0);
X86_FUNC(sub_0016F3C0) {
    uint32_t object = kernel::R32(ctx.esp + 4), cls = kernel::R32(ctx.esp + 8);
    bool player = cls == game_coop::kPlayerClass;
    std::string callers = player && game_coop::Trace() ? game_coop::Callers(ctx) : std::string();
    __imp__sub_0016F3C0(ctx, base);
    if (game_coop::Trace()) game_coop::g_class_of[ctx.eax] = cls;
    // S2_CLASS_MAP=1: each class's method table, once (tools\classmap.py makes the table of which function is
    // whose from these lines: the code-reading trace of two players goes by class)
    static const bool map = std::getenv("S2_CLASS_MAP") != nullptr;
    if (map && ctx.eax && mem::IsCommitted(ctx.eax + 4)) {
        static std::map<uint32_t, uint32_t> seen;
        uint32_t methods = kernel::R32(ctx.eax);
        if (seen.emplace(cls, methods).second) LOGR("classmap: %08X %08X", cls, methods);
    }
    if (player) {
        LOGI("coop: Player %08X made for object %08X%s%s", ctx.eax, object, callers.empty() ? "" : ", called from ", callers.c_str());
    }
}

// the object creation core (this = object manager 0x394678; template, ?, existing object, matrix, ?, parameters,
// ?, ?, ?), returns the engine object
X86_EXTERN_FUNC(__imp__sub_0018C1B0);
X86_FUNC(sub_0018C1B0) {
    uint32_t a[9];
    for (int i = 0; i < 9; i++) a[i] = kernel::R32(ctx.esp + 4 + i * 4);
    uint32_t before = game_coop::g_player_object;
    __imp__sub_0018C1B0(ctx, base);
    if (game_coop::Trace() && !before && game_coop::g_player_object && ctx.eax == game_coop::g_player_object)
        LOGI("coop trace: the Player's object %08X came from template %u (arguments %X %X %X %X %X %X %X %X %X)", ctx.eax, a[0], a[0],
             a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
}

// the Player's larger methods, counted
#define COOP_COUNT(addr)                            X86_EXTERN_FUNC(__imp__sub_##addr);             X86_FUNC(sub_##addr) {                              game_coop::Count(0x##addr, ctx.ecx);            __imp__sub_##addr(ctx, base);               }
COOP_COUNT(00021820)
COOP_COUNT(00022730)
// the two that run once a frame: Run and Detect
X86_EXTERN_FUNC(__imp__sub_00025480);
X86_FUNC(sub_00025480) {
    game_coop::Count(0x25480, ctx.ecx);
    uint32_t before = game_coop::g_running;
    game_coop::g_running = ctx.ecx;
    game_coop::AsSecond as(ctx.ecx);
    game_coop::GlobalsWatch watch(0x25480, ctx.ecx == game_coop::g_second ? 2 : 1, game_coop::g_watch_globals);
    __imp__sub_00025480(ctx, base);
    game_coop::g_running = before;
}
// Detect: the Player finds what he touches and what he stands on. (Player 2 has to run it: without it he fell
// through the floor after a jump. What he may not touch is refused where touches are delivered, see RefuseTouch.)
X86_EXTERN_FUNC(__imp__sub_00024B00);
X86_FUNC(sub_00024B00) {
    game_coop::Count(0x24B00, ctx.ecx);
    uint32_t before = game_coop::g_running;
    game_coop::g_running = ctx.ecx;
    game_coop::AsSecond as(ctx.ecx);
    game_coop::GlobalsWatch watch(0x24B00, ctx.ecx == game_coop::g_second ? 2 : 1, game_coop::g_watch_globals);
    __imp__sub_00024B00(ctx, base);
    game_coop::g_running = before;
}

// the walk stick's update, once a frame: then once more for Player 2's copy
X86_EXTERN_FUNC(__imp__sub_0017F130);
X86_FUNC(sub_0017F130) {
    __imp__sub_0017F130(ctx, base);
    if (!game_coop::g_second || game_coop::g_stick2_updating || game_coop::g_stick2_in) return;
    game_coop::g_stick2_updating = true;
    game_coop::SwapStick();
    kernel::CallGuest(ctx, game_coop::kMoveStickUpdate, {});
    game_coop::SwapStick();
    game_coop::g_stick2_updating = false;
}
#define COOP_ASKED(addr)                            X86_EXTERN_FUNC(__imp__sub_##addr);             X86_FUNC(sub_##addr) {                              game_coop::Asked(ctx, 0x##addr);                __imp__sub_##addr(ctx, base);               }
COOP_ASKED(001A0790)
COOP_ASKED(001A09C0)
X86_EXTERN_FUNC(__imp__sub_0001B0D0);
X86_FUNC(sub_0001B0D0) {
    game_coop::Count(0x1B0D0, ctx.ecx);
    if (game_coop::Trace()) {
        static std::map<uint64_t, int> seen;
        uint32_t view = kernel::R32(0x3934DC), current = view ? kernel::R32(view + 0x4C) : 9;
        uint64_t key = (uint64_t(kernel::R32(ctx.esp)) << 8) | current;
        if (seen.size() < 40 && !seen[key]++)
            LOGI("coop trace: Player method 1B0D0 for %08X with view camera %u current, argument %08X, called from %s", ctx.ecx, current,
                 kernel::R32(ctx.esp + 4), game_coop::CallersOf(ctx).c_str());
    }
    __imp__sub_0001B0D0(ctx, base);
}
COOP_COUNT(00029040)
// (0x28E00, the Player is hurt: counted in its hook at the end of this file)
COOP_COUNT(0001F2E0)

// Control manager: is this binding's control locked by another input user? (this = manager, binding, user, exact).
// The lock list (manager +0x30, count +0x38, 16 bytes each: owner user, device kind, control) is searched from the
// newest entry: without "exact" the control is locked when its newest lock belongs to another user.
X86_EXTERN_FUNC(__imp__sub_001A06B0);
X86_FUNC(sub_001A06B0) {
    uint32_t user = kernel::R32(ctx.esp + 8);
    game_coop::g_manager = ctx.ecx;
    // Player 2 may use a control whenever Player 1 may
    if (game_coop::g_second && user == game_coop::g_second + game_coop::kUser) kernel::W32(ctx.esp + 8, game_coop::g_player + game_coop::kUser);
    __imp__sub_001A06B0(ctx, base);
}

// trace: the Camera class's methods, counted
COOP_COUNT(00108240)
COOP_COUNT(00105970)
X86_EXTERN_FUNC(__imp__sub_00108060);
X86_FUNC(sub_00108060) {
    game_coop::Count(0x108060, ctx.ecx);
    static int said = 0;
    if (game_coop::Trace() && said < 3 && ++said) LOGI("coop trace: Camera %08X runs, called from %s", ctx.ecx, game_coop::CallersOf(ctx).c_str());
    __imp__sub_00108060(ctx, base);
}
COOP_COUNT(001058A0)
COOP_COUNT(00105800)

// The renderer is pointed at a texture (this = renderer; texture, ?, ?): 0 when done. Player 2's view camera has
// none: the engine then draws it on the screen, in the camera's own rectangle.
X86_EXTERN_FUNC(__imp__sub_001FB080);
X86_FUNC(sub_001FB080) {
    if (game_coop::g_no_texture && kernel::R32(ctx.esp + 4) == game_coop::g_no_texture) {
        game_coop::g_screen_pass = true;
        kernel::Return(ctx, 0xFFFFFFF7, 3);
        return;
    }
    __imp__sub_001FB080(ctx, base);
}

// ... and back at the screen (this = renderer). After Player 2's view there is nothing to go back from. (A texture
// the renderer was pointed at inside his view, a shadow's, is still its own to leave: renderer +0xE0 is the target
// to go back to.)
X86_EXTERN_FUNC(__imp__sub_001FB290);
X86_FUNC(sub_001FB290) {
    if (game_coop::g_screen_pass && !kernel::R32(ctx.ecx + 0xE0)) {
        game_coop::g_screen_pass = false;
        if (game_coop::g_stream_view && !game_coop::g_one_view) gpu::SnapshotForStream();  // (Player 1's view comes over it)
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_001FB290(ctx, base);
}

// A view camera ends (this = view camera): it releases its texture. Player 2's has none to release.
X86_EXTERN_FUNC(__imp__sub_001BB8C0);
X86_FUNC(sub_001BB8C0) {
    if (game_coop::g_no_texture && kernel::R32(ctx.ecx + game_coop::kVcTexture) == game_coop::g_no_texture) {
        kernel::W32(ctx.ecx + game_coop::kVcTexture, 0);
        LOGI("coop: Player 2's view camera %08X ends", ctx.ecx);
        game_view::SetSplit(false);
        gpu::g_stream_snapshot = false;
    }
    __imp__sub_001BB8C0(ctx, base);
}

// The object manager runs the game's Camera once a frame, after the objects (this = object manager): [+0x130],
// when its object is alive, method +0x18. Player 2's Camera is not the game's and is run here, after it.
X86_EXTERN_FUNC(__imp__sub_0018D130);
X86_FUNC(sub_0018D130) {
    __imp__sub_0018D130(ctx, base);
    uint32_t object = game_coop::g_camera2_object;
    static const bool norun = std::getenv("S2_COOP_NOCAM2RUN") != nullptr;  // bisecting
    if (!object || !game_coop::ViewCamera2() || int32_t(kernel::R32(object + 8)) < 0 || norun) return;
    uint32_t camera = kernel::R32(object + game_coop::kClassObject);
    if (!camera) return;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = camera;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(camera) + 0x18), {});
    ctx.ecx = ecx;
}

// trace: the Player's functions that use the view, with the view camera that is current when they run
namespace game_coop {
void ViewUse(X86Context& ctx, uint32_t fn) {
    if (!Trace()) return;
    static std::map<uint64_t, int> seen;
    uint32_t view = kernel::R32(0x3934DC), current = view ? kernel::R32(view + 0x4C) : 9;
    uint64_t key = (uint64_t(fn) << 40) | (uint64_t(ctx.ecx) << 8) | current;
    if (seen.size() < 80 && !seen[key]++)
        LOGI("coop trace: view use %06X for %08X with view camera %u current, inside the method of Player %08X, called from %s", fn, ctx.ecx, current,
             g_running, CallersOf(ctx).c_str());
}
}  // namespace game_coop
#define COOP_VIEWUSE(addr) X86_EXTERN_FUNC(__imp__sub_##addr); X86_FUNC(sub_##addr) { game_coop::ViewUse(ctx, 0x##addr); __imp__sub_##addr(ctx, base); }
COOP_VIEWUSE(0002BA50)
COOP_VIEWUSE(0002C320)
COOP_VIEWUSE(00026000)
COOP_VIEWUSE(0002B190)
COOP_VIEWUSE(0001E550)
COOP_VIEWUSE(000267D0)
COOP_VIEWUSE(00028730)
COOP_VIEWUSE(0002A630)
COOP_VIEWUSE(0002AB70)
COOP_VIEWUSE(00044380)
COOP_VIEWUSE(00045C80)

// ---- the 2D layer, once for each player --------------------------------------------------------------------------
// After the views the game draws its 2D (from 0x1B83E0, when [0x393364] bit 1 is set):
//   - the overlay: health, ammunition and the rest of the HUD (this = 0x3936C0, a library of its own with layers
//     at +0x19E). 0x22B460(ms) pulls its values from the game (the pieces at 0x1604D0, 0x160680, ... read "the
//     player"), 0x22B4F0(ms) advances it, 0x22B560 draws it. A piece scales itself by the smaller of width / 640
//     and height / 448 of the current view camera and adds that camera's corner (0x216710: +0x2A4, +0x2A8,
//     +0x28C, +0x294).
//   - the menu system's screens, with the crosshair and the messages (the game object's method +0x3C =
//     0x16C980, the manager [0x391D64]), laid out for the whole 640 x 480 frame.
// With two views both are drawn twice, each time laid out for the whole frame (the view camera's rectangle reads
// as the whole frame meanwhile) and put into one half by the runtime (gpu::g_flat2d.half): Player 1's in the
// left half, then Player 2's in the right half with him as the player and his values pulled with no time passing.
// With the game paused (a menu is up) the 2D is drawn once, across the whole picture.
namespace game_coop {
// The timer's flags (+0xA0) are 0x1080 in play and 0x5081 with the pause menu up: bit 0 stops the game's time
// (0x18F720), and it is the mark used here for "a menu has the screen". (This read `& 2` until 2026-11-25, a bit
// that is never set: the pause menu was drawn in both halves, squeezed and on top of itself.)
constexpr uint32_t kTimer = 0x384B78, kTimerFlags = 0xA0, kTimeStopped = 1;

bool TwoLayers() {
    uint32_t timer = kernel::R32(kTimer);
    return g_second && ViewCamera2() && !g_one_view && !g_stream_view && timer && !(kernel::R32(timer + kTimerFlags) & kTimeStopped);
}

struct Layer {
    uint32_t cam = 0, x = 0, y = 0, w = 0, h = 0, fx = 0, fy = 0;
    explicit Layer(int half) {
        uint32_t view = kernel::R32(kViewPointer);
        cam = kernel::R32(view + kViewCameras) + kernel::R32(view + kViewCurrent) * kViewCameraBytes;
        x = kernel::R32(cam + 0x29C), y = kernel::R32(cam + 0x2A0), w = kernel::R32(cam + 0x2A4), h = kernel::R32(cam + 0x2A8);
        fx = kernel::R32(cam + 0x28C), fy = kernel::R32(cam + 0x294);
        kernel::W32(cam + 0x29C, 0), kernel::W32(cam + 0x2A0, 0), kernel::W32(cam + 0x2A4, 640), kernel::W32(cam + 0x2A8, 480);
        kernel::W32(cam + 0x28C, 0), kernel::W32(cam + 0x294, 0);
        gpu::g_flat2d.half = half;
    }
    ~Layer() {
        gpu::g_flat2d.half = 0;
        kernel::W32(cam + 0x29C, x), kernel::W32(cam + 0x2A0, y), kernel::W32(cam + 0x2A4, w), kernel::W32(cam + 0x2A8, h);
        kernel::W32(cam + 0x28C, fx), kernel::W32(cam + 0x294, fy);
    }
};
bool g_layer_inside = false;  // the second pass is running (it comes back into the hooks)
}  // namespace game_coop

// The overlay's draw. Player 1's goes into the left half. A second draw for Player 2 is off (S2_COOP_HUD2=1 turns
// it on for tests): the overlay keeps one set of values, pulled from "the player" by its own script (a task
// raises an event, 0x1E5970 hands it to the HUD's variable source 0x3B7EB8 / 0x216470, which asks the pieces),
// and neither 0x22B460 nor 0x22B4F0 called again makes it pull: the second draw showed Player 1's numbers.
X86_EXTERN_FUNC(__imp__sub_0022B560);
X86_FUNC(sub_0022B560) {
    if (game_coop::g_layer_inside || !game_coop::TwoLayers()) {
        __imp__sub_0022B560(ctx, base);
        return;
    }
    uint32_t overlay = ctx.ecx;
    {
        game_coop::Layer first(1);
        __imp__sub_0022B560(ctx, base);
    }
    static const bool twice = std::getenv("S2_COOP_HUD2") != nullptr;
    if (!twice) return;
    game_coop::g_layer_inside = true;
    {
        game_coop::AsSecond as(game_coop::g_second);
        uint32_t ecx = ctx.ecx;
        game_coop::Layer second(2);
        ctx.ecx = overlay;
        kernel::CallGuest(ctx, 0x22B560, {});
        ctx.ecx = ecx;
    }
    game_coop::g_layer_inside = false;
}

X86_EXTERN_FUNC(__imp__sub_0016C980);
X86_FUNC(sub_0016C980) {
    uint32_t manager = kernel::R32(0x391D64);
    if (game_coop::g_layer_inside || !manager || !game_coop::TwoLayers()) {
        __imp__sub_0016C980(ctx, base);
        return;
    }
    // The function as the game has it, with its drawing done twice: once a frame the menu manager's own turn
    // (0x13C190: the controller's presses go to the screens there, so it must not run twice: a press moved the
    // pause menu two entries when it did), then, unless that says 1, the drawing (the manager's method +0x20), in
    // each half, then 0x13C780 for the manager's part at +0x98 when that is empty.
    uint32_t ecx = ctx.ecx;
    ctx.ecx = manager;
    uint32_t turn = kernel::CallGuest(ctx, 0x13C190, {});
    if (turn != 1) {
        uint32_t draw = kernel::R32(kernel::R32(manager) + 0x20);
        {
            game_coop::Layer first(1);
            ctx.ecx = manager;
            kernel::CallGuest(ctx, draw, {});
        }
        game_coop::g_layer_inside = true;
        {
            game_coop::AsSecond as(game_coop::g_second);
            game_coop::Layer second(2);
            ctx.ecx = manager;
            kernel::CallGuest(ctx, draw, {});
        }
        game_coop::g_layer_inside = false;
        if (!kernel::R32(manager + 0x98)) {
            ctx.ecx = manager + 0x98;
            kernel::CallGuest(ctx, 0x13C780, {});
        }
    }
    ctx.ecx = ecx;
    kernel::Return(ctx, 1, 1);
}

// trace: the overlay's sprite pieces as they are drawn (health, ammunition): which piece, its picture, the
// overlay's current matrix. S2_COOP_PIECES=<frame> logs that frame's and the frame 120 later; S2_COOP_HURT=<frame>
// halves Player 1's health at that frame (to see what changes in the pieces).
namespace game_coop {
uint64_t PiecesAt() {
    static const uint64_t at = std::getenv("S2_COOP_PIECES") ? uint64_t(std::atoll(std::getenv("S2_COOP_PIECES"))) : 0;
    return at;
}
}  // namespace game_coop
X86_EXTERN_FUNC(__imp__sub_00216710);
X86_FUNC(sub_00216710) {
    uint64_t at = game_coop::PiecesAt();
    if (at && (game_coop::g_frames == at || game_coop::g_frames == at + 120)) {
        uint32_t piece = ctx.ecx, ecx = ctx.ecx;
        uint32_t m = kernel::CallGuest(ctx, 0x1E5450, {});  // the overlay's current matrix
        ctx.ecx = ecx;
        auto f = [](uint32_t at_) { return game_coop::ReadF(at_); };
        LOGI("coop pieces: frame %llu %s pass piece %08X picture %X, matrix %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.2f %.2f %.2f %.2f",
             (unsigned long long)game_coop::g_frames, game_coop::g_layer_inside ? "second" : "first", piece, kernel::R32(piece + 0x10), f(m), f(m + 4),
             f(m + 8), f(m + 12), f(m + 16), f(m + 20), f(m + 24), f(m + 28), f(m + 32), f(m + 36), f(m + 40), f(m + 44), f(m + 48), f(m + 52),
             f(m + 56), f(m + 60));
    }
    __imp__sub_00216710(ctx, base);
}

// trace: when the HUD's pieces that read "the player" run
namespace game_coop {
void HudPiece(X86Context& ctx, uint32_t fn) {
    if (!Trace() || !g_second) return;
    static std::map<uint32_t, int> seen;
    uint32_t key = fn | (g_layer_inside ? 0x80000000u : 0);
    uint32_t list = kernel::R32(kObjectManager + kPlayerList);
    if (seen.size() < 80 && !seen[key]++)
        LOGI("coop trace: HUD piece %06X for %08X (%s pass, first listed player %08X, asked %X), called from %s", fn, ctx.ecx,
             g_layer_inside ? "second" : "first", list ? kernel::R32(list) : 0, kernel::R32(ctx.esp + 4), Callers(ctx, 16).c_str());
}
}  // namespace game_coop
#define COOP_HUDPIECE(addr) X86_EXTERN_FUNC(__imp__sub_##addr); X86_FUNC(sub_##addr) { game_coop::HudPiece(ctx, 0x##addr); __imp__sub_##addr(ctx, base); }
COOP_HUDPIECE(00158410)
COOP_HUDPIECE(00158ED0)
COOP_HUDPIECE(0015B1A0)
COOP_HUDPIECE(0015B5A0)
COOP_HUDPIECE(0015BEB0)
COOP_HUDPIECE(0015C380)
COOP_HUDPIECE(0015CB70)
COOP_HUDPIECE(0015CD10)
COOP_HUDPIECE(0015DA90)
COOP_HUDPIECE(001604D0)
COOP_HUDPIECE(00160680)
COOP_HUDPIECE(00160B70)
COOP_HUDPIECE(00162510)
COOP_HUDPIECE(00163300)
COOP_HUDPIECE(00163950)
COOP_HUDPIECE(001639A0)
COOP_HUDPIECE(00163CE0)

// A Camera ends (this = Camera). One that is not shown on a monitor clears the game's camera ([0x3947A8]) and the
// place sound is heard from, whichever Camera they were. When it is Player 2's that ends (co-op switched off, or
// the level's end reaching his first), both are given back to the game's own.
X86_EXTERN_FUNC(__imp__sub_00108120);
X86_FUNC(sub_00108120) {
    uint32_t ending = kernel::R32(ctx.ecx + 4), games = kernel::R32(game_coop::kGameCamera);
    __imp__sub_00108120(ctx, base);
    if (!games || games == ending || kernel::R32(game_coop::kGameCamera)) return;
    if (int32_t(kernel::R32(games + 8)) < 0) return;  // the game's own is ending too
    kernel::W32(game_coop::kGameCamera, games);
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = kernel::R32(game_coop::kSoundPointer);
    if (ctx.ecx) kernel::CallGuest(ctx, game_coop::kSetListener, {games});
    ctx.ecx = ecx;
    ctx.eax = eax;
    LOGI("coop: a second Camera (object %08X) ended; the game's camera is %08X again", ending, games);
}

// ---- Player 1 only: what Player 2 may not touch ---------------------------------------------------------------------
// The game tells an object that another touched it in one place: 0x1941D0 (contact, touched, toucher) and its twin
// 0x194180 (contact, toucher, touched) put the toucher into the contact (+0x30) and call the touched object's
// class method +0x24. Trigger volumes hear of a player that way (Detector 0xFD110 keeps a list of who is inside),
// items are picked up that way (the Player's own +0x24 = 0x1B0D0 sees the item and tells it), and the Player
// lists what he can use that way. Touches between Player 2 and these classes are not delivered, either way round:
// he sets off no trigger, picks nothing up and has nothing to use (the rule of 2026-11-23: only Player 1 opens
// doors, uses and picks up, sets off triggers and scenes). Ladders, ledges and mounted weapons are among them for
// now: Player 2's features come later, one at a time.
namespace game_coop {
constexpr uint32_t kTemplates = 0x384BA4;  // the template table: chunks of 1024 at +0, entry size +0x14, count +0x18
uint32_t ClassOf(uint32_t object) {
    if (!object) return 0;
    uint32_t index = kernel::R32(object + kTemplateWord) & 0xFFFF, table = kernel::R32(kTemplates);
    if (index == 0xFFFF || !table || index >= kernel::R32(table + 0x18)) return 0;
    uint32_t chunk = kernel::R32(kernel::R32(table) + (index >> 10) * 4);
    return chunk ? kernel::R32(chunk + (index & 0x3FF) * kernel::R32(table + 0x14) + 0x0C) : 0;
}

bool NotForSecond(uint32_t cls) {
    switch (cls) {
        // trigger volumes
        case 0x3F0D4EB2:  // Detector
        case 0xF64677A7:  // Pressure Plate
        case 0x10BD8382:  // Level Transition
        case 0x4A1EB7A1:  // Level Transition Remover
        case 0x38ACC9A1:  // STOMP Player
        case 0x1FE33E92:  // Map Marker Trigger
        case 0x2FB36366:  // Archives Trigger
        case 0x96144939:  // Health Threshold Trigger
        case 0x4286A498:  // Edge Passage
        case 0xE66D645E:  // Teleport Pad
        // things to pick up
        case 0x202383E5:  // Ammo Box
        case 0x9710D816:  // Ranged Weapon
        case 0x22E48520:  // Melee Weapon
        case 0x5ED5D552:  // Quest Item Pickup
        case 0x19F64171:  // Health Bonus Item
        case 0x0F86449A:  // Archive Page
        case 0x0FE41EA4:  // Key
        case 0x2A068D9B:  // Map Pack
        case 0x52DAEC1A:  // Insanity Attack Item
        case 0x5A410DDE:  // Item
        case 0x8AE7D2D4:  // Note Item
        case 0xA2C3F6A6:  // Map Item
        case 0xCF3A3FA5:  // Flashlight
        case 0xFE9F718D:  // Missile Item
        // things to use
        case 0x3F4C759C:  // Switch
        case 0x5FEBD8E9:  // Pushable
        case 0xD8E872CE:  // Security Monitor
        case 0xE4002F8D:  // Phone
        case 0x22B79066:  // Toilet
        case 0x69A9942B:  // Mountable
        case 0xC0DCDE7E:  // MountedWeapon
        case 0x671882A1:  // Ladder
        case 0xEC32498A:  // Clamber Point
            return true;
    }
    return false;
}

// true: this touch is not delivered
bool RefuseTouch(uint32_t touched, uint32_t toucher) {
    if (!g_second_object) return false;
    uint32_t other = toucher == g_second_object ? touched : touched == g_second_object ? toucher : 0;
    if (Trace() && g_player_object && (toucher == g_player_object || touched == g_player_object)) {  // what Player 1 touches
        static std::map<uint64_t, int> seen1;
        uint32_t c1 = ClassOf(toucher == g_player_object ? touched : toucher);
        uint64_t key = (uint64_t(c1) << 1) | (toucher == g_player_object ? 1 : 0);
        if (seen1.size() < 60 && !seen1[key]++) LOGI("coop trace: touch: Player 1 %s an object of class %08X", toucher == g_player_object ? "touches" : "is touched by", c1);
    }
    if (!other) return false;
    uint32_t cls = ClassOf(other);
    if (Trace()) {
        static std::map<uint64_t, int> seen2;
        uint64_t key = (uint64_t(cls) << 1) | (toucher == g_second_object ? 1 : 0);
        if (seen2.size() < 60 && !seen2[key]++) LOGI("coop trace: touch: Player 2 %s an object of class %08X", toucher == g_second_object ? "touches" : "is touched by", cls);
    }
    if (!NotForSecond(cls)) return false;
    if (Trace()) {
        static std::map<uint32_t, int> seen;
        if (seen.size() < 40 && !seen[cls]++) {
            // (with the class object's method table, and three of its methods: the search for the "cannot use" sound)
            uint32_t object = kernel::R32(other + kClassObject), methods = object ? kernel::R32(object) : 0;
            LOGI("coop trace: a touch between Player 2 and an object of class %08X is not delivered (class object %08X, methods %08X: +98 %08X, +B0 %08X, +D0 %08X)",
                 cls, object, methods, methods ? kernel::R32(methods + 0x98) : 0, methods ? kernel::R32(methods + 0xB0) : 0,
                 methods ? kernel::R32(methods + 0xD0) : 0);
        }
    }
    return true;
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_001941D0);
X86_FUNC(sub_001941D0) {
    if (game_coop::RefuseTouch(kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp + 12))) {
        kernel::Return(ctx, 1, 3);
        return;
    }
    __imp__sub_001941D0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00194180);
X86_FUNC(sub_00194180) {
    if (game_coop::RefuseTouch(kernel::R32(ctx.esp + 12), kernel::R32(ctx.esp + 8))) {
        kernel::Return(ctx, 1, 3);
        return;
    }
    __imp__sub_00194180(ctx, base);
}

// trace: the third way a touch is delivered (this = something with an object handle at +0x0C; contact)
X86_EXTERN_FUNC(__imp__sub_001706F0);
X86_FUNC(sub_001706F0) {
    if (game_coop::Trace()) {
        static std::map<uint64_t, int> seen;
        uint32_t table = kernel::R32(game_coop::kObjectManager + 0x0C), handle = kernel::R32(ctx.ecx + 0x0C) & 0xFFFF;
        uint32_t touched = table && handle ? kernel::R32(table + handle * 6) : 0, contact = kernel::R32(ctx.esp + 4);
        uint32_t toucher = contact ? kernel::R32(contact + 0x30) : 0;
        uint64_t key = (uint64_t(game_coop::ClassOf(touched)) << 32) | game_coop::ClassOf(toucher);
        if (seen.size() < 60 && !seen[key]++)
            LOGI("coop trace: touch by handle: an object of class %08X%s is told of class %08X%s", game_coop::ClassOf(touched),
                 touched == game_coop::g_second_object ? " (Player 2)" : touched == game_coop::g_player_object ? " (Player 1)" : "",
                 game_coop::ClassOf(toucher), toucher == game_coop::g_second_object ? " (Player 2)" : toucher == game_coop::g_player_object ? " (Player 1)" : "");
    }
    __imp__sub_001706F0(ctx, base);
}

// ---- pickups: what Player 1 gets in the way of weapons, ammunition and health, Player 2 gets too ---------------------
// 0x153A0 (this = Player, item's template, how many) gives a Player an item: the inventory entry (0x11540), and for
// a weapon he has no better use for, the weapon in his hands. Every pickup comes through it (13 callers). When it
// is Player 1 who gets a combat item, Player 2 is given the same (the rule of 2026-11-23); keys, notes, maps and
// archive pages stay Player 1's.
namespace game_coop {
uint32_t ClassOfTemplate(uint32_t index) {
    uint32_t table = kernel::R32(kTemplates);
    if (index == 0xFFFFFFFF || !table || index >= kernel::R32(table + 0x18)) return 0;
    uint32_t chunk = kernel::R32(kernel::R32(table) + (index >> 10) * 4);
    return chunk ? kernel::R32(chunk + (index & 0x3FF) * kernel::R32(table + 0x14) + 0x0C) : 0;
}
bool ForBoth(uint32_t cls) {
    switch (cls) {
        case 0x9710D816:  // Ranged Weapon
        case 0x22E48520:  // Melee Weapon
        case 0xFE9F718D:  // Missile Item (grenades and the like)
        case 0x19F64171:  // Health Bonus Item
        case 0x5A410DDE:  // Item (ammunition)
            return true;
    }
    return false;
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_000153A0);
X86_FUNC(sub_000153A0) {
    uint32_t player = ctx.ecx, item = kernel::R32(ctx.esp + 4), count = kernel::R32(ctx.esp + 8);
    __imp__sub_000153A0(ctx, base);
    static bool inside = false;
    if (inside || !game_coop::g_second || player != game_coop::g_player) return;
    uint32_t cls = game_coop::ClassOfTemplate(item);
    bool both = game_coop::ForBoth(cls);
    if (both) {
        inside = true;
        game_coop::AsSecond as(game_coop::g_second);
        uint32_t ecx = ctx.ecx, eax = ctx.eax;
        ctx.ecx = game_coop::g_second;
        kernel::CallGuest(ctx, 0x153A0, {item, count});
        ctx.ecx = ecx;
        ctx.eax = eax;
        inside = false;
    }
    auto has = [item](uint32_t who) {  // how many of it a Player carries (his inventory's entries: ?, item, how many)
        uint32_t list = kernel::R32(who + 0x128), n = kernel::R32(who + 0x128 + 8);
        for (uint32_t i = 0; list && i < n && i < 256; i++)
            if (kernel::R32(list + i * 12 + 4) == item) return int32_t(kernel::R32(list + i * 12 + 8));
        return 0;
    };
    LOGI("coop: Player 1 is given item %u (class %08X) x%d and has %d; Player 2 %s and has %d", item, cls, int32_t(count), has(player),
         both ? "is given the same" : "is not (Player 1 only)", has(game_coop::g_second));
}

// ---- who hurts whom ----------------------------------------------------------------------------------------------
// Everything with health has one class in common (methods around 0x16AB80): health is the float at +0x16, the most
// at +0x1A, +0x1E set means nothing hurts it. **0x16AF00 (this, amount, the object that did it) takes the amount
// off**: it sets the health through the class's own method +0x50 (0x16BC80 underneath, which tells the HUD), and a
// thing that may not die (+0x0E) is left with 1. It is reached through the class's "you are hurt" method: an
// enemy's is 0x71760 (this, amount, by whom), which first multiplies the amount when it is a Player that did it
// (class 0xC8BC44E1, any Player: 0x46B10 gives his factor), the Player's is 0x28E00 (this, amount, by whom), which
// does nothing while he is dead or the game is paused and passes no "by whom" on. (0x16BDC0, the class method
// +0x5C, and 0x16BB90 are something else: hits with a distance, queued at 0x391EF8; nothing came that way in a
// fist fight or under fire.)
// For the session log, and to see what enemies do about two players: every second, one line for each pair that
// had a Player on either side.
namespace game_coop {
struct Hits {
    uint32_t count = 0;
    float total = 0;
};
std::map<std::string, Hits> g_hits;
uint64_t g_hits_frame = 0;
std::map<uint32_t, int> g_hurt_us;  // engine object -> how often it has hurt a Player (this level)
uint32_t g_hit_by = 0;  // who is hurting a Player now (the Player's method knows, the common one is not told)

std::string WhoIs(uint32_t object) {
    if (!object) return "nothing named";
    if (object == g_player_object) return "Player 1";
    if (object == g_second_object) return "Player 2";
    uint32_t cls = ClassOf(object);
    static const struct {
        uint32_t cls;
        const char* name;
    } kNames[] = {{0xC3F8C321, "an NPC"}, {0x4E28364F, "a Slayer"}, {0x127243E5, "a Mainliner"}, {0x285A9A1F, "a Burrower"},
                  {0x137B74CF, "an InfernaBurrower"}, {0x26EB0742, "a Wretch"}, {0xDB1BE225, "a CartWheeler"}, {0xE2EBCB51, "a Fester"},
                  {0x0E15C87E, "an Unspeakable NPC"}, {0x06CE3FE2, "a Rat"}, {0x9710D816, "a ranged weapon"}, {0x22E48520, "a melee weapon"},
                  {0xFE9F718D, "a missile"}, {0xC8BC44E1, "a Player"}};
    for (const auto& n : kNames)
        if (n.cls == cls) return n.name;
    char out[40];
    std::snprintf(out, sizeof out, "an object of class %08X", cls);
    return out;
}

void Hit(uint32_t victim, uint32_t by, float amount) {
    if (!g_player_object || !victim || !mem::IsCommitted(victim + 0x20)) return;
    uint32_t object = kernel::R32(victim + 4);
    if (by && !mem::IsCommitted(by + 0x110)) by = 0;
    bool player = object == g_player_object || object == g_second_object || (by && (by == g_player_object || by == g_second_object));
    if (!player) return;
    if (by && amount > 0 && (object == g_player_object || object == g_second_object) && by != g_player_object && by != g_second_object)
        g_hurt_us[by]++;  // (see NearerIsSecond: who has shown he is against the players)
    std::string key = WhoIs(object) + " is hurt by " + WhoIs(by);
    if (kernel::R32(victim + 0x1E)) key += " (nothing hurts it now)";
    Hits& h = g_hits[key];
    h.count++;
    h.total += amount;
}

void HitsSecond() {
    if (g_hits.empty() || g_frames < g_hits_frame) return;
    uint32_t timer = kernel::R32(0x384B78);
    g_hits_frame = g_frames + uint64_t(1.0f / std::clamp(timer ? ReadF(timer) : 1.0f / 60, 1.0f / 480, 1.0f / 20));
    for (const auto& [what, h] : g_hits) LOGI("coop: hits: %s: %.1f in %u", what.c_str(), h.total, h.count);
    g_hits.clear();
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_00028E00);
X86_FUNC(sub_00028E00) {
    game_coop::Count(0x28E00, ctx.ecx);
    uint32_t before = game_coop::g_hit_by;
    game_coop::g_hit_by = kernel::R32(ctx.esp + 8);
    // tests: S2_COOP_HURT_TRACE=1 logs where the first hurts of a Player by a person or a creature come from
    static const bool trace = std::getenv("S2_COOP_HURT_TRACE") != nullptr;
    static int said = 0;
    if (trace && said < 8 && game_coop::g_hit_by && mem::IsCommitted(game_coop::g_hit_by + 0x110)) {
        uint32_t cls = game_coop::ClassOf(game_coop::g_hit_by);
        if (cls == 0x0E15C87E || cls == 0xC3F8C321 || cls == 0x4E28364F || cls == 0x127243E5 || cls == 0x285A9A1F) {
            said++;
            LOGI("coop trace: a Player (%08X) is hurt by %08X (class %08X), called from %s", ctx.ecx, game_coop::g_hit_by, cls,
                 game_coop::CallersOf(ctx).c_str());
        }
    }
    __imp__sub_00028E00(ctx, base);
    game_coop::g_hit_by = before;
}

X86_EXTERN_FUNC(__imp__sub_0016AF00);
X86_FUNC(sub_0016AF00) {
    float amount;
    std::memcpy(&amount, kernel::HostPtr(ctx.esp + 4), 4);
    uint32_t by = kernel::R32(ctx.esp + 8);
    {   // tests: S2_COOP_HURT_TRACE=1 also logs the first hurts of anything that is not a Player, whoever did it
        static const bool trace = std::getenv("S2_COOP_HURT_TRACE") != nullptr;
        static int said = 0;
        uint32_t object = mem::IsCommitted(ctx.ecx + 8) ? kernel::R32(ctx.ecx + 4) : 0;
        if (trace && said < 12 && object && object != game_coop::g_player_object && object != game_coop::g_second_object) {
            said++;
            LOGI("coop trace: %s (class object %08X) is hurt %.1f by %s (%08X), called from %s", game_coop::WhoIs(object).c_str(), ctx.ecx, amount,
                 by && mem::IsCommitted(by + 0x110) ? game_coop::WhoIs(by).c_str() : "nothing named", by, game_coop::CallersOf(ctx).c_str());
        }
    }
    game_coop::Hit(ctx.ecx, by ? by : game_coop::g_hit_by, amount);
    __imp__sub_0016AF00(ctx, base);
}

// ---- "the second view" on a surface ---------------------------------------------------------------------------------
// The game can show a second view on a surface (a security monitor's screen): a material of that kind takes the
// texture of the view's second camera when the view has more than one (0x1C2230: `if (view.count > 1) texture =
// view.cameras[1].texture`, then 0x1C0E70 keeps it, calling the texture's own methods). With two players the second
// camera is Player 2's, and its texture is a stand-in with no methods: level 08 (Drowning Pool), which has such a
// surface in sight at its start, crashed there 1.4 s after the opening scene (a read of 0x2C in 0x1C0E70).
// While that function runs, the list reads the way the game expects: the texture of the game's own extra camera
// in the second place when there is one further down, and otherwise a view of one camera (the surface then shows
// nothing, as it does in single player while no camera is on it).
X86_EXTERN_FUNC(__imp__sub_001C2230);
X86_FUNC(sub_001C2230) {
    using namespace game_coop;
    uint32_t view = kernel::R32(kViewPointer);
    uint32_t cameras = view ? kernel::R32(view + kViewCameras) : 0, count = view ? kernel::R32(view + kViewCount) : 0;
    uint32_t second = cameras + kViewCameraBytes + kVcTexture;
    if (!g_no_texture || !cameras || count < 2 || count > 16 || kernel::R32(second) != g_no_texture) {
        __imp__sub_001C2230(ctx, base);
        return;
    }
    uint32_t other = 0;
    for (uint32_t i = 2; i < count && !other; i++) {
        uint32_t texture = kernel::R32(cameras + i * kViewCameraBytes + kVcTexture);
        if (texture && texture != g_no_texture) other = texture;
    }
    static bool said = false;
    if (!said) said = true, LOGI("coop: a surface asks for the second view's picture while that view is Player 2's: it gets %s", other ? "the game's own extra camera's" : "none");
    if (other) {
        kernel::W32(second, other);
        __imp__sub_001C2230(ctx, base);
        kernel::W32(second, g_no_texture);
    } else {
        kernel::W32(view + kViewCount, 1);
        __imp__sub_001C2230(ctx, base);
        kernel::W32(view + kViewCount, count);
    }
}


// The game saves (this = the game; where to, a name, ...): Player 2 is taken out first (see Return, above).
X86_EXTERN_FUNC(__imp__sub_001B4D50);
X86_FUNC(sub_001B4D50) {
    game_coop::BeforeSave(ctx);
    __imp__sub_001B4D50(ctx, base);
}

// ---- found by reading the code: the places that ask "was it the player?" -----------------------------------------
// (2026-11-25, the trace of all 162 uses of "the player": work\COOP_TRACE.md, "By the code".)
// 1. An enemy's or a person's "you are hit" (method +0x58 = 0x739E0, which calls 0x72330) asks whether the one who
//    hit it is the first Player, in two places: 0x73A39 (a thing that only one of Torque's two forms can hurt: the
//    other form's hit is thrown back at him) and 0x7251F (the hit killed it: the Player is credited through
//    0x2B5F0, which is his morality for that kill, 0x46B60, the insanity meter +0x48A and the kill streak). Player
//    2's hits failed both tests: his kills counted for nothing and the form rule did not hold for him. While such
//    a hit is handled, he is "the player".
// 2. Morality is the two players' together (the rule of 2026-11-24: both players' actions count, both see the
//    same ending), and it lives in Player 1 (+0x482, saved with him): 0x46B60 called for Player 2 is run for Player
//    1. The insanity meter (+0x48A) is each player's own: Player 2 fills his with his kills and changes form with
//    his own button (the same rules; first done on 2026-11-25 as one shared meter, put right the same day).
// 3. A level's exit (Level Transition, 0x160D20) walks the players by number: it reads a start point for each from
//    its own properties, saves, loads the next level and puts each player on his point. The levels were made for
//    one player. Player 2 is taken out before it, so the exit is the one-player exit, and he comes in beside
//    Player 1 in the new level like after any level start.
namespace game_coop {
int g_as_second = 0;  // depth of "Player 2 is the player" below
uint32_t g_as_place = 0;

bool HitBySecond(uint32_t hit) {  // the hit's maker is a handle at +0x24 into the object table (6-byte entries)
    if (!g_second_object || !hit || !mem::IsCommitted(hit + 0x28)) return false;
    uint32_t table = kernel::R32(kObjectManager + 0x0C);
    return table && kernel::R32(table + uint32_t(kernel::R16(hit + 0x24)) * 6) == g_second_object;
}
struct ListAsSecond {
    bool on;
    explicit ListAsSecond(bool wanted) : on(wanted) {
        if (on && g_as_second++ == 0) g_as_place = ListSecondFirst();
    }
    ~ListAsSecond() {
        if (on && --g_as_second == 0) ListSecondBack(g_as_place);
    }
};
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_000739E0);
X86_FUNC(sub_000739E0) {
    game_coop::ListAsSecond as(game_coop::HitBySecond(kernel::R32(ctx.esp + 4)));
    __imp__sub_000739E0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00072330);
X86_FUNC(sub_00072330) {
    bool second = game_coop::HitBySecond(kernel::R32(ctx.esp + 4));
    // tests: S2_COOP_ONEHIT=1 leaves whatever Player 2 hits with half a point of health, so his hit kills it
    static const bool onehit = std::getenv("S2_COOP_ONEHIT") != nullptr;
    if (onehit && second && mem::IsCommitted(ctx.ecx + 0x20) && game_coop::ReadF(ctx.ecx + 0x16) > 0.5f) {
        float half = 0.5f;
        std::memcpy(kernel::HostPtr(ctx.ecx + 0x16), &half, 4);
    }
    game_coop::ListAsSecond as(second);
    __imp__sub_00072330(ctx, base);
}

// kill credit (this = Player; the one killed, ...), and morality (this = Player; how much)
X86_EXTERN_FUNC(__imp__sub_0002B5F0);
X86_FUNC(sub_0002B5F0) {
    // Each player has his own insanity meter and can change form on his own (the co-op rules of 2026-11-24, as
    // the project's coordinator passed them on 2026-11-25): the kill's meter and streak stay with the one who
    // killed. Its morality is shared: 0x46B60, called from in here for Player 2, is run for Player 1 (below).
    uint32_t player = ctx.ecx;
    int who = player == game_coop::g_player ? 1 : game_coop::g_second && player == game_coop::g_second ? 2 : 0;
    bool known = who && game_coop::g_player && mem::IsCommitted(player + 0x490) && mem::IsCommitted(game_coop::g_player + 0x490);
    float morality = known ? game_coop::ReadF(game_coop::g_player + 0x482) : 0, meter = known ? game_coop::ReadF(player + 0x48A) : 0;
    __imp__sub_0002B5F0(ctx, base);
    if (known)
        LOGI("coop: a kill by Player %d: the shared morality %.2f to %.2f, his insanity meter %.2f to %.2f", who, morality,
             game_coop::ReadF(game_coop::g_player + 0x482), meter, game_coop::ReadF(player + 0x48A));
}
X86_EXTERN_FUNC(__imp__sub_00046B60);
X86_FUNC(sub_00046B60) {
    if (game_coop::g_second && ctx.ecx == game_coop::g_second && game_coop::g_player) ctx.ecx = game_coop::g_player;
    __imp__sub_00046B60(ctx, base);
}

// Tests: S2_COOP_EXIT=<frame> sets off the level's exit at that frame without walking to it: the first Level
// Transition object that is loaded gets its own "go" (0x160B70, this = the Level Transition; it stops Player 1, puts
// up the loading screen and leaves itself at 0x392138, from where the game's frame calls 0x160D20).
namespace game_coop {
void TestExit(X86Context& ctx) {
    static const char* v = std::getenv("S2_COOP_EXIT");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    uint32_t table = kernel::R32(kObjectManager + 0x0C);
    int seen = 0;
    for (uint32_t i = 0; table && i < 0x4000 && mem::IsCommitted(table + i * 6 + 4); i++) {
        uint32_t object = kernel::R32(table + i * 6);
        // (a free entry holds a small number, the next free one, not an object)
        if (object < 0x400000 || !mem::IsCommitted(object) || !mem::IsCommitted(object + 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
        if (ClassOf(object) != 0x10BD8382) continue;
        uint32_t transition = kernel::R32(object + 0x60);
        // its properties 0 to 6 (0: the level to load, 1: a start point for each player, 2 and 3: which level
        // comes next, 4 and 5: switches, 6: the loading screen), through the object's own property reader
        uint32_t props = kernel::R32(object + 0xEC), value[7] = {};
        uint32_t ecx = ctx.ecx;
        for (uint32_t k = 0; props && k < 7; k++) {
            ctx.ecx = props;
            uint32_t at = kernel::CallGuest(ctx, kernel::R32(kernel::R32(props) + 8), {k, 0xFFFFFFFF});
            value[k] = at && mem::IsCommitted(at) ? kernel::R32(at) : 0xDEADDEAD;
        }
        ctx.ecx = ecx;
        static const int which = std::strchr(v, ',') ? std::atoi(std::strchr(v, ',') + 1) : 0;
        bool go = seen == which && transition;
        LOGR("coop: test: Level Transition %d: object %08X (class object %08X), properties %08X %08X %08X %08X %08X %08X %08X%s", seen, object,
             transition, value[0], value[1], value[2], value[3], value[4], value[5], value[6], go ? ": set off" : "");
        seen++;
        if (!go || std::strstr(v, "list")) continue;
        ctx.ecx = transition;
        kernel::CallGuest(ctx, 0x160B70, {0});
        ctx.ecx = ecx;
    }
    if (!seen) LOGR("coop: test: no Level Transition object is loaded");
}
}  // namespace game_coop

// a level's exit (this = the Level Transition)
X86_EXTERN_FUNC(__imp__sub_00160D20);
X86_FUNC(sub_00160D20) {
    game_coop::HudPiece(ctx, 0x160D20);
    if (game_coop::g_second_object) {
        uint32_t ecx = ctx.ecx;
        game_coop::g_return.valid = false;
        game_coop::EndSecond(ctx, "a level exit: the exit is the one-player exit");
        ctx.ecx = game_coop::kObjectManager;  // the ended objects are freed now (what the game's save does first)
        kernel::CallGuest(ctx, 0x18DDB0, {});
        ctx.ecx = ecx;
        LOGR("coop: level exit with %u player(s) listed", kernel::R32(game_coop::kObjectManager + game_coop::kPlayerCount));
    }
    __imp__sub_00160D20(ctx, base);
}

// 4. A behaviour that a level's designer aims at "the player" (six kinds of behaviour have the switch "the target is
//    the player" beside "the target is the thing named ...": 0x75E00, 0x76740, 0x78DC0, 0x795A0, 0x952A0, 0xA39C0,
//    each run when the behaviour starts; the target is kept as a handle from then on) took the first Player. That
//    is why the inmates of chapter 1's yard, who are sent by such behaviours, went for Player 1 alone while
//    creatures that find their own target (Detect, 0x73490) went for both. When the behaviour's owner is a creature,
//    or an armed person who has already hurt a Player twice, the nearer of the two players is "the player" while
//    the behaviour starts. Everyone else keeps Player 1: someone who is to follow or lead the player must stay
//    with the one who opens the doors. S2_COOP_NO_NEAREST=1 switches this off (tests).
namespace game_coop {
bool NearerIsSecond(X86Context& ctx, uint32_t behaviour, uint32_t fn) {
    static const bool off = std::getenv("S2_COOP_NO_NEAREST") != nullptr;
    if (off || !g_second_object || !g_second || !g_player_object || g_down_until || g_one_view || g_as_second) return false;
    if (!mem::IsCommitted(behaviour + 0x20)) return false;
    uint32_t holder = kernel::R32(behaviour + 0x1C);
    if (holder < 0x10000 || !mem::IsCommitted(holder + 0x14)) return false;
    uint32_t owner = kernel::R32(holder + 0x10);
    if (owner < 0x400000 || !mem::IsCommitted(owner) || !mem::IsCommitted(owner + 0x110)) return false;
    uint32_t cls = ClassOf(owner);
    bool person = cls == 0x0E15C87E;
    switch (cls) {
        case 0x4E28364F: case 0x127243E5: case 0x285A9A1F: case 0x137B74CF: case 0x26EB0742: case 0xDB1BE225: case 0xE2EBCB51:
        case 0x0E15C87E:
            break;
        default:
            return false;
    }
    if (person) {
        // A person takes the nearer player only once he has shown he is against them: he has hurt a Player twice.
        // (First tried by side, the class method +0x74 that the hit code compares: the guard who walks with Torque
        // at the start of level 05b is not on the players' side by that number, and would have followed Player 2.)
        // Until then, and for everyone who never hurts them (escorts, guides, people to be saved), "the player" is
        // Player 1.
        auto it = g_hurt_us.find(owner);
        if (it == g_hurt_us.end() || it->second < 2) return false;
    }
    float d1 = 0, d2 = 0;
    for (int i = 0; i < 3; i++) {
        float at = ReadF(owner + kPosition + i * 4), a = ReadF(g_player_object + kPosition + i * 4) - at, b = ReadF(g_second_object + kPosition + i * 4) - at;
        d1 += a * a, d2 += b * b;
    }
    static int said = 0;
    if (said < 40) {
        said++;
        LOGI("coop: %s starts a behaviour aimed at the player (%06X): Player 1 is %.1f m away, Player 2 %.1f m: it takes Player %d",
             WhoIs(owner).c_str(), fn, std::sqrt(d1), std::sqrt(d2), d2 < d1 ? 2 : 1);
    }
    return d2 < d1;
}
}  // namespace game_coop
#define COOP_AIMED(addr) X86_EXTERN_FUNC(__imp__sub_##addr); X86_FUNC(sub_##addr) { game_coop::ListAsSecond as(game_coop::NearerIsSecond(ctx, ctx.ecx, 0x##addr)); __imp__sub_##addr(ctx, base); }
COOP_AIMED(00075E00)
COOP_AIMED(00076740)
COOP_AIMED(00078DC0)
COOP_AIMED(000795A0)
COOP_AIMED(000952A0)
COOP_AIMED(000A39C0)

// ---- test aids for the risk points of the playthrough study ---------------------------------------------------------
// S2_TEST_CENSUS=<frame>: how many objects of each kind worth testing are loaded, with the first one's place.
// S2_TEST_GOTO=<frame>,<class id in hex>[,<which>[,<metres>[,<side>]]]: Player 1 is put <metres> (1.2) from the
// <which>-th loaded object of that class, facing it (side 0: on the object's forward side, 1: behind it, 2: to its
// right, 3: to its left), by the game's own teleport; Player 2 comes beside him. A test only: the co-op rules never
// move Player 1.
namespace game_coop {
struct Kind {
    uint32_t cls;
    const char* name;
};
const Kind kKinds[] = {{0x69A9942B, "Mountable"}, {0xC0DCDE7E, "MountedWeapon"}, {0x671882A1, "Ladder"}, {0xEC32498A, "Clamber Point"},
                       {0x3F4C759C, "Switch"}, {0x5FEBD8E9, "Pushable"}, {0xD8E872CE, "Security Monitor"}, {0x08A4DE62, "Autosave"},
                       {0xAAAD15CA, "Movie Player"}, {0x38ACC9A1, "STOMP Player"}, {0x9710D816, "Ranged Weapon"}, {0x22E48520, "Melee Weapon"},
                       {0x202383E5, "Ammo Box"}, {0xE4002F8D, "Phone"}, {0xE66D645E, "Teleport Pad"}, {0x10BD8382, "Level Transition"},
                       {0x5F96BDA5, "StruggleBehavior"}, {0xEEA94147, "GrabBehavior"}, {0x0E15C87E, "Unspeakable NPC"}, {0xC3F8C321, "NPC"},
                       {0x4E28364F, "Slayer"}, {0x127243E5, "Mainliner"}, {0x285A9A1F, "Burrower"}, {0xDB1BE225, "CartWheeler"},
                       {0xE2EBCB51, "Fester"}, {0x26EB0742, "Wretch"}};

template <typename F>
void EachObject(F&& f) {
    uint32_t table = kernel::R32(kObjectManager + 0x0C);
    for (uint32_t i = 0; table && i < 0x4000 && mem::IsCommitted(table + i * 6 + 4); i++) {
        uint32_t object = kernel::R32(table + i * 6);
        if (object < 0x400000 || !mem::IsCommitted(object) || !mem::IsCommitted(object + 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
        f(object);
    }
}

void TestCensus() {
    static const char* v = std::getenv("S2_TEST_CENSUS");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    std::map<uint32_t, std::pair<int, uint32_t>> seen;
    EachObject([&](uint32_t object) {
        auto& s = seen[ClassOf(object)];
        if (!s.first++) s.second = object;
    });
    for (const Kind& k : kKinds) {
        auto it = seen.find(k.cls);
        if (it == seen.end()) continue;
        uint32_t o = it->second.second;
        LOGR("census: %s x%d, the first at %.1f %.1f %.1f (Player 1 at %.1f %.1f %.1f)", k.name, it->second.first, ReadF(o + kPosition),
             ReadF(o + kPosition + 4), ReadF(o + kPosition + 8), ReadF(g_player_object + kPosition), ReadF(g_player_object + kPosition + 4),
             ReadF(g_player_object + kPosition + 8));
    }
}

void TestGoto(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_GOTO");
    if (!v || !g_player_object || !g_player || g_frames != uint64_t(std::atoll(v))) return;
    uint32_t cls = 0;
    int which = 0, side = 0;
    float far = 1.2f;
    if (const char* c = std::strchr(v, ',')) {
        cls = uint32_t(std::strtoul(c + 1, nullptr, 16));
        if (const char* c2 = std::strchr(c + 1, ',')) {
            which = std::atoi(c2 + 1);
            if (const char* c3 = std::strchr(c2 + 1, ',')) {
                far = float(std::atof(c3 + 1));
                if (const char* c4 = std::strchr(c3 + 1, ',')) side = std::atoi(c4 + 1);
            }
        }
    }
    uint32_t target = 0;
    int n = 0;
    EachObject([&](uint32_t object) {
        if (ClassOf(object) != cls) return;
        if (n < 24)
            LOGR("goto: %08X number %d at %.1f %.1f %.1f", cls, n, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8));
        if (n++ == which) target = object;
    });
    if (!target) {
        LOGR("goto: no object number %d of class %08X is loaded (%d are)", which, cls, n);
        return;
    }
    float m[16], o[16];
    std::memcpy(m, kernel::HostPtr(g_player_object + kMatrix), 64);
    std::memcpy(o, kernel::HostPtr(target + kMatrix), 64);
    float dx = side < 2 ? o[8] : o[0], dz = side < 2 ? o[10] : o[2];
    if (side & 1) dx = -dx, dz = -dz;
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.01f) dx = 1, dz = 0, len = 1;
    dx /= len, dz /= len;
    float rx = m[5] * m[10] - m[6] * m[9], rz = m[4] * m[9] - m[5] * m[8];
    float sign = rx * m[0] + rz * m[2] < 0 ? -1.0f : 1.0f;
    float fx = -dx, fz = -dz;  // he faces the object
    m[8] = fx, m[9] = 0, m[10] = fz;
    m[0] = sign * fz, m[1] = 0, m[2] = -sign * fx;
    m[4] = 0, m[5] = 1, m[6] = 0;
    m[12] = o[12] + dx * far, m[13] = o[13] + 0.1f, m[14] = o[14] + dz * far;
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), m, 64);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = g_player;
    kernel::CallGuest(ctx, kPlayerStop, {});
    ctx.ecx = g_player;
    kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
    ctx.ecx = ecx;
    LOGR("goto: Player 1 put at %.1f %.1f %.1f, %.1f m from object %08X of class %08X", m[12], m[13], m[14], far, target, cls);
    BesideFirst(ctx);
}
}  // namespace game_coop

// S2_TEST_METER=<frame>[,<player>]: that player's insanity meter is filled (0x2ADD0, this = Player; the most is
// 0x2A490), so a script can press the transform button. And always: a line when a player changes form (Player
// +0xBC bit 0x80000 is the creature form) or the meter's fill changes by a tenth.
namespace game_coop {
void TestMeter(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_METER");
    if (v && g_player && g_frames == uint64_t(std::atoll(v))) {
        uint32_t who = std::strchr(v, ',') && std::atoi(std::strchr(v, ',') + 1) == 2 ? g_second : g_player;
        if (who) {
            uint32_t ecx = ctx.ecx;
            ctx.ecx = who;
            kernel::CallGuest(ctx, 0x2ADD0, {0x461C4000});  // 10000.0: the function keeps it to the most
            ctx.ecx = ecx;
            LOGR("test: Player %d's insanity meter is filled: %.1f", who == g_second ? 2 : 1, ReadF(who + 0x48A));
        }
    }
    static uint32_t form[2] = {0, 0};
    uint32_t players[2] = {g_player, g_second};
    for (int i = 0; i < 2; i++) {
        if (!players[i] || !mem::IsCommitted(players[i] + 0x490)) { form[i] = 0; continue; }
        uint32_t now = kernel::R32(players[i] + 0xBC) & 0x80000;
        if (now != form[i]) LOGI("coop: Player %d is now in %s form (insanity meter %.1f)", i + 1, now ? "creature" : "human", ReadF(players[i] + 0x48A));
        form[i] = now;
    }
    // S2_TEST_FLAGS=1: a line whenever a player's state words change (+0xB0, +0xBC: what he is doing), at most 300
    static const bool flags = std::getenv("S2_TEST_FLAGS") != nullptr;
    static uint32_t was[2][2] = {};
    static int said = 0;
    for (int i = 0; flags && i < 2; i++) {
        if (!players[i] || !mem::IsCommitted(players[i] + 0x490)) continue;
        uint32_t b0 = kernel::R32(players[i] + 0xB0), bc = kernel::R32(players[i] + 0xBC);
        if ((b0 != was[i][0] || bc != was[i][1]) && said < 300) {
            said++;
            uint32_t object = kernel::R32(players[i] + 4);
            LOGI("flags: Player %d +B0 %08X +BC %08X at %.1f %.1f %.1f", i + 1, b0, bc, ReadF(object + kPosition), ReadF(object + kPosition + 4),
                 ReadF(object + kPosition + 8));
        }
        was[i][0] = b0, was[i][1] = bc;
    }
}

// S2_TEST_CALL=<frame>,<class id in hex>,<function in hex>[,<which>]: calls that function (this = the class object of
// the <which>-th loaded object of that class; one argument, 0), for setting off things a level sets off by message:
// an Autosave (0x108400), a Movie Player.
void TestCall(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_CALL");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    const char* c = std::strchr(v, ',');
    const char* c2 = c ? std::strchr(c + 1, ',') : nullptr;
    if (!c2) return;
    uint32_t cls = uint32_t(std::strtoul(c + 1, nullptr, 16)), fn = uint32_t(std::strtoul(c2 + 1, nullptr, 16));
    int which = std::strchr(c2 + 1, ',') ? std::atoi(std::strchr(c2 + 1, ',') + 1) : 0, n = 0;
    uint32_t target = 0;
    EachObject([&](uint32_t object) {
        if (ClassOf(object) == cls && n++ == which) target = object;
    });
    uint32_t self = target ? kernel::R32(target + 0x60) : 0;
    LOGR("test: call %06X on object %08X (class object %08X), number %d of %d of class %08X", fn, target, self, which, n, cls);
    if (!self) return;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    kernel::CallGuest(ctx, fn, {0});
    ctx.ecx = ecx;
}
}  // namespace game_coop
