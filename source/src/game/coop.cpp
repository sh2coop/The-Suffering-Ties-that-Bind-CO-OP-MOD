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
#include <chrono>
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
float Aspect();          // width / height of the picture the frame is shown in
}
namespace game_detail {  // detail.cpp: the record of every frame and object
bool On();
void Line(const char* fmt, ...);
}
namespace game_record {  // record.cpp: pictures of the window
void Mark(const std::string& why);
uint32_t Marks(std::string* why);
}

namespace game_skins {  // skins.cpp: each player's character
int BodySkin(uint32_t object);
uint64_t SkinKey(int character, uint32_t place);  // the pack image a character's body is drawn with in that place
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
         mem::Spans(user, 4) ? kernel::R32(user + 4) : 99, action, g_running, Callers(ctx, 6).c_str());
}

std::string CallersOf(X86Context& ctx) { return Callers(ctx, 8); }
extern bool g_layer_inside;

// Tests: S2_ACTION_TRACE=1 logs which of the game's controller actions answer "yes" for each Player (0x1A09C0: this =
// the input system [0x384BC8]; the Player's input user part, the action's number, how it is asked), once per press,
// and each action a Player is asked about, once.
extern uint32_t g_player, g_second;
int DoubleAction(uint32_t user, uint32_t action);  // ("the stand-in bodies": the controller a stand-in's question goes to, or -1)
void MirrorStates(X86Context& ctx, int which);     // ... and the states it takes over from its owner
bool DoublesThing(uint32_t object);                // a stand-in body, or what one holds
void Answered(X86Context& ctx, uint32_t getter, uint32_t user, uint32_t action, uint32_t how) {
    static const bool trace = std::getenv("S2_ACTION_TRACE") != nullptr;
    if (!trace) return;
    int who = g_player && user == g_player + 0x3A ? 1 : g_second && user == g_second + 0x3A ? 2 : 0;
    if (!who) return;
    static std::map<uint64_t, uint64_t> last;  // (getter, who, how, action) -> the frame it last answered yes
    static std::map<uint64_t, int> asked;
    uint64_t key = (uint64_t(getter & 0xFFF) << 52) | (uint64_t(who) << 48) | (uint64_t(how & 0xFF) << 32) | action;
    if (!asked[key]++ && asked.size() < 600) LOGI("action trace: Player %d is asked about action %X (getter %06X, how %u)", who, action, getter, how);
    if (getter != 0x1A09C0 || !ctx.eax) return;
    uint64_t& at = last[key];
    if (g_frames > at + 1) LOGI("action trace: frame %llu: Player %d: action %X (how %u) says %X", (unsigned long long)g_frames, who, action, how, ctx.eax);
    at = g_frames;
}

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
// A first-person player's body for the other player's view (see "the stand-in bodies", below): one more Player
// object for each player
struct Double {
    uint32_t object = 0, player = 0;      // its engine object and its Player (the class object); 0: not made
    bool out = false;                     // its owner is in first person: it is drawn and runs (else it is put away)
    uint64_t since = 0;                   // the frame it was made in, or last taken out or put away
    bool landed = false;                  // it has been put on its owner the game's way since it was taken out (PinDoubles)
    uint32_t display = 0;                 // its own weapon display on the HUD (Player +0x7F0), taken off it while it lives
    uint32_t held[2] = {};                // the objects in its hands (once a frame, for the draw)
    uint32_t owner_held[2] = {};          // and in its owner's, while it is out
    uint32_t tried = 0xFFFF;              // the weapon it was last handed (an item: a template's number)
    uint64_t tried_at = 0;
    uint32_t loose[2] = {};               // what it has ready to throw (on its belt) and what it has taken in hand to throw
    uint32_t owner_loose[2] = {};         // ... and its owner's, on his first-person arms, while it is out
    uint32_t ready_tried = 0xFFFFFFFE;    // the thing to throw it was last handed (an item), 0xFFFFFFFF: it was taken off it
    uint64_t ready_at = 0;
};
Double g_doubles[2];              // [0] Player 1's, [1] Player 2's
bool g_doubles_fresh = false;     // Doubles has looked them over since the views were last drawn (the second pin only then)
bool g_double_spawning = false;   // the game is making a stand-in right now (it gets no HUD parts: the hook on 0x1DE10)
uint32_t g_double_hits = 0;       // how often something hurt a stand-in (it should be never: the record says so if it happens)
int DoubleOf(uint32_t player);    // which stand-in this Player is: 0, 1 or -1
int DoubleObject(uint32_t object);
void EndDoubles(X86Context& ctx, const char* why);
void Doubles(X86Context& ctx);     // once a frame
void PinDoubles(X86Context& ctx);  // the stand-ins that are out are put exactly where their owners are
bool OfDouble(const Double& d, uint32_t object);  // its body, or one of the things on it
uint32_t g_no_texture = 0;      // the stand-in texture of Player 2's view camera
extern const char* g_scene_why;
bool g_one_view = false;        // a scene: the picture is one view, Player 1's (see Views)
extern uint64_t g_down_until;
extern uint64_t g_spare_at;  // (Spare, below)
extern bool g_paused_now;
extern bool g_stopped_now;
extern std::map<uint32_t, int> g_hurt_us;
void Downs(X86Context& ctx);
bool g_screen_pass = false;     // the engine is drawing Player 2's view camera
uint32_t FlashlightOf(uint32_t player);  // a Player's Flashlight (the class object; 0: he has none)
void Flashlights();                      // ... and both players', looked at once a frame for the log
void TestTorchAgain(X86Context& ctx);
uint64_t Frames() { return g_frames; }

// Player 2's view camera, wherever it is in the list now (0: none)
uint32_t ViewCamera2() {
    uint32_t view = kernel::R32(kViewPointer);
    if (!view || !g_no_texture) return 0;
    uint32_t cameras = kernel::R32(view + kViewCameras), count = kernel::R32(view + kViewCount);
    for (uint32_t i = 1; cameras && i < count && i < 16; i++)
        if (kernel::R32(cameras + i * kViewCameraBytes + kVcTexture) == g_no_texture) return cameras + i * kViewCameraBytes;
    return 0;
}

// The two view cameras' rectangles for the state now: side by side, or one view (a scene: Player 2's is left out,
// its texture's word says "nothing shows it").
void Rects(X86Context& ctx) {
    uint32_t view = kernel::R32(kViewPointer), second = ViewCamera2();
    if (!view || !second) return;
    uint32_t w = kernel::R32(view + 0xA4), h = kernel::R32(view + 0xA8);
    bool whole = g_one_view;
    uint32_t first = kernel::R32(view + kViewCameras), ecx = ctx.ecx;
    ctx.ecx = first;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(first)), {0, 0, whole ? w : w / 2, h});
    ctx.ecx = second;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(second)), {w / 2, 0, w / 2, h});
    ctx.ecx = ecx;
    kernel::W16(g_no_texture + 8, g_one_view ? 1 : 2);
    game_view::SetSplit(!whole);
    if (g_one_view) LOGI("coop: one view because: %s", g_scene_why);
    LOGI("coop: views: %s", g_one_view ? "one (a scene)" : "side by side");
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
        // (tests: S2_COOP_SWAP_PARTS=<bits> leaves parts of it out: 1 the game's camera, 2 the current view camera,
        // 4 the view matrix)
        static const int parts = std::getenv("S2_COOP_SWAP_PARTS") ? std::atoi(std::getenv("S2_COOP_SWAP_PARTS")) : 7;
        camera = kernel::R32(kGameCamera);
        if (parts & 1) kernel::W32(kGameCamera, g_camera2_object);
        current = kernel::R32(view + kViewCurrent);
        matrix = kernel::R32(kViewMatrixPointer);
        if (parts & 2) kernel::W32(view + kViewCurrent, (mine - kernel::R32(view + kViewCameras)) / kViewCameraBytes);
        if (parts & 4) kernel::W32(kViewMatrixPointer, mine + 8);
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
// A stand-in body's own methods: it is "the player" (the first in the list) meanwhile, with its owner's walk stick,
// Camera and view (AsSecond's swap, when the owner is Player 2). Declared before AsSecond in the hooks, so that it
// is undone after it.
int g_double_scope = 0;  // how many AsDouble are open: a stand-in is "the player" right now (its events are dropped, 0x1A1E40)
struct AsDouble {
    int which;
    uint32_t first = 0;
    AsSecond owner;
    explicit AsDouble(uint32_t player) : which(DoubleOf(player)), owner(which == 1 ? g_second : 0) {
        if (which < 0) return;
        g_double_scope++;
        uint32_t list = kernel::R32(kObjectManager + kPlayerList);
        if (!list) return;
        first = kernel::R32(list);
        kernel::W32(list, g_doubles[which].object);
    }
    ~AsDouble() {
        if (which < 0) return;
        g_double_scope--;
        if (!first) return;
        uint32_t list = kernel::R32(kObjectManager + kPlayerList);
        if (list && g_doubles[which].object && kernel::R32(list) == g_doubles[which].object) kernel::W32(list, first);
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
    float most = 0;       // ... of this most: he comes back with the same share of his new most
    float meter = 0;      // his insanity meter
    bool beside = false;  // a scene was on when the game saved: he comes back beside Player 1, not where he stood
};
Return g_return;
// What Player 2 takes into the next level (a level exit ends him and the next level makes him anew): the share of
// his health and his insanity meter. Until 2026-11-26 he came into every level with full health and an empty meter.
struct Carry {
    bool valid = false;
    float share = 1.0f, meter = 0;
    std::chrono::steady_clock::time_point at{};
};
Carry g_carry;

void SpawnSecond(X86Context& ctx) {
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
    float pos[3];
    for (int i = 0; i < 3; i++) pos[i] = ReadF(g_player_object + kPosition + i * 4) + ReadF(g_player_object + kMatrix + i * 4);  // + right
    std::memcpy(kernel::HostPtr(matrix + 0x30), pos, 12);
    bool back = g_return.valid;
    // back where he stood when the game saved. (Not when a scene was on at that moment: in suffering2_011.log the
    // harbor's ride carried Player 1 55 m, the game saved at its end, and Player 2 was made again at the level's
    // start, 60 m behind, with nothing left to bring him over.)
    if (back && !g_return.beside) {
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
        // His health as the same share of the new Player's most. (Until 2026-11-26 the number itself was written:
        // the new Player's most is the game's 200 until it is set to Player 1's / 1.3 a frame later, "the same
        // share of it", so 154 of 154 came back as 154 of 200 = 118 of 154. A quarter of his health went at every
        // save: four times in suffering2_011.log.)
        float most = ReadF(g_second + 0x1A);
        bool carried = !back && g_carry.valid && std::chrono::steady_clock::now() - g_carry.at < std::chrono::seconds(120);
        if (back && g_return.health > 0 && g_return.most > 0 && most > 0) {
            float health = std::min(most, g_return.health * most / g_return.most);
            std::memcpy(kernel::HostPtr(g_second + 0x16), &health, 4);
            std::memcpy(kernel::HostPtr(g_second + 0x48A), &g_return.meter, 4);
            LOGI("coop: Player 2 is back after the save with the health he had (%.0f of %.0f) and his meter (%.0f)%s", g_return.health, g_return.most,
                 g_return.meter, g_return.beside ? ", beside Player 1: a scene was on" : "");
        } else if (carried && most > 0) {
            float health = std::clamp(g_carry.share, 0.05f, 1.0f) * most;
            std::memcpy(kernel::HostPtr(g_second + 0x16), &health, 4);
            std::memcpy(kernel::HostPtr(g_second + 0x48A), &g_carry.meter, 4);
            LOGI("coop: Player 2 comes into the level with what he left the last one with: %.0f%% of his health, meter %.0f", g_carry.share * 100.0f, g_carry.meter);
        }
        g_carry.valid = false;
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
// Player 2's own look (decided 2026-11-25: each player's settings are his own; since 2026-11-27 both players set
// theirs in the menus: "both players need to be able to change inverted options"). The game has one Invert Pitch
// Axis and one sensitivity, and applies them to every Player: the Pitch state's update (0x2D460) turns the stick's
// value round when bit 0x10 of the options' switches ([0x392158] +0x50) is set. That one is Player 1's (the game's
// Controller Settings, or the Player 1 look row of the Display and Co-op page). Player 2's two are applied to his
// right stick before the game sees it: inverted or not, his own whatever Player 1 has (so his stick is turned round
// when his choice and the game's differ), and how fast he turns, as a share of the game's own speed.
uint32_t g_p2_invert = 0;   // 1: Player 2's up and down are inverted
uint32_t g_p2_speed = 100;  // per cent, 40 to 100
bool GameInverted() {
    constexpr uint32_t kOptions = 0x392158, kSwitches = kOptions + 0x50, kInverted = 0x10, kOptionsMethods = 0x347C44;
    return mem::IsCommitted(kSwitches) && kernel::R32(kOptions) == kOptionsMethods && (kernel::R32(kSwitches) & kInverted) != 0;
}
void LookWith(input::Pad& p, uint32_t invert, uint32_t speed) {
    float k = float(std::clamp<uint32_t>(speed, 40, 100)) / 100.0f;
    float rx = float(p.rx) * k, ry = float(p.ry) * k * (invert ? -1.0f : 1.0f);
    p.rx = int16_t(std::clamp(rx, -32767.0f, 32767.0f));
    p.ry = int16_t(std::clamp(ry, -32767.0f, 32767.0f));
}
void SecondLook(input::Pad& p) { LookWith(p, (g_p2_invert != 0) != GameInverted() ? 1 : 0, g_p2_speed); }  // controller 2
bool g_wanted = false;  // co-op is on now (the setting, or a test's switch)

bool InLevel() { return g_player_object != 0 && g_player != 0; }  // a level is in play (it has its Player)
bool SplitActive() { return g_second_object && ViewCamera2() && !g_one_view; }
bool g_paused_now = false;   // the pause menu is up (someone paused)
bool g_stopped_now = false;  // the game's time is stopped: the pause menu, a movie, a level exit (see the pause, below)
constexpr uint32_t kPlayerMethods = 0x3381D0;
constexpr uint32_t kObjectEnd = 0x1849B0;  // (this = engine object)

bool IsPlayerObject(uint32_t object) {
    if (!object || !mem::Spans(object, 0x110)) return false;
    uint32_t player = kernel::R32(object + kClassObject);
    return player && mem::Spans(player, 0x88F) && kernel::R32(player) == kPlayerMethods && kernel::R32(player + 4) == object;
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
    g_doubles[0] = g_doubles[1] = Double{};  // (ended with the level, or by EndSecond)
    g_doubles_fresh = false;
    g_spare_at = 0;
    g_down_until = 0;
    g_one_view = false;
    game_view::SetSplit(false);
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
// What he holds is ended with him. The weapon in each hand is an object of its own (the handles at +0x3AC and +0x3AE
// of the Player) and not one of his object's children, which 0x1849B0 ends with it: every time he was taken out
// for a save, the gun he held stayed where his hand had been ("there were floating guns around the place",
// 2026-11-26: nine saves in that session; run guns1 shows one).
uint32_t Named(uint32_t player, uint32_t at);
// Player 2's flashlight as it was when he was last taken out (a save, a level's end): 1 on, 0 off, -1 not known. He
// comes back with it that way (FlashlightAsFirst), within two minutes; else as Player 1's is.
int g_torch2 = -1;
std::chrono::steady_clock::time_point g_torch2_at{};
std::vector<std::pair<uint32_t, uint32_t>> g_trace_left;  // (S2_COOP_HELD_TRACE: what was still there right after he was taken out; looked at again 90 frames later)
uint64_t g_trace_left_at = 0;
uint32_t ClassOf(uint32_t object);
constexpr uint32_t kFlashlightClass = 0xCF3A3FA5;
void EndSecond(X86Context& ctx, const char* why) {
    EndDoubles(ctx, why);
    if (uint32_t light = FlashlightOf(g_second)) g_torch2 = int(kernel::R32(light + 0xA8) & 1), g_torch2_at = std::chrono::steady_clock::now();
    uint32_t ecx = ctx.ecx;
    static const bool leave = std::getenv("S2_COOP_LEAVE_GUNS") != nullptr;  // tests: as before 2026-11-26
    static const bool trace = std::getenv("S2_COOP_HELD_TRACE") != nullptr;  // tests: what is near him and names him, before and after
    // What goes with him. Ending a Player ends none of the things he carries: his weapons stayed in the air where he
    // stood (until 2026-11-26), and the grenade on his belt fell to the ground there (seen 2026-11-26 in the pictures
    // of the flashlight tests; it lay there after every save). They are:
    //   - what his own handles name: the weapons in his hands (+0x3AC, +0x3AE) and his flashlight (+0x16C);
    //   - every object near him whose class object names him as the one who carries it: the items (weapons, the
    //     flashlight: the handle at +0x56) and the loose things on him (class BouncingObject 0xEB13B226: the grenade
    //     on his belt, his other weapon's body; the handle at +0x92).
    // His Collider (class FA5951FC, it names him at +0x0C) is the engine's own business and is not touched.
    std::vector<uint32_t> held;
    struct Near {
        uint32_t object, index, cls;
        std::string where;
    };
    std::vector<Near> near;
    if (!leave && g_second && g_second_object) {
        for (uint32_t at : {0x3ACu, 0x3AEu, 0x16Cu})
            if (uint32_t object = Named(g_second, at)) held.push_back(object);
        // (The handle table: entries of six bytes, an object and how many hold its handle, at the object manager
        // +0x0C; how many entries there are at +0x14; an object knows its own handle, the word at +0xE6: 0x18E970.
        // Past the table's end the memory holds other things, old copies of object addresses among them: a scan
        // that ran on to 0x4000 counted Player 2's flashlight twice in some runs.)
        uint32_t table = kernel::R32(kObjectManager + 0x0C), entries = std::min<uint32_t>(kernel::R32(kObjectManager + 0x14), 0x4000);
        uint32_t mine = kernel::R16(g_second_object + 0xE6);
        if (!table || !mine || mine >= entries || kernel::R32(table + mine * 6) != g_second_object) mine = 0;
        for (uint32_t i = 1; table && mine && i < entries && mem::IsCommitted(table + i * 6 + 4); i++) {
            uint32_t object = kernel::R32(table + i * 6);
            // (a free entry holds a small number, the next free one, not an object)
            if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0 || object == g_second_object) continue;
            if (kernel::R16(object + 0xE6) != i) continue;
            float d2 = 0;
            for (int k = 0; k < 3; k++) {
                float d = ReadF(object + kPosition + k * 4) - ReadF(g_second_object + kPosition + k * 4);
                d2 += d * d;
            }
            if (!(d2 <= 16.0f)) continue;  // (four metres: what he carries is on him)
            uint32_t c = kernel::R32(object + kClassObject), cls = ClassOf(object);
            if (!c || !mem::Spans(c, 0x100)) continue;
            bool item = cls == 0x9710D816 || cls == 0x22E48520 || cls == 0xFE9F718D || cls == kFlashlightClass;
            bool loose = cls == 0xEB13B226;
            if ((item && kernel::R16(c + 0x56) == mine) || (loose && kernel::R16(c + 0x92) == mine)) held.push_back(object);
            if (trace) {
                std::string where;
                char one[16];
                for (uint32_t at = 0; at < 0x100; at += 2)
                    if (kernel::R16(c + at) == mine) std::snprintf(one, sizeof one, " +%02X", at), where += one;
                if (!where.empty()) near.push_back({object, i, cls, where});
            }
        }
    }
    for (uint32_t object : {g_camera2_object, g_second_object}) {
        if (!object || int32_t(kernel::R32(object + 8)) < 0) continue;
        ctx.ecx = object;
        kernel::CallGuest(ctx, kObjectEnd, {});
    }
    int ended = 0;
    std::sort(held.begin(), held.end());
    held.erase(std::unique(held.begin(), held.end()), held.end());
    for (uint32_t object : held) {
        if (!object || object == g_second_object || object == g_player_object || int32_t(kernel::R32(object + 8)) < 0) continue;
        // (never what Player 1 holds: the two never share an object, but a wrong handle must not cost him his gun)
        if (g_player && (Named(g_player, 0x3AC) == object || Named(g_player, 0x3AE) == object || Named(g_player, 0x16C) == object)) continue;
        ctx.ecx = object;
        kernel::CallGuest(ctx, kObjectEnd, {});
        ended++;
    }
    if (ended) LOGI("coop: what Player 2 carried goes with him (%d object%s: his weapons, his flashlight, what hung on him)", ended, ended == 1 ? "" : "s");
    for (const Near& n : near) {
        uint32_t table = kernel::R32(kObjectManager + 0x0C);
        bool there = table && kernel::R32(table + n.index * 6) == n.object && int32_t(kernel::R32(n.object + 8)) >= 0;
        LOGI("held trace: object %08X (handle %X) of class %08X named him at%s: %s", n.object, n.index, n.cls, n.where.c_str(), there ? "STILL THERE after he was taken out" : "gone with him");
        if (there) g_trace_left.push_back({n.object, n.index}), g_trace_left_at = g_frames + 90;
    }
    ctx.ecx = ecx;
    SecondGone(ctx, why);
}

// right before the game saves (the hook on 0x1B4D50)
void BeforeSave(X86Context& ctx) {
    EndDoubles(ctx, "the game is saving");
    if (!g_second_object || !IsPlayerObject(g_second_object)) return;
    g_return.valid = true;
    std::memcpy(g_return.matrix, kernel::HostPtr(g_second_object + kMatrix), 64);
    g_return.health = g_down_until ? 0.0f : ReadF(g_second + 0x16);
    g_return.most = ReadF(g_second + 0x1A);
    g_return.meter = ReadF(g_second + 0x48A);
    g_return.beside = g_one_view;
    EndSecond(ctx, "the game is saving: he is not saved");
}

// Player 2 comes in with a copy of what Player 1 carries. A Player's inventory is the list at +0x128 (12 bytes an
// entry: ?, item, how many; a second list of items at +0x0C of it): 0x11380 (this = inventory, another) makes one
// a copy of another, 0x110A0 (item) how many, 0x11220 (item, n) takes away. Then the weapon Player 1 holds is put
// in his hands the way the weapon button does it (0x171F0: 0x17050 picks the next one from the inventory, going on
// from the one held, so it finds none for empty hands; 0x14F30 and 0x16320 put it in his hands).
// (Everything is copied for now, quest items too: the Player 1 only rule for those is still to come.)
constexpr uint32_t kInventory = 0x128, kInventoryCopy = 0x11380;
constexpr uint32_t kEquip = 0x14F30;  // (this = Player, weapon's template, slot): puts a weapon in his hands, as 0x171F0 does
uint64_t g_arm_at = 0;
// Player 2's weapon loads itself from what he carries, within a second of being put in his hands: the ammunition he
// had just been given went into the empty gun and he was left with one clip and nothing to reload with, where Player
// 1 has a loaded gun AND the same spare (found 2026-11-26: after eight shots Player 2's pistol stayed empty, run
// ammo2). Once his weapon is loaded, what the loading took is given again.
std::vector<std::pair<uint32_t, uint32_t>> g_spare;  // item, how many: what Player 1 carried when Player 2 was armed
uint64_t g_spare_at = 0;
uint32_t ClassOfTemplate(uint32_t index);
uint32_t ClassOf(uint32_t object);
const char* KindName(uint32_t cls);            // what the players use, by its class: "a ladder", "a ledge" (nullptr: none of them)
bool Whole(uint32_t p, uint32_t size);  // both ends of an object in committed memory (DetailFrame)
uint32_t Named(uint32_t player, uint32_t at);  // the object a Player's handle names (+0x33E: what he is using, +0x340: what he could use)
bool ForBoth(uint32_t cls);
void Spare(X86Context& ctx);
void FlashlightAsFirst(X86Context& ctx);
uint32_t g_hints_after = 0;  // Player 2's marks for the hints shown, set once his things are given (Arm)
void Arm(X86Context& ctx) {
    uint32_t ecx = ctx.ecx;
    // Each thing Player 1 carries that is for both (weapons, ammunition, grenades, health: ForBoth) is GIVEN to Player
    // 2 the game's own way (0x153A0, this = Player; item, how many), and then his count of it is set to Player 1's.
    // Until 2026-11-25 the list was copied as bytes (0x11380): he had the numbers but nothing on him showed them
    // (no grenade on his belt, empty hands), and he got Player 1's keys and notes as well, against the rule that
    // those are Player 1's alone.
    std::vector<std::pair<uint32_t, uint32_t>> items;
    {
        uint32_t list = kernel::R32(g_player + kInventory), n = kernel::R32(g_player + kInventory + 8);
        for (uint32_t i = 0; list && i < n && i < 256; i++) {
            uint32_t item = kernel::R32(list + i * 12 + 4), count = kernel::R32(list + i * 12 + 8);
            if (int32_t(count) > 0 && ForBoth(ClassOfTemplate(item))) items.push_back({item, count});
        }
        AsSecond as(g_second);
        // A Player keeps a mark for each of the game's hints he has been shown (+0xB8, one bit a hint; 0x2EE80, this =
        // Player, hint, - shows a hint once and sets its bit). Player 2 is made anew after every save and level
        // change, so being given his things showed "Use Y to switch between your two current weapons" each time,
        // and with a flashlight of his own "Use right thumbstick to turn your flashlight on and off" as well. While
        // he is given his things every hint counts as shown; afterwards he has Player 1's marks, and those two.
        constexpr uint32_t kHintsShown = 0xB8;
        kernel::W32(g_second + kHintsShown, 0xFFFFFFFF);
        for (const auto& [item, count] : items) {
            ctx.ecx = g_second;
            kernel::CallGuest(ctx, 0x153A0, {item, count});
        }
        g_hints_after = kernel::R32(g_player + kHintsShown) | (1u << 2) | (1u << 4);
        uint32_t mine = kernel::R32(g_second + kInventory), mine_n = kernel::R32(g_second + kInventory + 8);
        for (uint32_t i = 0; mine && i < mine_n && i < 256; i++)
            for (const auto& [item, count] : items)
                if (kernel::R32(mine + i * 12 + 4) == item) kernel::W32(mine + i * 12 + 8, count);
        LOGI("coop: Player 2 is given %zu kinds of things Player 1 carries (weapons, ammunition, grenades, health)", items.size());
    }
    {
        // the weapon Player 1 holds (the object in his right hand, handle at +0x3AC; an item is its template's number)
        uint32_t table = kernel::R32(kObjectManager + 0x0C), handle = kernel::R32(g_player + 0x3AC) & 0xFFFF;
        uint32_t held = table && handle ? kernel::R32(table + handle * 6) : 0;
        if (held) {
            uint32_t weapon = kernel::R32(held + kTemplateWord) & 0xFFFF;
            // (0x14F30 puts it in his hands. 0x16320, called after it until 2026-11-26 because the weapon button
            // does, only makes the notice on the HUD that names the weapon: not for Player 2's.)
            AsSecond as(g_second);
            ctx.ecx = g_second;
            kernel::CallGuest(ctx, kEquip, {weapon, 1});
        }
    }
    FlashlightAsFirst(ctx);
    LOGI("coop: Player 2's marks for the hints he has been shown: %08X (Player 1's %08X, and the two for what he was just given)", g_hints_after, kernel::R32(g_player + 0xB8));
    kernel::W32(g_second + 0xB8, g_hints_after);
    // (what he was given, for Spare below: his weapon loads itself from it a moment later)
    g_spare = items;
    g_spare_at = g_frames + 90;
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

void Spare(X86Context& ctx) {
    if (!g_spare_at || g_frames < g_spare_at) return;
    if (!g_second || !mem::Spans(g_second, 0x490)) {
        g_spare_at = 0;
        return;
    }
    // (not before the weapon is loaded; and not for ever: he may have none that loads)
    if (int32_t(kernel::R32(g_second + 0x140)) <= 0 && g_frames < g_spare_at + 300) return;
    g_spare_at = 0;
    uint32_t ecx = ctx.ecx;
    AsSecond as(g_second);
    int again = 0;
    for (const auto& [item, count] : g_spare) {
        ctx.ecx = g_second + kInventory;
        uint32_t have = kernel::CallGuest(ctx, 0x110A0, {item});
        if (have >= count) continue;
        ctx.ecx = g_second;
        kernel::CallGuest(ctx, 0x153A0, {item, count - have});
        again += int(count - have);
    }
    ctx.ecx = ecx;
    if (again) LOGI("coop: Player 2's weapon loaded itself from what he was given: the %d it took are given again (he has Player 1's spare ammunition too)", again);
}

// A scene: the game's Camera is not following Player 1 (the top of its mode stack is not 3: a script or a cut-scene
// has pushed a mode of its own; or it follows someone else). The picture is one view then, Player 1's, as in the Silent Hill 2 co-op mod; the two
// views come back when the Camera follows again. Player 2's view camera stays in the list and is skipped by the
// engine (its stand-in texture reads as shown by nothing).
const char* g_scene_why = "";  // which sign of a scene InScene saw last (for the log)
bool InScene() {
    // The story holds Player 1: he sits at the chess table the game opens at, is walked along the dock, stands
    // through the moment in the apartment. The camera stays the following one and no sequence moves it, so none of
    // the signs below shows it, and in the online session of 2026-11-26 Player 2 walked about through all three
    // ("Player 2 was still able to walk around while I was sitting", "the same issue happened at the dock"). The mark
    // is in Player 1's flags at +0xB0: bit 0x02000000 set with bit 0 (his controls) clear. In the once-a-second
    // state lines of that session's log and the one before (3,500 lines) it is set through all five holds, 367.8 to
    // 413.1 s, 873.0 to 972.6 s, 1581.2 to 1600.1 s and 123.4 to 138.4 s, 531.8 to 562.8 s, and never outside one:
    // not on any of the 287 lines where he was on a door, ladder, switch or ledge (those keep bit 0 set).
    static uint32_t held = 0;
    {
        uint32_t b0 = g_player && mem::Spans(g_player, 0xC0) ? kernel::R32(g_player + 0xB0) : 0;
        held = (b0 & 0x02000000) && !(b0 & 1) ? held + 1 : 0;
    }
    uint32_t object = kernel::R32(kGameCamera);
    uint32_t camera = object && mem::Spans(object, 0x110) ? kernel::R32(object + kClassObject) : 0;
    if (!camera || !mem::Spans(camera, 0x40)) return g_scene_why = "no game camera", true;
    uint32_t stack = kernel::R32(camera + 0x20), count = kernel::R32(camera + 0x28);
    static uint32_t mode_seen = 0;
    uint32_t mode = stack && count && count <= 64 ? kernel::R32(stack + count * 8 - 4) : 99;
    if (mode != mode_seen) LOGI("coop: the game camera's mode is %u (was %u; %u on its stack)", mode, mode_seen, count), mode_seen = mode;
    // Mode 3 follows the Player; mode 0 leaves the camera to whoever moves it, and that is the Player himself in
    // first person (d-pad up puts a mode 0 on the stack). Until 2026-11-25 mode 0 counted as a scene: Player 1
    // looking through his own eyes took Player 2's view away and hid him. A scene that uses mode 0 shows by one of
    // the signs below.
    // Player 1 at a security monitor (bit 0x400000 of his flags at +0xBC) is not a scene: the camera shows the
    // monitor's picture in his view, and Player 2 keeps his own view and his controls. (In the first online session,
    // 2026-11-25, a monitor took Player 2's view away and put him beside Player 1 afterwards.)
    static const bool nohold = std::getenv("S2_TEST_NO_HOLD") != nullptr;  // tests: as before 2026-11-26
    if (held > 12 && !nohold) return g_scene_why = "the story holds Player 1 (he sits, or is walked along)", true;
    if (g_player && (kernel::R32(g_player + 0xBC) & 0x400000)) return false;
    // Player 1 in an enemy's grip is not a scene either (the owner, 2026-11-28: "if player 1 is grabbed, player 2
    // cant move"): the grab moves the camera like a sequence does, and Player 2 lost his view and his controls while
    // he could have helped. Player 1's states HeadGrab (33) and BodySlam (38), bits 1 and 6 of +0xC0, and Hit
    // Reacting (16) mark it; a story scene holds him in Controlled (11) alone.
    if (g_player && mem::Spans(g_player, 0xC4) && ((kernel::R32(g_player + 0xC0) & 0x42) || (kernel::R32(g_player + 0xBC) & 0x10000))) {
        static uint64_t said = 0;
        if (g_frames - said > 600) said = g_frames, LOGI("coop: Player 1 is held by an enemy (states %08X %02X): not a scene, Player 2 plays on", kernel::R32(g_player + 0xBC), kernel::R32(g_player + 0xC0) & 0x7F);
        return false;
    }
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
    // (Since 2026-11-26 bit 0x40000 is not a scene either. It is the Player's state 18, entered with 0x25CD0(0x12)
    // from two places only, both changes of form: 0x2B230 (the story's, and the change back) and 0x1C21E. Counted as
    // a scene, every change of Player 1's form took Player 2's view for up to three seconds and, when he was more
    // than 12 m away, put him beside Player 1: the same fault as the doors. Real scenes are found by the rules above.)
    // ... or Player 1 is carried off on something he is mounted on (bit 0x200000 of the same flags is set from the
    // moment he is on the truck's gun in chapter 10; run truck1: the truck drove him 250 m away and Player 2 stood
    // where the level began). A mount that stays where it is leaves Player 2 free: it only counts once he is more
    // than 15 m behind, and then until Player 1 is off the mount. He comes back beside Player 1 as after a scene.
    // (Until 2026-11-25 "Player 1 sits" counted as a scene too: bit 0x200000 without bit 1 of +0xB0, as at the chess
    // table the game opens at. But the game sets exactly that for a second or two on every door, ladder, switch and
    // monitor Player 1 uses: in the first online session it made 34 "scenes" in twenty minutes, and each one took
    // Player 2's view, hid him and put him back beside Player 1, sometimes inside a wall. It is not a scene.)
    // (And only a mount that moves him: until 2026-11-26 the rule was "mounted, and Player 2 more than 15 m away".
    // But every door, ladder and switch is a mount for a second or two. With the two players apart, each door Player 1
    // opened took Player 2's view for two seconds and put him beside Player 1: 16 times in the 15 minutes of
    // suffering2_011.log. Now Player 1 must also be 25 m from where he got on.)
    static bool riding = false, was_mounted = false;
    static float got_on[3] = {0, 0, 0};
    bool mounted = g_player && (kernel::R32(g_player + 0xBC) & 0x200000);
    if (mounted && !was_mounted && g_player_object)
        for (int i = 0; i < 3; i++) got_on[i] = ReadF(g_player_object + kPosition + i * 4);
    was_mounted = mounted;
    if (!mounted || !g_second_object) riding = false;
    else if (!riding && g_player_object) {
        float d = 0, moved = 0;
        for (int i = 0; i < 3; i++) {
            float v = ReadF(g_player_object + kPosition + i * 4) - ReadF(g_second_object + kPosition + i * 4);
            d += v * v;
            float m = ReadF(g_player_object + kPosition + i * 4) - got_on[i];
            moved += m * m;
        }
        riding = d > 15.0f * 15.0f && moved > 25.0f * 25.0f;
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
// Where: a place Player 1 has just walked over (his trail: a point every half metre, the last sixty), the newest
// that is between 0.9 and 4 m from where he stands now and about level with him. Such a place is inside the level
// and can be stood on. Until 2026-11-25 it was one metre along Player 1's right whatever was there: after the dock
// scene of the first online session that was outside the docks, and elsewhere inside walls. With no such point
// (a scene has just set Player 1 down somewhere new) it is Player 1's own place.
// The game hears from one object: the sound device ([0x398CB0]) keeps its handle at +0x42 (0x1FEF80 reads its place;
// with no such object the listening point is 0, 0, 0). A Camera's set-up makes itself that object, so with Player 2
// in, the game heard from HIS camera, and once he was out again (co-op switched off in a level, a save, a level
// exit) the handle was of an object that had ended: the game then heard from the origin and refused every sound
// made for a place in the level as too far away. The owner's session of 2026-11-25 lost most sounds that way; run
// onoff: 69 plays refused against 1. The listening object is kept to be the game's own Camera (Player 1's), every
// frame. Both players hear the same sound, the host's.
void Ears() {
    uint32_t device = kernel::R32(0x398CB0), camera = kernel::R32(kGameCamera);
    static const bool trace = std::getenv("S2_EARS_TRACE") != nullptr;
    if (trace && g_frames % 120 == 0 && device && mem::Spans(device, 0x44)) {
        uint32_t table = kernel::R32(kObjectManager + 0x0C), handle = kernel::R16(device + 0x42);
        uint32_t object = table && handle ? kernel::R32(table + handle * 6) : 0;
        LOGI("coop: ears trace: device %08X handle %04X object %08X at %.1f %.1f %.1f; game camera %08X (handle %04X), Player 1 object %08X (handle %04X)", device, handle,
             object, object && object > 0x400000 ? ReadF(object + kPosition) : 0.0f, object && object > 0x400000 ? ReadF(object + kPosition + 4) : 0.0f,
             object && object > 0x400000 ? ReadF(object + kPosition + 8) : 0.0f, camera, camera ? kernel::R16(camera + 4) : 0, g_player_object,
             g_player_object ? kernel::R16(g_player_object + 4) : 0);
    }
    if (!device || !mem::Spans(device, 0x44) || !camera || !mem::Spans(camera, 0x60)) return;
    if (camera == g_camera2_object) return;  // (Player 2's code is running with his Camera as the game's: not now)
    // (the handle is the object's place in the object manager's table, six bytes an entry, the object first)
    uint32_t table = kernel::R32(kObjectManager + 0x0C), have = kernel::R16(device + 0x42);
    if (!table || (have && mem::IsCommitted(table + have * 6 + 4) && kernel::R32(table + have * 6) == camera)) return;
    for (uint32_t i = 1; i < 4096 && mem::IsCommitted(table + i * 6 + 4); i++) {
        if (kernel::R32(table + i * 6) != camera) continue;
        kernel::W16(device + 0x42, uint16_t(i));
        static int said = 0;
        if (said++ < 20) LOGI("coop: the game listens from its own Camera again (object handle %04X, was %04X)", i, have);
        return;
    }
}
struct TrailPoint {
    float at[3];
};
std::vector<TrailPoint> g_trail;
void TrailStep() {
    if (!g_player_object || !mem::Spans(g_player_object, 0x60)) {
        g_trail.clear();
        return;
    }
    TrailPoint now;
    for (int i = 0; i < 3; i++) now.at[i] = ReadF(g_player_object + kPosition + i * 4);
    if (!g_trail.empty()) {
        float d = 0;
        for (int i = 0; i < 3; i++) d += (now.at[i] - g_trail.back().at[i]) * (now.at[i] - g_trail.back().at[i]);
        if (d < 0.5f * 0.5f) return;
        if (d > 6.0f * 6.0f) g_trail.clear();  // (he was moved, not walking: what is behind him is another place)
    }
    g_trail.push_back(now);
    if (g_trail.size() > 60) g_trail.erase(g_trail.begin());
}
float FromFirst(uint32_t object) {  // metres between Player 1 and an object
    float d = 0;
    for (int i = 0; i < 3; i++) {
        float v = ReadF(g_player_object + kPosition + i * 4) - ReadF(object + kPosition + i * 4);
        d += v * v;
    }
    return std::sqrt(d);
}
void BesideFirst(X86Context& ctx) {
    if (!g_second || !g_second_object || !g_player_object) return;
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
    float first[3], place[3];
    for (int i = 0; i < 3; i++) place[i] = first[i] = ReadF(g_player_object + kPosition + i * 4);
    const char* how = "Player 1's own place";
    for (size_t k = g_trail.size(); k-- > 0;) {
        const float* t = g_trail[k].at;
        float flat = (t[0] - first[0]) * (t[0] - first[0]) + (t[2] - first[2]) * (t[2] - first[2]), up = std::fabs(t[1] - first[1]);
        if (flat < 0.9f * 0.9f || flat > 4.0f * 4.0f || up > 1.2f) continue;
        std::memcpy(place, t, sizeof place);
        how = "where Player 1 has just walked";
        break;
    }
    LOGI("coop: Player 2 is put at %.1f %.1f %.1f (%s; Player 1 is at %.1f %.1f %.1f)", place[0], place[1], place[2], how, first[0], first[1], first[2]);
    for (int i = 0; i < 3; i++) std::memcpy(kernel::HostPtr(matrix + 0x30 + i * 4), &place[i], 4);
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
// The partner-down sound (decided 2026-11-24: the game's own tonal hum, twice back to back). The game plays a
// sound by its number in its package: the Player's package is 0x3C and the hum is its sound 0xF4, so 0x003C00F4 (in
// the package's sound file, Common/Player/Player.sdu, the wave has the same 0xF4 under another upper half,
// 0x00A500F4, which is what the PS2 extraction named it by; asking for that number gives a voice that never
// sounds). It is a wave that repeats, one pulse a second: it is started with the game's own play call (no place,
// flag 4: heard whatever the distance) and stopped by the game's own stop call (0x190F40: the voice, a fade in
// seconds) after two pulses. Both players hear it: the joined player hears the host's sound.
constexpr uint32_t kDownSound = 0x003C00F4, kStopSound = 0x190F40;
constexpr int kDownSoundMs = 1900;  // (the third pulse would begin at 2000)
uint32_t PlaySound(X86Context& ctx, uint32_t sound, bool at_player, uint32_t flags = 2);
std::chrono::steady_clock::time_point g_down_sound_stop{};
uint32_t g_down_voice = 0;
void DownSoundStop(X86Context& ctx, float fade) {
    uint32_t manager = kernel::R32(0x384BBC), ecx = ctx.ecx, bits;
    std::memcpy(&bits, &fade, 4);
    if (manager && g_down_voice) {
        ctx.ecx = manager;
        kernel::CallGuest(ctx, kStopSound, {g_down_voice, bits});
        ctx.ecx = ecx;
    }
    g_down_voice = 0;
    g_down_sound_stop = {};
}
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
    if (g_down_voice && std::chrono::steady_clock::now() >= g_down_sound_stop) DownSoundStop(ctx, 0.15f);
    if (!g_down_until) {
        bool fallen = g_player_object && ReadF(g_second_object + kPosition + 4) < ReadF(g_player_object + kPosition + 4) - kFallen;
        if (!(most > 0 && health <= most * kDownAt) && !fallen) return;
        g_down_until = g_frames + (fallen ? 1 : kDownFrames);
        LOGI("coop: Player 2 is down (%s; health %.0f of %.0f)", fallen ? "fell out of the level" : "health", health, most);
        show(false);
        static const bool quiet = std::getenv("S2_COOP_NO_DOWN_SOUND") != nullptr;
        if (!quiet) {
            if (g_down_voice) DownSoundStop(ctx, 0.0f);
            g_down_voice = PlaySound(ctx, kDownSound, false, 4);
            g_down_sound_stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDownSoundMs);
            LOGI("coop: the partner-down sound (the tonal hum, two pulses; voice %08X)", g_down_voice);
        }
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
    // In a scene Player 2 is not to be seen. After it he is where he was, unless the scene took Player 1 away
    // (more than 12 m from him): then he comes to Player 1. (Until 2026-11-25 he was put beside Player 1 after
    // every scene, however short.)
    if (g_second_object && !Down()) {
        if (!one && g_player_object && FromFirst(g_second_object) > 12.0f) BesideFirst(ctx);
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
        uint32_t camera = object && mem::Spans(object, 0x110) ? kernel::R32(object + kClassObject) : 0;
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
    {   // the record: Player 2's own Camera, when its mode, its flags or what it follows changes (the game's camera has
        // its line in InScene). Asked for on 2026-11-26: "there was a bug where his camera became fixed", and the
        // log of that session had nothing about his camera.
        static uint64_t before = ~0ull;
        uint32_t camera = g_camera2_object && mem::Spans(g_camera2_object, 0x110) ? kernel::R32(g_camera2_object + kClassObject) : 0;
        if (camera && mem::Spans(camera, 0x40)) {
            uint32_t stack = kernel::R32(camera + 0x20), n = kernel::R32(camera + 0x28);
            uint32_t mode = stack && n && n <= 64 ? kernel::R32(stack + n * 8 - 4) : 99, cflags = kernel::R32(camera + 0x2C);
            uint32_t table = kernel::R32(kObjectManager + 0x0C), handle = kernel::R32(camera + 0x38) & 0xFFFF;
            uint32_t tracked = table && handle ? kernel::R32(table + handle * 6) : 0;
            uint64_t now = (uint64_t(mode) << 56) ^ (uint64_t(n) << 48) ^ (uint64_t(cflags) << 16) ^ tracked;
            if (now != before)
                LOGI("coop: Player 2's camera: mode %u (%u on its stack), flags %08X, follows %s", mode, n, cflags,
                     tracked == g_second_object ? "Player 2" : tracked == g_player_object ? "Player 1" : tracked ? "something else" : "nothing");
            before = now;
        } else before = ~0ull;
    }
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
        // (the record: one scene's sign gives way to another's while the picture stays one view, as when the
        // opening's sequence ends and Player 1 is left sitting at the chess table)
        static const char* why_said = "";
        if (g_one_view && !clear && g_scene_why != why_said && *why_said) LOGI("coop: still one view, now because: %s", g_scene_why);
        why_said = g_one_view && !clear ? g_scene_why : g_one_view ? why_said : "";
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
    Spare(ctx);
    Doubles(ctx);
}

// The teleport: Player 2 holds A, B, X and Y together for five seconds and is put beside Player 1 (never the other
// way). The rule as decided on 2026-11-24 (a keyboard's T for five seconds did the same for a joined player while
// online co-op existed); until 2026-11-25 it was a press of BACK, which is too easy to hit. While all four
// are down the game does not see them (no jump, use and reload at once). His START pauses for both players: the
// game takes START from controller 1 only, so pad.cpp passes it on there. His BACK the game does not see (the map
// is Player 1's).
std::atomic<bool> g_teleport2{false}, g_start2{false};
bool SecondStart() { return g_start2.load(); }
// Pause by either player (decided 2026-11-25: "if it pauses for one player, it should pause for both", and the other
// player is told who is in the pause menu, in the game's own menu look). The game takes its menus' input from
// controller 1. So: whose START opened the pause menu is remembered (g_pauser), and while Player 2's pause is up
// his controller is handed to the game as controller 1 (pad.cpp), Player 1's is not. The pause screen's own title
// then reads "PLAYER 2 PAUSED" (menu.cpp, PauseTitle): one screen for both, in the game's font.
std::atomic<int> g_pauser{0};       // 0 not paused, 1 or 2
input::Pad g_pad2_raw;              // controller 2 as polled, before anything is taken from it (game thread)
std::chrono::steady_clock::time_point g_start2_at{};
bool PausedBySecond() { return g_pauser.load() == 2; }
input::Pad SecondRaw() { return g_pad2_raw; }
int Pauser() { return g_pauser.load(); }
void Controller2(input::Pad& p) {
    constexpr uint16_t kStart = 16, kBack = 32;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point since;
    static bool held = false, done = false;
    bool all = p.analog[0] > 100 && p.analog[1] > 100 && p.analog[2] > 100 && p.analog[3] > 100;
    if (all && !held) since = Clock::now(), done = false;
    held = all;
    if (all) {
        if (!done && Clock::now() - since >= std::chrono::seconds(5)) g_teleport2 = true, done = true;
        p.analog[0] = p.analog[1] = p.analog[2] = p.analog[3] = 0;
    }
    g_pad2_raw = p;
    g_start2 = g_second_object && (p.buttons & kStart) != 0;
    if (g_start2) g_start2_at = Clock::now();
    if (g_second_object) p.buttons &= ~(kBack | kStart);
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
void TestLineup(X86Context& ctx);
void TestPut(X86Context& ctx);
void TestSpot(X86Context& ctx);
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
            if (listed++ < 400) LOGI("coop: test: template %u is a %s", i, kEnemies[k].name);
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

// Tests: S2_TEST_LINEUP=<frame>,<first>,<how many>[,<metres>]: at that frame the level's enemy and character
// templates (the classes TestEnemy knows, in the order the level lists them) number <first> onwards are made in a
// row across Player 1's view, 1.5 m apart, <metres> (5) in front of him, each with a log line: for looking at how
// every kind is drawn (the owner saw two kinds with a wrong texture, 2026-11-25).
void TestLineup(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_LINEUP");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    static const struct {
        uint32_t cls;
        const char* name;
    } kKinds[] = {{0x4E28364F, "Slayer"}, {0x127243E5, "Mainliner"}, {0x285A9A1F, "Burrower"}, {0x137B74CF, "InfernaBurrower"},
                  {0x26EB0742, "Wretch"}, {0xDB1BE225, "CartWheeler"}, {0xE2EBCB51, "Fester"}, {0x0E15C87E, "Unspeakable NPC"}, {0xC3F8C321, "NPC"}};
    int first = 0, many = 8;
    float far = 5.0f;
    if (const char* a = std::strchr(v, ',')) {
        first = std::atoi(a + 1);
        if (const char* b = std::strchr(a + 1, ',')) {
            many = std::atoi(b + 1);
            if (const char* c = std::strchr(b + 1, ',')) far = float(std::atof(c + 1));
        }
    }
    static const bool creatures = std::getenv("S2_TEST_LINEUP_CREATURES") != nullptr;  // (without the human characters)
    uint32_t table = kernel::R32(0x384BA4), count = table ? kernel::R32(table + 0x18) : 0;
    std::vector<std::pair<uint32_t, const char*>> list;
    for (uint32_t i = 0; i < count && i < 0x8000; i++) {
        uint32_t cls = ClassOfTemplate(i);
        for (const auto& k : kKinds)
            if (cls == k.cls && !(creatures && k.cls == 0x0E15C87E)) list.push_back({i, k.name});
    }
    LOGR("lineup: the level has %zu such templates; making %d from number %d", list.size(), many, first);
    static uint32_t matrix = mem::AllocSystem(64);
    uint32_t state = kernel::R32(kObjectManager + kManagerState);
    for (int k = 0; k < many && size_t(first + k) < list.size(); k++) {
        std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(g_player_object + kMatrix), 64);
        // (across the VIEW: away from the game's camera through Player 1, not along the way Player 1 faces, which
        // in the first try put them all behind the camera)
        static const float gap = std::getenv("S2_TEST_LINEUP_GAP") ? float(std::atof(std::getenv("S2_TEST_LINEUP_GAP"))) : 1.6f;
        float pos[3], side = (float(k) - float(many - 1) * 0.5f) * gap, dir[3] = {1, 0, 0};
        uint32_t cam = kernel::R32(kGameCamera);
        if (cam && mem::Spans(cam, 0x60)) {
            dir[0] = ReadF(g_player_object + kPosition) - ReadF(cam + kPosition), dir[2] = ReadF(g_player_object + kPosition + 8) - ReadF(cam + kPosition + 8);
            float len = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
            if (len > 0.01f) dir[0] /= len, dir[2] /= len;
        }
        float right[3] = {-dir[2], 0, dir[0]};
        for (int i = 0; i < 3; i++) pos[i] = ReadF(g_player_object + kPosition + i * 4) + far * dir[i] + side * right[i];
        std::memcpy(kernel::HostPtr(matrix + 0x30), pos, 12);
        kernel::W32(kObjectManager + kManagerState, state & ~2u);
        uint32_t made = kernel::CallGuest(ctx, kSpawn, {g_player_object, matrix, 0, list[size_t(first + k)].first});
        LOGR("lineup: place %d: template %u, a %s, object %08X", k, list[size_t(first + k)].first, list[size_t(first + k)].second, made);
    }
    kernel::W32(kObjectManager + kManagerState, (kernel::R32(kObjectManager + kManagerState) & ~2u) | (state & 2));
}

// Tests: S2_COOP_FACE=1 keeps Player 2 turned to the enemy S2_COOP_ENEMY made (four times a second, the game's
// own move, without ending what he is doing), so that a script that only holds his trigger hits it.
void TestFace(X86Context& ctx) {
    static const bool on = std::getenv("S2_COOP_FACE") != nullptr;
    if (!on || !g_test_enemy || !g_second || !g_second_object || g_frames % 15) return;
    if (!mem::Spans(g_test_enemy, 0x110) || (kernel::R32(g_test_enemy + 8) & 0x80010000)) return;
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
}
namespace game_menu { void PauseTitle(X86Context& ctx, int pauser); }
namespace game_coop {
void TestExit(X86Context& ctx);
void TestCensus();
void TestMeter(X86Context& ctx);
void TestCall(X86Context& ctx);
void TestBody();
void TestSound(X86Context& ctx);
void TestSounds(X86Context& ctx);
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
// ---- the stand-in bodies --------------------------------------------------------------------------------------------
// In first person the game takes the Player's body off him: 0x27760 gives his object the first-person model (two arms,
// model 0x3C0002 in place of the body 0x3C0001, kept at Player +0x764) with a skeleton of its own. With two players
// the other one saw a pair of arms in the air ("the first person i still only see his arms", "it should be the same
// thing im seeing as if hes 3rd person", 2026-11-26). The body the other player sees is a stand-in: one more Player
// object from the same template for each player, worked like a puppet.
//   - They are made once, half a second after both players are in, and put away (not out: not drawn, no Run). A
//     player's is taken out when he goes into first person and put away again when he leaves it or the picture
//     becomes one view. They end with Player 2 (a save, a level exit, co-op off) and are made again after him. Why
//     not made when they are needed: see SpawnDouble.
//   - They are not on the players' list (nothing counts them as players) and their input user's device is one that
//     does not exist: no button reaches them, but the few it does with its owner (below).
//   - While one is out its Run runs, as "the player" (AsDouble) with its owner's walk stick: it stands when he stands
//     and runs when he runs. It has no Detect (the hook on 0x24B00), so it has no movement of its own and meets
//     nothing. Two things were found the hard way:
//       With its Detect running and the stand-in standing in its owner, he could not walk (8 cm where he walked 3 m).
//       A Player is moved by a second object of his own (class Collider, made by 0x1CB80, see ProxyOf); a sweep that
//       finds another player's Collider leaves contacts in THAT player's body too (he is told of the touch, 0x1706F0),
//       and his solver (0x19E580) then takes his whole step back. That is also why two players cannot walk through
//       each other.
//       Put on its owner every frame the game's way (0x2F060) it never landed: 0x2F060 clears the on-the-ground bit
//       (Player +0xA8 bit 0), so it stayed in the Fall state (state 9, Player +0xBC bit 9) and its Run dropped it
//       faster each frame, up to 1.03 m a frame: it was drawn sunk to the waist, its pistol a metre under its hand.
//   - It is put where its owner is twice a frame (PinDoubles).
//   - It holds the kind of weapon its owner holds (0x14F30, again when he changes weapons).
//   - It sends no game events and has no HUD parts (the hooks on 0x1A1E40 and 0x1DE10, below).
//   - Nothing finds it: its object carries the engine's "not for questions" mark, touches with it are refused both
//     ways (RefuseTouch, 0x1706F0) and nothing hurts it (Player +0x1E; the hook on 0x28E00 says so if anything tries).
//   - It is drawn in every view but its owner's, and its owner's arms and weapon in his own view only (0x1D1970).
//   - It does what its owner does (since 2026-11-27; "it needs to show him jumopiung too", "and doing interactable
//     stuff"): it reads his controller for jump, fire, reload and the second attack (DoubleAction), is on the ground
//     and lands when he does (the hook of 0x25480), and takes over his Aim, Attack, Reload and second-attack states
//     (MirrorStates). Its weapon fires blanks and its blows meet nothing ("the messages a stand-in body sends").
//   - It throws and crouches with him (2026-11-27): the thing he has ready to throw hangs on its belt too, it takes
//     it in hand and lets go when he does, for show (DoubleTakesInHand), and it crouches while he holds the button.
//   - It does not use things, and need not: the game takes a player out of first person to use anything. Its table
//     of states (0x371220, 39 x 39: the state that begins, the state that is on; 0 "may not begin", 1 "both",
//     2 "ends the other") has Interact (what the use button does: switches, doors, boxes, ledges, ladders), Item
//     Pickup, View Monitor, Head Grab and the scripted states end First Person, and Ladder Climb may not begin
//     while it is on. So his own body does it, in his own place, and the stand-in is put away for as long (runs
//     fpbox1 and fpledge1: First Person ends in the frame Interact begins; seen in pictures in fpledge2). After a
//     ledge he is in first person again by himself.
// A player in creature form has none.
//   S2_COOP_NO_DOUBLE=1: none (as before 2026-11-26)      S2_COOP_DOUBLE_NO_WEAPON=1: empty hands
//   S2_DOUBLE_NO_THROW=1: nothing on its belt, so no throw (as before 2026-11-27)
//   S2_COOP_DOUBLE_SOLID=1: without the "not for questions" mark      S2_COOP_DOUBLE_EVENTS=1: their events go out
bool g_double_off = std::getenv("S2_COOP_NO_DOUBLE") != nullptr;
int DoubleOf(uint32_t player) {  // which stand-in this Player (class object) is: 0 Player 1's, 1 Player 2's, -1 neither
    if (!player) return -1;
    return g_doubles[0].player == player ? 0 : g_doubles[1].player == player ? 1 : -1;
}
int DoubleObject(uint32_t object) {  // the same, by its engine object
    if (!object) return -1;
    return g_doubles[0].object == object ? 0 : g_doubles[1].object == object ? 1 : -1;
}
bool OfDouble(const Double& d, uint32_t object) {  // its body, what it holds, or what hangs on it
    return d.object && object && (object == d.object || object == d.held[0] || object == d.held[1] || object == d.loose[0] || object == d.loose[1]);
}
bool DoublesThing(uint32_t object) {  // a stand-in body, or what one holds
    if (!object) return false;
    for (const Double& d : g_doubles)
        if (OfDouble(d, object)) return true;
    return false;
}
// A Player is moved by a second object of his own (made by 0x1CB80 from the template his property 0x78 names, kept
// by the handle at body +0xD8, the body being [Player +0xF0]): it is that object that is swept through the world.
// (A stand-in has none as long as its Detect never runs; 0x1CB80 is also called from 0x41770 and 0x45890, so the
// code below looks for one all the same.)
uint32_t ProxyOf(uint32_t player) {
    if (!player || !mem::Spans(player, 0xF4)) return 0;
    uint32_t body = kernel::R32(player + 0xF0);
    if (!body || !mem::Spans(body, 0x130)) return 0;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = kernel::R16(body + 0xD8);
    uint32_t object = table && h && h < kernel::R32(kObjectManager + 0x14) ? kernel::R32(table + h * 6) : 0;
    return object && Whole(object, 0x114) ? object : 0;  // (both ends: see Named)
}
void OffTheList(uint32_t object) {
    uint32_t list = kernel::R32(kObjectManager + kPlayerList), count = kernel::R32(kObjectManager + kPlayerCount);
    for (uint32_t i = 0; list && i < count && i < 8; i++) {
        if (kernel::R32(list + i * 4) != object) continue;
        for (uint32_t k = i; k + 1 < count; k++) kernel::W32(list + k * 4, kernel::R32(list + k * 4 + 4));
        kernel::W32(kObjectManager + kPlayerCount, count - 1);
        return;
    }
}
// What a Player has ready to throw hangs from a point of his skeleton (a grenade on his belt): the point's number is
// property 0x11 of his model's set (0x26090, this = Player: the set is another in first person, 0x25FE0), and the
// object at a point is the skeleton's to say (0x188130, this = the skeleton at [object +0xE8]; the point).
uint32_t ReadyPoint(X86Context& ctx, uint32_t player) {
    if (!player || !mem::Spans(player, 0x770) || !kernel::R32(player + ((kernel::R32(player + 0xBC) & 0x8000) ? 0x768 : 0x76C))) return 0xFFFFFFFF;
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = player;
    uint32_t point = kernel::CallGuest(ctx, 0x26090, {0x11});
    ctx.ecx = ecx, ctx.eax = eax;
    return point;
}
uint32_t AtPoint(X86Context& ctx, uint32_t player, uint32_t point) {
    uint32_t object = player ? kernel::R32(player + 4) : 0, skeleton = object && mem::Spans(object, 0x110) ? kernel::R32(object + 0xE8) : 0;
    if (!skeleton || !mem::Spans(skeleton, 0x10) || point == 0xFFFFFFFF) return 0;
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = skeleton;
    uint32_t at = kernel::CallGuest(ctx, 0x188130, {point});
    ctx.ecx = ecx, ctx.eax = eax;
    return at && mem::Spans(at, 0x110) && int32_t(kernel::R32(at + 8)) >= 0 ? at : 0;
}
// A thing made to be thrown, as the game asks it (0x2B0C0): a Bouncing Object, a Missile, Lightning.
bool Throwable(uint32_t object) {
    if (!object || !mem::Spans(object, 0x110)) return false;
    uint32_t cls = ClassOf(object);
    return cls == 0xEB13B226 || cls == 0xFD73BAB8 || cls == 0x16AB1B0C;
}
// What hangs on a stand-in and on its owner: the thing ready to throw, and the one taken in hand for the throw (the
// point of the hand is at Player +0x44A while a throw goes on). Looked up before the Players' methods run and again
// right before the views are drawn.
void DoublesLoose(X86Context& ctx) {
    for (int which = 0; which < 2; which++) {
        Double& d = g_doubles[which];
        d.loose[0] = d.loose[1] = d.owner_loose[0] = d.owner_loose[1] = 0;
        if (!d.object || !d.player || !mem::Spans(d.object, 0x110) || int32_t(kernel::R32(d.object + 8)) < 0) continue;
        auto look = [&](uint32_t player, uint32_t out[2]) {
            out[0] = AtPoint(ctx, player, ReadyPoint(ctx, player));
            uint32_t hand = kernel::R32(player + 0x44A);
            out[1] = hand != 0xFFFFFFFF ? AtPoint(ctx, player, hand) : 0;
            if (!Throwable(out[0])) out[0] = 0;
            if (!Throwable(out[1])) out[1] = 0;
        };
        look(d.player, d.loose);
        uint32_t owner = which ? g_second : g_player, owner_object = which ? g_second_object : g_player_object;
        if (d.out && owner && owner_object && mem::Spans(owner, 0x490) && int32_t(kernel::R32(owner_object + 8)) >= 0) look(owner, d.owner_loose);
    }
}
// The throw of a stand-in is for show. The game throws in three steps, each a message the throwing movement sends its
// Player (his message table is filled at 0x2631C0: the message at +0, its routine at +8, 0x1C a row):
//   0xE7 (0x32E40): the thing ready to throw is taken from its point into the hand the message names, one is taken
//        out of his inventory, and the next one of the kind is made ready (0x13A00). What the hand held is hidden
//        and remembered (handle at +0x448, the hand's point at +0x44A).
//   0xE8 (0x307A0): the weapon in the throwing hand goes to the other hand for the length of the throw.
//   0xE6 (0x30FD0): the thing in the hand is let go: taken off the skeleton and sent on its way, as fast as the
//        trigger was held long (+0x3CA). The state's end (0x32C70) calls the same routine if it is still in the hand.
// A stand-in has no inventory and what it let go would be a second grenade. For it: 0xE7 moves the thing from the
// belt to the hand and nothing else, 0xE6 takes it out of the hand and ends it (its owner's own grenade is the one
// that flies), and the next one is put on its belt by Doubles when the throw is over.
void DoubleTakesInHand(X86Context& ctx, int which, uint32_t message) {
    Double& d = g_doubles[which];
    uint32_t record = message && mem::Spans(message, 8) ? kernel::R32(message + 4) : 0;
    if (!record || !mem::Spans(record, 0x10) || kernel::R32(record + 4) != 1 || !d.object) return;
    uint32_t hand = kernel::R32(record + 0x0A), skeleton = kernel::R32(d.object + 0xE8);
    uint32_t there = AtPoint(ctx, d.player, hand);
    if (!skeleton || Throwable(there)) return;  // (as the game: the hand has one already)
    uint32_t point = ReadyPoint(ctx, d.player), ready = AtPoint(ctx, d.player, point);
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    if (!ready) {
        ctx.ecx = d.player;
        kernel::CallGuest(ctx, 0x25C10, {20, 0});
        ctx.ecx = ecx, ctx.eax = eax;
        return;
    }
    kernel::W32(d.player + 0x44A, hand);
    if (there) {  // what the hand held is hidden and remembered: the state's end shows it again
        ctx.ecx = there;
        kernel::CallGuest(ctx, 0x183C70, {0});
        ctx.ecx = d.player + 0x448;
        kernel::CallGuest(ctx, 0x12000, {there});
    }
    ctx.ecx = skeleton;
    kernel::CallGuest(ctx, 0x18A580, {point});
    ctx.ecx = skeleton;
    kernel::CallGuest(ctx, 0x18AD50, {hand, ready, 0, 0});
    ctx.ecx = ecx, ctx.eax = eax;
    d.loose[0] = 0, d.loose[1] = ready;
    static int said = 0;
    if (said++ < 4) LOGI("coop: Player %d throws in first person: his stand-in body takes the thing on its belt in hand (for show: nothing leaves its inventory, it has none)", which + 1);
}
void DoubleLetsGo(X86Context& ctx, int which, uint32_t message) {
    Double& d = g_doubles[which];
    uint32_t record = message && mem::Spans(message, 8) ? kernel::R32(message + 4) : 0;
    if (!record || !mem::Spans(record, 0x10) || kernel::R32(record + 4) != 1 || !d.object) return;
    uint32_t hand = kernel::R32(record + 0x0A), skeleton = kernel::R32(d.object + 0xE8);
    uint32_t there = AtPoint(ctx, d.player, hand);
    if (!skeleton || !Throwable(there)) return;
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = skeleton;
    kernel::CallGuest(ctx, 0x18A580, {hand});
    if (int32_t(kernel::R32(there + 8)) >= 0) {
        ctx.ecx = there;
        kernel::CallGuest(ctx, kObjectEnd, {});
    }
    ctx.ecx = ecx, ctx.eax = eax;
    if (!kernel::R16(d.player + 0x448)) kernel::W32(d.player + 0x44A, 0xFFFFFFFF);  // (nothing was hidden for the throw: the hand is free again)
    d.loose[1] = 0;
    static int said = 0;
    if (said++ < 4) LOGI("coop: Player %d's stand-in body lets go: the thing in its hand is taken away, nothing flies from it (his own throw is the real one)", which + 1);
}
// What a stand-in does with its owner: the actions whose question is put to HIS controller while it is out. The
// numbers are the game's own (read with S2_ACTION_TRACE: 3E9 fire, 3EA throw, 3EB jump, 3EC use, 3ED first person,
// 3F5 reload, 3F6 flashlight, 3FB crouch, 44A the second attack). The throw's state asks how long the trigger is
// held (0x30650) and the crouch ends when its button is let go (0x3B270), both through 0x1A08A0, which asks 0x1A0790.
// Tests: S2_DOUBLE_ACTIONS=<hex>,<hex>,... names them.
int DoubleAction(uint32_t user, uint32_t action) {
    static const std::vector<uint32_t> actions = [] {
        std::vector<uint32_t> list = {0x3EB, 0x3E9, 0x3F5, 0x44A, 0x3EA, 0x3FB};
        if (const char* v = std::getenv("S2_DOUBLE_ACTIONS")) {
            list.clear();
            for (const char* e = v; e && *e; e = std::strchr(e, ',') ? std::strchr(e, ',') + 1 : nullptr) list.push_back(uint32_t(std::strtoul(e, nullptr, 16)));
        }
        return list;
    }();
    for (int which = 0; which < 2; which++) {
        const Double& d = g_doubles[which];
        if (!d.object || !d.out || !d.player || user != d.player + kUser) continue;
        return std::find(actions.begin(), actions.end(), action) != actions.end() ? which : -1;
    }
    return -1;
}
// The states a stand-in takes over from its owner: begun on it when he begins one (0x25CD0: this = Player; state)
// and ended when his ends (0x25C10: state, 0). (A state's bit: bit N of +0xBC, from 32 on of +0xC0.) What a player
// does in first person, by the buttons (run st1): jump = Fall (9) alone, no Jump; fire = Move, Aim, Attack (3);
// reload = Reload (17); the second attack = FP Melee Attack (31), for which the stand-in has Melee Attack (4);
// throw = Throw (20); changing weapon = Equip (24), which the stand-in gets by being handed the weapon (Doubles).
// Jump, Fall and Landing are not in this list: the stand-in reads the jump button itself and lands when he lands
// (the hook of 0x25480). Throw (20) is begun once for each throw of his and left to end by itself: its movement in
// third person is another than his in first person (at the pace it is given, see the hook of 0x259B0, it lets go in
// the frame he does and is over five frames before him), and what it does to the world is taken out of it
// (DoubleTakesInHand, DoubleLetsGo). Crouch (34) and the crouch a low place forces on him (35) are his.
// The game may say no to a state (it sorts out which states go together, 0x25C80), and a state may end by itself
// (Attack ends when its time is up): one that is refused is tried again only when its owner begins it anew, one that
// ended while he is still in his is begun again after a few frames.
//   S2_DOUBLE_MIRROR=<number>,<number>,... names them (a pair <his>:<its> gives it another state for one of his);
//   S2_DOUBLE_MIRROR=0: none (tests)
void MirrorStates(X86Context& ctx, int which) {
    static const std::vector<std::pair<uint32_t, uint32_t>> states = [] {
        std::vector<std::pair<uint32_t, uint32_t>> list = {{2, 2}, {3, 3}, {17, 17}, {31, 4}, {20, 20}, {34, 34}, {35, 35}};
        if (const char* v = std::getenv("S2_DOUBLE_MIRROR")) {
            list.clear();
            for (const char* e = v; e && *e; e = std::strchr(e, ',') ? std::strchr(e, ',') + 1 : nullptr) {
                uint32_t his = uint32_t(std::atoi(e)), its = his;
                const char* colon = std::strchr(e, ':');
                const char* comma = std::strchr(e, ',');
                if (colon && (!comma || colon < comma)) its = uint32_t(std::atoi(colon + 1));
                if (his && his < 39 && its && its < 39) list.push_back({his, its});
            }
        }
        return list;
    }();
    Double& d = g_doubles[which];
    uint32_t owner = which ? g_second : g_player;
    if (states.empty() || !d.out || !d.player || !owner) return;
    // (the rounds in the weapon in its hand are its owner's, Player +0x140: with none the Attack state ends at once)
    static const bool no_rounds = std::getenv("S2_DOUBLE_NO_ROUNDS") != nullptr;
    if (!no_rounds) kernel::W32(d.player + 0x140, kernel::R32(owner + 0x140));
    auto on = [](uint32_t player, uint32_t state) { return (kernel::R32(player + 0xBC + (state >> 5) * 4) >> (state & 31)) & 1; };
    static uint64_t tried[2][40] = {};   // the frame a state was last begun on this stand-in for the state its owner is in now
    static bool refused[2][40] = {};     // ... and whether the game said no
    static const bool trace = std::getenv("S2_DOUBLE_TRACE") != nullptr;
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    for (const auto& [his, its] : states) {
        bool he = on(owner, his) != 0, it = on(d.player, its) != 0;
        if (!he) tried[which][his] = 0, refused[which][his] = false;
        // (the throw: once for each of his, whether this began it or the stand-in itself, which reads his trigger;
        // and it ends by itself)
        if (its == 20 && he && it && !tried[which][his]) tried[which][his] = g_frames;
        if (he == it) continue;
        if (its == 20 && (!he || tried[which][his])) continue;
        // (while it throws, its Aim is the throw's own: 0x304A0 begins it, and it is not ended under the throw; and
        // when its throw is over before his, which is the longer by a few frames, no Aim is begun for the rest)
        if (its == 2 && ((!he && on(d.player, 20)) || (he && on(owner, 20)))) continue;
        if (he && (refused[which][his] || (tried[which][his] && g_frames < tried[which][his] + 6))) continue;
        ctx.ecx = d.player;
        uint32_t result = he ? kernel::CallGuest(ctx, 0x25CD0, {its}) : kernel::CallGuest(ctx, 0x25C10, {its, 0});
        if (he) tried[which][his] = g_frames, refused[which][his] = !on(d.player, its);
        static int said = 0;
        if (trace && said++ < 200)
            LOGI("double trace: frame %llu: Player %d's stand-in %s state %u (his %u): the game says %u, its states are now %08X %02X", (unsigned long long)g_frames, which + 1,
                 he ? "begins" : "ends", its, his, result, kernel::R32(d.player + 0xBC), kernel::R32(d.player + 0xC0) & 0x7F);
    }
    ctx.ecx = ecx, ctx.eax = eax;
}
// The names of the states that are on, for the logs (bit N of +0xBC is state N; from 32 on, bit N - 32 of +0xC0).
std::string StateNames(uint32_t low, uint32_t high) {
    static const char* const kStates[39] = {"0", "Move", "Aim", "Attack", "MeleeAttack", "Turn", "Idle", "IntermittentIdle", "Jump", "Fall", "Pitch", "Controlled", "Landing",
                                            "JumpMelee", "Roll", "FirstPerson", "HitReacting", "Reload", "Morph", "Insanity", "Throw", "Interact", "ViewMonitor",
                                            "CameraYaw", "Equip", "ItemPickup", "LadderClimb", "LookAt", "InsanityAttack", "InsanityAttackAcquisition",
                                            "DocumentView", "FPMelee", "PlayAnim", "HeadGrab", "Crouch", "ForcedCrouch", "Halt", "MorphFail", "BodySlam"};
    std::string out;
    for (int i = 1; i < 39; i++)
        if ((i < 32 ? low >> i : high >> (i - 32)) & 1) out += std::string(out.empty() ? "" : " ") + kStates[i];
    return out.empty() ? std::string("none") : out;
}
void EndDoubles(X86Context& ctx, const char* why) {
    uint32_t ecx = ctx.ecx;
    int ended = 0;
    DoublesLoose(ctx);
    for (Double& d : g_doubles) {
        uint32_t object = d.object, player = d.player, display = d.display, held[2] = {Named(d.player, 0x3AC), Named(d.player, 0x3AE)};
        uint32_t loose[2] = {d.loose[0], d.loose[1]};
        d = Double{};
        if (!object) continue;
        ended++;
        // (what hangs on it, ready to throw or in its hand: objects of their own, they would fall to the ground there)
        for (uint32_t l : loose) {
            if (!l || !mem::Spans(l, 0x110) || int32_t(kernel::R32(l + 8)) < 0) continue;
            ctx.ecx = l;
            kernel::CallGuest(ctx, kObjectEnd, {});
        }
        if (display && mem::Spans(player, 0x7F4) && !kernel::R32(player + 0x7F0)) kernel::W32(player + 0x7F0, display);
        if (mem::Spans(object, 0x110) && int32_t(kernel::R32(object + 8)) >= 0) {
            ctx.ecx = object;
            kernel::CallGuest(ctx, kObjectEnd, {});
        }
        // (what it held goes with it: a held weapon is an object of its own, see EndSecond)
        for (uint32_t h : held) {
            if (!h || h == object || int32_t(kernel::R32(h + 8)) < 0) continue;
            if (Named(g_player, 0x3AC) == h || Named(g_player, 0x3AE) == h || Named(g_second, 0x3AC) == h || Named(g_second, 0x3AE) == h) continue;
            ctx.ecx = h;
            kernel::CallGuest(ctx, kObjectEnd, {});
        }
    }
    ctx.ecx = ecx;
    if (ended) LOGI("coop: the stand-in bodies are taken out (%s)", why);
}
// They are made once and put away, not each time a player goes into first person: the making of a Player has the HUD
// show its parts afresh (the new Player's own making sends 0x186B1, 0x186AC and 0x186B3, and those cannot be dropped:
// without them the ammunition counter and the flashlight sign stayed off Player 1's HUD for good, run dbl48).
void SpawnDouble(X86Context& ctx, int which) {
    Double& d = g_doubles[which];
    uint32_t owner = g_player_object;  // (both players are of one template: the stand-ins are made beside Player 1)
    if (!owner) return;
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(owner + kMatrix), 64);
    uint32_t tmpl = kernel::R32(owner + kTemplateWord) & 0xFFFF, ecx = ctx.ecx;
    uint32_t count = kernel::R32(kObjectManager + kPlayerCount);
    g_double_spawning = true;
    uint32_t object = kernel::CallGuest(ctx, kSpawn, {owner, matrix, 0, tmpl});
    g_double_spawning = false;
    ctx.ecx = ecx;
    uint32_t player = object ? kernel::R32(object + kClassObject) : 0;
    if (!player || !IsPlayerObject(object)) {
        LOGR("coop: Player %d's stand-in body could not be made (object %08X)", which + 1, object);
        return;
    }
    d = Double{};
    d.object = object, d.player = player, d.since = g_frames;
    OffTheList(object);
    kernel::W32(player + kUserDevice, 3);  // a controller that is not there
    kernel::W32(player + 0x1E, 1);         // nothing hurts it
    // A Player may have a weapon display of his own (+0x7F0, made at 0x20497 when his property 0x144 names one), which
    // is told of each weapon he takes in hand (0x15047). A stand-in has none while it lives (the Player's code leaves
    // the display alone when there is none, 0x1501B) and gets it back to end with.
    d.display = kernel::R32(player + 0x7F0);
    kernel::W32(player + 0x7F0, 0);
    LOGI("coop: Player %d's stand-in body (object %08X, Player %08X) is made and put away: it shows him to the other player while he is in first person; %u players were listed, %u are now",
         which + 1, object, player, count, kernel::R32(kObjectManager + kPlayerCount));
}
// A stand-in that is out is put where its owner stands, facing as he faces. Twice a frame: before the Players' methods
// run, so that its own Run starts from its owner's place with its owner's stick, and again right before the views are
// drawn (the hook on 0x1B83E0), which throws away the step its own Run took in between.
// How: the first time it is out (and whenever it is far off) the game's own way of putting a Player somewhere
// (0x2F060), followed by the game's own "he has landed" (0x3BB00: it ends the Fall and Jump states, begins Landing and
// sets the on-the-ground bit that 0x2F060 cleared; a stand-in has no Detect to find the ground with). After that only
// its place is written: its object's matrix, its skeleton's copy of it (+0x1A4, as 0x2F060 does) and its body's
// "where I was" (body +0x84); what it holds is moved by the same step (a held thing's place is worked out from the
// hand while the Player's own methods run, so without this it would lag the body by that step).
void PinDoubles(X86Context& ctx) {
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    DoublesLoose(ctx);
    for (int which = 0; which < 2; which++) {
        Double& d = g_doubles[which];
        if (!d.object || !d.out) continue;
        uint32_t owner = which ? g_second_object : g_player_object, owner_player = which ? g_second : g_player;
        if (!owner || !owner_player || !mem::Spans(owner, 0x110) || !mem::Spans(d.object, 0x110)) continue;
        // (both still what they were: not ended, and each object still its Player's)
        if (int32_t(kernel::R32(owner + 8)) < 0 || kernel::R32(owner + kClassObject) != owner_player) continue;
        if (int32_t(kernel::R32(d.object + 8)) < 0 || kernel::R32(d.object + kClassObject) != d.player) continue;
        static uint32_t matrix = mem::AllocSystem(64);
        std::memcpy(kernel::HostPtr(matrix), kernel::HostPtr(owner + kMatrix), 64);
        float step[3], far2 = 0;
        for (int i = 0; i < 3; i++) step[i] = ReadF(matrix + 0x30 + i * 4) - ReadF(d.object + kPosition + i * 4), far2 += step[i] * step[i];
        if (!d.landed || far2 > 4.0f) {
            d.landed = true;
            ctx.ecx = d.player;
            kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
            ctx.ecx = d.player;
            kernel::CallGuest(ctx, 0x3BB00, {});
        } else {
            ctx.ecx = d.object;
            uint32_t its = kernel::CallGuest(ctx, 0x184EB0, {});  // (an object's matrix, to be written: the engine marks it changed)
            if (its && mem::Spans(its, 0x3C)) std::memcpy(kernel::HostPtr(its), kernel::HostPtr(matrix), 64);
            uint32_t skeleton = kernel::R32(d.object + 0xE8), body = kernel::R32(d.player + 0xF0);
            if (skeleton && mem::Spans(skeleton, 0x1E4)) std::memcpy(kernel::HostPtr(skeleton + 0x1A4), kernel::HostPtr(matrix), 64);
            if (body && mem::Spans(body, 0x90)) std::memcpy(kernel::HostPtr(body + 0x84), kernel::HostPtr(matrix + 0x30), 12);
            uint32_t carried[4] = {Named(d.player, 0x3AC), Named(d.player, 0x3AE), d.loose[0], d.loose[1]};
            if (carried[1] == carried[0]) carried[1] = 0;
            for (uint32_t held : carried) {
                if (!held) continue;
                ctx.ecx = held;
                uint32_t m = kernel::CallGuest(ctx, 0x184EB0, {});
                if (!m || !mem::Spans(m, 0x3C)) continue;
                for (int i = 0; i < 3; i++) {
                    float v = ReadF(m + 0x30 + i * 4) + step[i];
                    std::memcpy(kernel::HostPtr(m + 0x30 + i * 4), &v, 4);
                }
            }
        }
        if (uint32_t proxy = ProxyOf(d.player)) {
            ctx.ecx = proxy;
            uint32_t its = kernel::CallGuest(ctx, 0x184EB0, {});
            if (its && mem::Spans(its, 0x3C)) std::memcpy(kernel::HostPtr(its + 0x30), kernel::HostPtr(matrix + 0x30), 12);
        }
        OffTheList(d.object);
    }
    ctx.ecx = ecx, ctx.eax = eax;
}
// once a frame
void Doubles(X86Context& ctx) {
    g_doubles_fresh = false;
    bool both = g_second && g_second_object && mem::Spans(g_second, 0x490) && g_player && g_player_object && mem::Spans(g_player, 0x490);
    for (Double& d : g_doubles)
        if (d.object && (!mem::Spans(d.object, 0x110) || int32_t(kernel::R32(d.object + 8)) < 0 || kernel::R32(d.object + kClassObject) != d.player)) {
            LOGI("coop: a stand-in body's object ended by itself");
            d = Double{};
        }
    if (!both || g_double_off) {
        EndDoubles(ctx, "the two players are no longer both in");
        return;
    }
    // (made when both have been in for half a second and the picture is theirs: not in a scene, not while one is down)
    static uint64_t both_since = 0;
    if (!g_doubles[0].object || !g_doubles[1].object) {
        if (!both_since) both_since = g_frames;
        if (g_frames < both_since + 30 || g_one_view || Down()) return;
        for (int which = 0; which < 2; which++)
            if (!g_doubles[which].object) SpawnDouble(ctx, which);
        if (!g_doubles[0].object || !g_doubles[1].object) return;
    }
    both_since = 0;
    static const bool solid = std::getenv("S2_COOP_DOUBLE_SOLID") != nullptr;      // tests
    static const bool unarmed = std::getenv("S2_COOP_DOUBLE_NO_WEAPON") != nullptr;  // tests
    uint32_t ecx = ctx.ecx;
    for (int which = 0; which < 2; which++) {
        Double& d = g_doubles[which];
        uint32_t owner = which ? g_second : g_player;
        // Out while its owner is in first person: the Player's state 15 (bit 15 of his state word +0xBC); 0xC0000 are
        // Morph and Insanity, the creature form.
        bool want = !g_one_view && !Down() && (kernel::R32(owner + 0xBC) & 0x8000) && !(kernel::R32(owner + 0xBC) & 0xC0000);
        if (d.out != want) {
            if (want) LOGI("coop: Player %d is in first person: his stand-in body shows him to the other player", which + 1);
            else LOGI("coop: Player %d's stand-in body is put away (he is no longer in first person, or the picture is one view)", which + 1);
            d.out = want, d.since = g_frames, d.landed = false;
        }
        // Nothing finds it, out or put away: its object, what it holds and the object that moves it (should it have
        // one) carry the engine's "not for questions" mark (object word +8, 0x400000: the world's questions pass such
        // objects by, 0x1965CD; the game itself puts it on a player's weapons for the length of his own sweep,
        // 0x16820, and on a player for his line-of-sight check, 0xD72D5).
        d.held[0] = Named(d.player, 0x3AC), d.held[1] = Named(d.player, 0x3AE);
        if (!solid)
            for (uint32_t object : {d.object, ProxyOf(d.player), d.held[0], d.held[1], d.loose[0], d.loose[1]})
                if (object) kernel::W32(object + 8, kernel::R32(object + 8) | 0x400000);
        if (kernel::R32(d.player + 0x1E) != 1) kernel::W32(d.player + 0x1E, 1);
        float most = ReadF(d.player + 0x1A);
        if (most > 0) std::memcpy(kernel::HostPtr(d.player + 0x16), &most, 4);
        d.owner_held[0] = d.owner_held[1] = 0;
        if (!d.out) continue;
        d.owner_held[0] = Named(owner, 0x3AC), d.owner_held[1] = Named(owner, 0x3AE);
        // The weapon in its hands is the kind its owner holds (the object in his right hand, handle at +0x3AC; an item
        // is its template's number), put there by 0x14F30 as Player 2's is (Arm), again whenever he changes weapons.
        uint32_t item = d.owner_held[0] ? kernel::R32(d.owner_held[0] + kTemplateWord) & 0xFFFF : 0xFFFF;
        uint32_t has = d.held[0] ? kernel::R32(d.held[0] + kTemplateWord) & 0xFFFF : 0xFFFF;
        if (!unarmed && item != 0xFFFF && item != has && (d.tried != item || d.tried_at < d.since || g_frames > d.tried_at + 120)) {
            d.tried = item, d.tried_at = g_frames;
            {
                AsDouble as(d.player);
                ctx.ecx = d.player;
                kernel::CallGuest(ctx, kEquip, {item, 1});
                ctx.ecx = ecx;
            }
            OffTheList(d.object);
            LOGI("coop: Player %d's stand-in is handed the weapon he holds (item %u, class %08X; it had %s)", which + 1, item, ClassOfTemplate(item),
                 has == 0xFFFF ? "nothing" : "another");
            d.held[0] = Named(d.player, 0x3AC), d.held[1] = Named(d.player, 0x3AE);
        }
        // The thing on its belt, ready to throw, is the kind its owner has ready (the item he has ready is written
        // down at Player +0x144 by 0x13A00, the game's routine that makes one and hangs it on its point: a Missile
        // Item's property 0x40 names what is made). Handed over the same way, again when his kind changes or the
        // stand-in has thrown its own, and taken off it (0x131D0) when he has none left. Not while either throws.
        static const bool no_throw = std::getenv("S2_DOUBLE_NO_THROW") != nullptr;  // tests: as before 2026-11-27
        bool throwing = ((kernel::R32(owner + 0xBC) | kernel::R32(d.player + 0xBC)) & (1u << 20)) != 0;
        uint32_t ready = d.owner_loose[0] ? kernel::R32(owner + 0x144) : 0xFFFFFFFF;
        if (ready != 0xFFFFFFFF && ClassOfTemplate(ready) != 0xFE9F718D) ready = 0xFFFFFFFF;
        uint32_t has_ready = d.loose[0] ? kernel::R32(d.player + 0x144) : 0xFFFFFFFF;
        if (!no_throw && !unarmed && !throwing && ready != has_ready && (d.ready_tried != ready || d.ready_at < d.since || g_frames > d.ready_at + 120)) {
            d.ready_tried = ready, d.ready_at = g_frames;
            ctx.ecx = d.player;
            if (ready != 0xFFFFFFFF) kernel::CallGuest(ctx, 0x13A00, {ready});  // (its hook makes the stand-in "the player" meanwhile: its events are dropped)
            else kernel::CallGuest(ctx, 0x131D0, {d.loose[0]});
            ctx.ecx = ecx;
            OffTheList(d.object);
            DoublesLoose(ctx);
            static int said = 0;
            if (said++ < 12) {
                if (ready != 0xFFFFFFFF) LOGI("coop: Player %d's stand-in is handed what he has ready to throw (item %u): %s", which + 1, ready, d.loose[0] ? "it hangs on its belt" : "IT COULD NOT BE MADE");
                else LOGI("coop: Player %d has nothing left to throw: the thing on his stand-in's belt is taken off it", which + 1);
            }
            if (!solid && d.loose[0]) kernel::W32(d.loose[0] + 8, kernel::R32(d.loose[0] + 8) | 0x400000);
        }
    }
    PinDoubles(ctx);
    g_doubles_fresh = true;
    {   // tests: S2_DOUBLE_TRACE=1 says what a stand-in that is out is doing beside its owner, when either changes
        static const bool trace = std::getenv("S2_DOUBLE_TRACE") != nullptr;
        static uint32_t was[2][4] = {};
        static int said = 0;
        for (int which = 0; trace && which < 2; which++) {
            const Double& d = g_doubles[which];
            uint32_t owner = which ? g_second : g_player;
            if (!d.out || !d.player || !owner) continue;
            uint32_t his = kernel::R32(owner + 0xBC), its = kernel::R32(d.player + 0xBC), his2 = kernel::R32(owner + 0xC0) & 0x7F, its2 = kernel::R32(d.player + 0xC0) & 0x7F;
            if ((his != was[which][0] || its != was[which][1] || his2 != was[which][2] || its2 != was[which][3]) && said++ < 400)
                LOGI("double trace: frame %llu: Player %d does [%s], his stand-in [%s] (on the ground: he %u, it %u; rounds in the weapon: he %d, it %d)", (unsigned long long)g_frames,
                     which + 1, StateNames(his, his2).c_str(), StateNames(its, its2).c_str(), kernel::R32(owner + 0xA8) & 1, kernel::R32(d.player + 0xA8) & 1,
                     int32_t(kernel::R32(owner + 0x140)), int32_t(kernel::R32(d.player + 0x140)));
            was[which][0] = his, was[which][1] = its, was[which][2] = his2, was[which][3] = its2;
            if (g_frames % 60 == 0) {
                uint32_t hp = ReadyPoint(ctx, owner), ip = ReadyPoint(ctx, d.player);
                uint32_t ho = AtPoint(ctx, owner, hp), io = AtPoint(ctx, d.player, ip);
                LOGI("double trace: ready to throw: Player %d has item %d (+0x144), point %d, there %08X (class %08X, template %u), hand point %d; his stand-in item %d, point %d, there %08X (class %08X, template %u), hand point %d",
                     which + 1, int32_t(kernel::R32(owner + 0x144)), int32_t(hp), ho, ho ? ClassOf(ho) : 0, ho ? kernel::R16(ho + kTemplateWord) : 0, int32_t(kernel::R32(owner + 0x44A)),
                     int32_t(kernel::R32(d.player + 0x144)), int32_t(ip), io, io ? ClassOf(io) : 0, io ? kernel::R16(io + kTemplateWord) : 0, int32_t(kernel::R32(d.player + 0x44A)));
            }
        }
    }
}

// Player 2's Camera made anew: his view camera and his Camera object are ended the way they are when he leaves, and
// made again the way they are when he comes in (SplitOn). For a Camera that no longer follows him ("there was a bug
// where his camera became fixed", 2026-11-26; not reproduced, so the cause is not known: the check in DetailFrame
// records the Camera's state first). Player 2 can ask for it himself: the teleport (A+B+X+Y held five seconds).
void RemakeCamera2(X86Context& ctx, const char* why) {
    uint32_t view = kernel::R32(kViewPointer);
    if (!view || !g_second_object || !ViewCamera2() || !g_camera2_object) return;
    LOGR("coop: Player 2's camera is made anew (%s)", why);
    uint32_t ecx = ctx.ecx, old = g_camera2_object;
    ctx.ecx = view;
    kernel::CallGuest(ctx, 0x1A6E10, {old});  // (the engine's own way out for a view camera, by its object)
    if (int32_t(kernel::R32(old + 8)) >= 0) {
        ctx.ecx = old;
        kernel::CallGuest(ctx, kObjectEnd, {});
    }
    ctx.ecx = ecx;
    g_camera2_object = 0;
    bool one = g_one_view;
    SplitOn(ctx);
    if (one) Views(ctx, true);
    LOGR("coop: Player 2's camera: %s", g_camera2_object && ViewCamera2() ? "a new one follows him" : "COULD NOT BE MADE AGAIN");
}

// ---- the detail record ----------------------------------------------------------------------------------------------
// (2026-11-26: "The logs need to be extremely detailed, to catch graphics etc." Frame rate is not the limit, the
// folder's size is: detail.cpp packs the lines.) One line for every frame of a level, with both players, both
// cameras and what each view drew; once a second every object that appeared, moved, changed or ended.
struct PassStats {
    uint32_t draws = 0, fogged = 0;
    bool have = false, fog_on = false;
    uint32_t fog_color = 0, fog_start = 0, fog_end = 0, ambient = 0, lights = 0, vw = 0, vh = 0, rt = 0;
    float proj[4] = {0, 0, 0, 0}, eye[3] = {0, 0, 0};
    uint64_t hash = 0;
};
PassStats g_pass[2];       // [0] Player 1's view and all else, [1] Player 2's view (filled by DrawVariant)
PassStats g_pass_last[2];  // ... of the frame before (for the marks)
std::atomic<int> g_ammo2_rounds{-1}, g_ammo2_clip{0}, g_ammo2_more{0};  // Player 2's ammunition, for his display (Ui)
std::atomic<int> g_ammo2_shells{0};  // ... 1: his weapon is the shotgun (template 3572), whose rounds the game shows as shells

struct CameraNow {
    float eye[3] = {0, 0, 0}, way[3] = {0, 0, 0};
    uint32_t mode = 99, depth = 0, flags = 0;
    bool there = false;
};
CameraNow CameraOf(uint32_t object) {
    CameraNow c;
    if (!object || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) return c;
    uint32_t camera = kernel::R32(object + kClassObject);
    if (!camera || !mem::Spans(camera, 0x40)) return c;
    c.there = true;
    for (int i = 0; i < 3; i++) c.eye[i] = ReadF(object + kPosition + i * 4), c.way[i] = ReadF(object + kMatrix + 0x20 + i * 4);
    uint32_t stack = kernel::R32(camera + 0x20);
    c.depth = kernel::R32(camera + 0x28);
    c.mode = stack && c.depth && c.depth <= 64 ? kernel::R32(stack + c.depth * 8 - 4) : 99;
    c.flags = kernel::R32(camera + 0x2C);
    return c;
}
float Facing(uint32_t object) {  // the "facing" of the position lines
    return std::atan2(ReadF(object + kMatrix + 0x20), ReadF(object + kMatrix + 0x28)) * 57.29578f;
}

// [the owner, 2026-11-27: Load Game from the pause menu crashed here] An object the game has freed while its address
// is still ours: its first and its last bytes must both be in committed memory. A Player or a body can lie across a
// page edge; Load Game freed the page of the Player's start (+0xB0 read) while the page of +0x490 stayed.
bool Whole(uint32_t p, uint32_t size) { return p && mem::IsCommitted(p) && mem::IsCommitted(p + size - 1); }
void DetailFrame(X86Context& ctx) {
    PassStats pass[2] = {g_pass[0], g_pass[1]};
    g_pass[0] = g_pass[1] = PassStats{};
    static bool was_in = false;
    {   // tests: S2_WEAPON_PROBE=1: each player's weapon in hand, where it is and which way it points, twice a second
        static const int probe = std::getenv("S2_WEAPON_PROBE") ? std::max(1, std::atoi(std::getenv("S2_WEAPON_PROBE"))) : 0;
        if (probe && g_frames % probe == 0) {
            const uint32_t pl[2] = {g_player, g_second}, body[2] = {g_player_object, g_second_object};
            for (int i = 0; i < 2; i++) {
                if (!mem::Spans(pl[i], 0x540) || !mem::Spans(body[i], 0x114)) continue;
                uint32_t w = Named(pl[i], 0x3AC);
                if (!w) continue;
                LOGI("weapon probe: Player %d at %.2f %.2f facing %.0f; weapon %08X at %.2f %.2f %.2f way %.2f %.2f, owner handle %04X (body %04X), skeleton %08X, parent %08X",
                     i + 1, ReadF(body[i] + kPosition), ReadF(body[i] + kPosition + 8), Facing(body[i]), w, ReadF(w + kPosition), ReadF(w + kPosition + 4),
                     ReadF(w + kPosition + 8), ReadF(w + kMatrix + 0x20), ReadF(w + kMatrix + 0x28), kernel::R16(w + 0x56), kernel::R16(body[i] + 0xE6),
                     kernel::R32(w + 0xE8), kernel::R32(w + 0x5C));
                uint32_t sk = kernel::R32(w + 0xE8);
                if (mem::Spans(sk, 0x150)) {
                    uint32_t of = kernel::R32(sk + 0xFC), bones = kernel::R32(sk + 0x140), n = kernel::R32(sk + 0x148);
                    std::string b;
                    for (uint32_t k = 0; k < n && k < 4 && mem::Spans(bones + k * 0x7C, 0x7C); k++) {
                        char one[160];
                        std::snprintf(one, sizeof one, " [%u: x %.2f %.2f %.2f z %.2f %.2f %.2f at %.2f %.2f %.2f]", k, ReadF(bones + k * 0x7C), ReadF(bones + k * 0x7C + 0x10),
                                      ReadF(bones + k * 0x7C + 0x20), ReadF(bones + k * 0x7C + 8), ReadF(bones + k * 0x7C + 0x18), ReadF(bones + k * 0x7C + 0x28),
                                      ReadF(bones + k * 0x7C + 0x30), ReadF(bones + k * 0x7C + 0x34), ReadF(bones + k * 0x7C + 0x38));
                        b += one;
                    }
                    LOGI("weapon probe: Player %d's weapon skeleton %08X: of %08X (%s at %.2f %.2f facing %.0f), %u bones:%s", i + 1, sk, of,
                         of == w ? "the weapon" : of == body[i] ? "his body" : of == g_player_object ? "PLAYER 1's body" : of == g_second_object ? "PLAYER 2's body" : "other",
                         mem::Spans(of, 0x114) ? ReadF(of + kPosition) : 0.0f, mem::Spans(of, 0x114) ? ReadF(of + kPosition + 8) : 0.0f, mem::Spans(of, 0x114) ? Facing(of) : 0.0f, n, b.c_str());
                }
            }
        }
    }
    {   // tests: S2_TEST_RECORD_FAULT=<frame>: a read of memory that is not there at that frame (the guard must hold)
        static const uint64_t fault = std::getenv("S2_TEST_RECORD_FAULT") ? uint64_t(std::atoll(std::getenv("S2_TEST_RECORD_FAULT"))) : 0;
        if (fault && g_frames == fault) LOGR("test: the record reads 0000FFF4: %08X", kernel::R32(0xFFF4));
    }
    bool in = InLevel() && Whole(g_player, 0x494) && Whole(g_player_object, 0x114);
    static std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - before).count();
    before = now;
    bool second = Whole(g_second, 0x494) && Whole(g_second_object, 0x114);
    CameraNow c1 = CameraOf(kernel::R32(kGameCamera)), c2 = second ? CameraOf(g_camera2_object) : CameraNow{};
    // ---- Player 2's ammunition, for the display in his picture: what is in the weapon in his hand (Player +0x140),
    // what its clip holds (0x17400) and what he carries for it (0x174A0). The game's own HUD number is the sum of the
    // first and the last (0x181B0), its row of marks the first.
    if (second && g_frames % 6 == 0) {
        uint32_t ecx = ctx.ecx;
        ctx.ecx = g_second;
        int clip = int(kernel::CallGuest(ctx, 0x17400, {}));
        ctx.ecx = g_second;
        int more = int(kernel::CallGuest(ctx, 0x174A0, {}));
        int rounds = int(kernel::R32(g_second + 0x140));
        ctx.ecx = ecx;
        bool creature = (kernel::R32(g_second + 0xBC) & 0xC0000) != 0;
        g_ammo2_clip = clip, g_ammo2_more = more, g_ammo2_rounds = clip > 0 && !creature && !Down() ? rounds : -1;
        uint32_t weapon = Named(g_second, 0x3AC);
        g_ammo2_shells = weapon && (kernel::R32(weapon + kTemplateWord) & 0xFFFF) == 3572 ? 1 : 0;
        static int said[3] = {-9, -9, -9};
        static uint64_t said_at = 0;
        if ((rounds != said[0] || clip != said[1] || more != said[2]) && g_frames - said_at > 120) {
            LOGI("ammo: Player 2 has %d in his weapon (a clip of %d) and carries %d more", rounds, clip, more);
            said[0] = rounds, said[1] = clip, said[2] = more, said_at = g_frames;
        }
    } else if (!second) g_ammo2_rounds = -1;
    {   // S2_AMMO_TRACE=1: both players' ammunition getters once a second
        static const bool trace = std::getenv("S2_AMMO_TRACE") != nullptr;
        if (trace && in && g_frames % 60 == 11) {
            const uint32_t players[2] = {g_player, second ? g_second : 0};
            for (int i = 0; i < 2; i++) {
                if (!players[i]) continue;
                uint32_t ecx = ctx.ecx, v[6];
                const uint32_t fns[6] = {0x17400, 0x174A0, 0x16130, 0x18160, 0x16100, 0x181B0};
                for (int k = 0; k < 6; k++) ctx.ecx = players[i], v[k] = kernel::CallGuest(ctx, fns[k], {});
                ctx.ecx = ecx;
                LOGI("ammo trace: Player %d: clip size %d, 0x174A0 %d, 0x16130 %d, 0x18160 %d, 0x16100 %d, 0x181B0 %d; in hands %04X %04X", i + 1, int(v[0]), int(v[1]),
                     int(v[2]), int(v[3]), int(v[4]), int(v[5]), kernel::R16(players[i] + 0x3AC), kernel::R16(players[i] + 0x3AE));
                uint32_t weapon = Named(players[i], 0x3AC), self = weapon ? kernel::R32(weapon + kClassObject) : 0;
                if (self && g_frames % 300 == 11) {
                    ctx.ecx = self;
                    uint32_t item = kernel::CallGuest(ctx, 0xF0CC0, {});
                    ctx.ecx = players[i] + kInventory;
                    uint32_t have = kernel::CallGuest(ctx, 0x110A0, {item});
                    ctx.ecx = ecx;
                    std::string items;
                    char one[48];
                    uint32_t list = kernel::R32(players[i] + kInventory), n = kernel::R32(players[i] + kInventory + 8);
                    for (uint32_t k = 0; k < n && k < 24; k++) {
                        std::snprintf(one, sizeof one, " [%X %X x%d]", kernel::R32(list + k * 12), kernel::R32(list + k * 12 + 4), int32_t(kernel::R32(list + k * 12 + 8)));
                        items += one;
                    }
                    LOGI("ammo trace: Player %d: his weapon (template %u) takes item %X, of which his inventory says he has %d; inventory at %08X: list %08X, %u entries:%s; +0x0C %08X +0x10 %08X +0x14 %08X",
                         i + 1, kernel::R32(weapon + kTemplateWord) & 0xFFFF, item, int(have), players[i] + kInventory, list, n, items.c_str(),
                         kernel::R32(players[i] + kInventory + 0x0C), kernel::R32(players[i] + kInventory + 0x10), kernel::R32(players[i] + kInventory + 0x14));
                }
            }
        }
    }

    // ---- a mark (F9, or a check below): the state of this moment into the session log
    {
        static uint32_t marks = 0;
        std::string why;
        uint32_t n = game_record::Marks(&why);
        if (n != marks) {
            marks = n;
            LOGR("mark: state at the mark (%s): frame %llu, %s, %s", why.c_str(), (unsigned long long)g_frames,
                 !in ? "no level in play" : g_one_view ? "one view (a scene)" : second ? "split screen" : "one player",
                 g_paused_now ? "paused" : "not paused");
            if (in) {
                LOGR("mark: Player 1 at %.2f %.2f %.2f facing %.0f, state %08X flags %08X health %.0f; his camera at %.2f %.2f %.2f looking %.2f %.2f %.2f, mode %u (%u on its stack), flags %08X",
                     ReadF(g_player_object + kPosition), ReadF(g_player_object + kPosition + 4), ReadF(g_player_object + kPosition + 8), Facing(g_player_object),
                     kernel::R32(g_player + 0xBC), kernel::R32(g_player + 0xB0), ReadF(g_player + 0x16), c1.eye[0], c1.eye[1], c1.eye[2], c1.way[0], c1.way[1], c1.way[2],
                     c1.mode, c1.depth, c1.flags);
                if (second)
                    LOGR("mark: Player 2 at %.2f %.2f %.2f facing %.0f, state %08X flags %08X health %.0f; his camera at %.2f %.2f %.2f looking %.2f %.2f %.2f, mode %u (%u on its stack), flags %08X",
                         ReadF(g_second_object + kPosition), ReadF(g_second_object + kPosition + 4), ReadF(g_second_object + kPosition + 8), Facing(g_second_object),
                         kernel::R32(g_second + 0xBC), kernel::R32(g_second + 0xB0), ReadF(g_second + 0x16), c2.eye[0], c2.eye[1], c2.eye[2], c2.way[0], c2.way[1],
                         c2.way[2], c2.mode, c2.depth, c2.flags);
                for (int i = 0; i < 2; i++) {
                    const PassStats& d = pass[i].draws ? pass[i] : g_pass_last[i];
                    if (!d.draws) continue;
                    float fs, fe;
                    std::memcpy(&fs, &d.fog_start, 4), std::memcpy(&fe, &d.fog_end, 4);
                    LOGR("mark: Player %d's view: %u draws (%u with fog); fog %s colour %08X from %.1f to %.1f, ambient %08X, lights %X; target %u x %u",
                         i + 1, d.draws, d.fogged, d.fog_on ? "on" : "off", d.fog_color, fs, fe, d.ambient, d.lights, d.vw, d.vh);
                }
            }
            game_detail::Line("mark %u %s", n, why.c_str());
        }
    }
    for (int i = 0; i < 2; i++)
        if (pass[i].draws) g_pass_last[i] = pass[i];
    if (!game_detail::On()) return;
    if (in != was_in) game_detail::Line(in ? "level in" : "level out"), was_in = in;
    if (!in) return;

    // ---- checks: something that should not be. Each writes a line, and asks for a full-size picture (a mark).
    {
        static uint64_t far1 = 0, far2 = 0, said1 = 0, said2 = 0, said3 = 0, level_frames = 0;
        static uint32_t draws_before = 0;
        static float eye_before[3] = {0, 0, 0};
        level_frames++;
        auto apart = [](const float* a, uint32_t object) {
            float d = 0;
            for (int i = 0; i < 3; i++) {
                float v = a[i] - ReadF(object + kPosition + i * 4);
                d += v * v;
            }
            return std::sqrt(d);
        };
        bool playing1 = (kernel::R32(g_player + 0xB0) & 1) && !g_one_view && !g_stopped_now && !(kernel::R32(g_player + 0xBC) & 0x600000);
        float d1 = c1.there ? apart(c1.eye, g_player_object) : 0;
        // A camera that stays where it is while its player walks off: he has moved more than 3 m in two seconds and
        // it has not moved a hand's breadth. (A following camera moves whenever he does; "far from him" alone needs
        // room to walk 15 m away, and a room is often smaller: run camfix.)
        struct Trail {
            float player[120][3] = {}, eye[120][3] = {};
            uint32_t filled = 0;
            bool Still(uint64_t frame, uint32_t body, const CameraNow& c, bool playing) {
                uint32_t at = uint32_t(frame % 120);
                float now_p[3], moved = 0, camera = 0;
                for (int i = 0; i < 3; i++) now_p[i] = ReadF(body + kPosition + i * 4);
                bool still = false;
                if (playing && c.there && filled >= 120) {
                    for (int i = 0; i < 3; i++) moved += (now_p[i] - player[at][i]) * (now_p[i] - player[at][i]), camera += (c.eye[i] - eye[at][i]) * (c.eye[i] - eye[at][i]);
                    still = moved > 3.0f * 3.0f && moved < 30.0f * 30.0f && camera < 0.1f * 0.1f;
                }
                filled = playing && c.there ? filled + 1 : 0;
                for (int i = 0; i < 3; i++) player[at][i] = now_p[i], eye[at][i] = c.eye[i];
                return still;
            }
        };
        static Trail trail1, trail2;
        static uint64_t said4 = 0;
        if (trail1.Still(g_frames, g_player_object, c1, playing1) && g_frames - said4 > 1200) {
            said4 = g_frames;
            LOGR("check: Player 1's camera has not moved for two seconds while he walked off (it is %.0f m from him; mode %u, %u on its stack, flags %08X)", d1, c1.mode, c1.depth, c1.flags);
            game_record::Mark("check: Player 1's camera does not follow him");
        }
        far1 = playing1 && d1 > 15.0f ? far1 + 1 : 0;
        if (far1 == 120 && g_frames - said1 > 1200) {
            said1 = g_frames;
            LOGR("check: Player 1's camera has been %.0f m from him for two seconds while he plays (mode %u, %u on its stack, flags %08X)", d1, c1.mode, c1.depth, c1.flags);
            game_record::Mark("check: Player 1's camera is far from him");
        }
        if (second) {
            bool playing2 = (kernel::R32(g_second + 0xB0) & 1) && !g_one_view && !g_stopped_now && !Down() && !(kernel::R32(g_second + 0xBC) & 0x600000);
            float d2 = c2.there ? apart(c2.eye, g_second_object) : 0;
            static uint64_t said5 = 0;
            if (trail2.Still(g_frames, g_second_object, c2, playing2) && g_frames - said5 > 600) {
                said5 = g_frames;
                LOGR("check: Player 2's camera has not moved for two seconds while he walked off (it is %.0f m from him; mode %u, %u on its stack, flags %08X)", d2, c2.mode, c2.depth, c2.flags);
                game_record::Mark("check: Player 2's camera does not follow him");
                RemakeCamera2(ctx, "it had stopped following him");
                return;
            }
            far2 = playing2 && (d2 > 15.0f || !c2.there) ? far2 + 1 : 0;
            if (far2 == 120 && g_frames - said2 > 600) {
                said2 = g_frames;
                LOGR("check: Player 2's camera has been %.0f m from him for two seconds while he plays (mode %u, %u on its stack, flags %08X)", d2, c2.mode, c2.depth, c2.flags);
                game_record::Mark("check: Player 2's camera is far from him");
                RemakeCamera2(ctx, "it had stopped following him");
            }
        } else far2 = 0;
        // what Player 1's view draws halves or doubles from one frame to the next while his camera has hardly moved
        uint32_t draws = pass[0].draws;
        float moved = 0;
        for (int i = 0; i < 3; i++) moved += (c1.eye[i] - eye_before[i]) * (c1.eye[i] - eye_before[i]);
        if (level_frames > 600 && playing1 && draws_before > 150 && draws > 0 && moved < 0.3f * 0.3f && (draws * 2 < draws_before || draws > draws_before * 2) &&
            g_frames - said3 > 1800) {
            said3 = g_frames;
            LOGR("check: Player 1's view went from %u to %u draws in one frame with his camera still", draws_before, draws);
            game_record::Mark("check: Player 1's view changed what it draws");
        }
        if (draws) draws_before = draws;
        for (int i = 0; i < 3; i++) eye_before[i] = c1.eye[i];
        if (!was_in) level_frames = 0;
    }

    // ---- the frame's line
    static bool told = false;
    if (!told) {
        told = true;
        game_detail::Line("f fields: frame, ms since the frame before, views (A one player, S split, O one view: a scene; p paused, m the game's time stopped without its pause menu: a movie or a level exit) | "
                          "1 = Player 1: x y z, facing, flags +B0, state +BC, health, insanity meter, class of what he uses (0 none) | c1 = his camera: eye x y z, "
                          "looking x y z, mode, modes on its stack, flags | 2 and c2 = Player 2 and his camera (- when there is none) | d1 = what Player 1's view drew "
                          "(with the 2D): draws, draws with fog, fog on, fog colour, fog start, fog end, ambient, lights, a mark of all its draws' fog and light "
                          "(changes when any changes), target size | d2 = the same for Player 2's view (- in frames it was not drawn) | s = the screen "
                          "flashes (pictures over the whole screen, the red one after an innocent is killed among them): how many, then of up to three "
                          "of them: strength, colour, texture (0 none), class of the object that owns it (P1 or P2: that Player's own, the flash when he is "
                          "hurt, drawn for him only) | t = the flashlights, Player 1's then "
                          "Player 2's: flags (1 on, 2 the battery is giving out, 4 the battery is dead) and charge in seconds (- he has none)");
    }
    char line[1200];
    int n = 0;
    auto player = [&](int who, uint32_t pl, uint32_t body, const CameraNow& c) {
        uint32_t used = Named(pl, 0x33E);
        n += std::snprintf(line + n, sizeof line - size_t(n), " | %d %.2f %.2f %.2f %.0f %08X %08X %.0f %.0f %X | c%d %.2f %.2f %.2f %.2f %.2f %.2f %u %u %X", who,
                           ReadF(body + kPosition), ReadF(body + kPosition + 4), ReadF(body + kPosition + 8), Facing(body), kernel::R32(pl + 0xB0), kernel::R32(pl + 0xBC),
                           ReadF(pl + 0x16), ReadF(pl + 0x48A), used ? ClassOf(used) : 0, who, c.eye[0], c.eye[1], c.eye[2], c.way[0], c.way[1], c.way[2], c.mode, c.depth,
                           c.flags);
    };
    n += std::snprintf(line + n, sizeof line - size_t(n), "f %llu %.1f %c%s", (unsigned long long)g_frames, ms,
                       g_one_view ? 'O' : second ? 'S' : 'A', g_paused_now ? "p" : g_stopped_now ? "m" : "");
    player(1, g_player, g_player_object, c1);
    if (second) player(2, g_second, g_second_object, c2);
    else n += std::snprintf(line + n, sizeof line - size_t(n), " | 2 - | c2 -");
    for (int i = 0; i < 2; i++) {
        const PassStats& d = pass[i];
        if (!d.draws) {
            n += std::snprintf(line + n, sizeof line - size_t(n), " | d%d -", i + 1);
            continue;
        }
        float fs, fe;
        std::memcpy(&fs, &d.fog_start, 4), std::memcpy(&fe, &d.fog_end, 4);
        n += std::snprintf(line + n, sizeof line - size_t(n), " | d%d %u %u %d %08X %.1f %.1f %08X %X %04X %ux%u", i + 1, d.draws, d.fogged, int(d.fog_on),
                           d.fog_color, fs, fe, d.ambient, d.lights, unsigned(d.hash & 0xFFFF), d.vw, d.vh);
    }
    {   // the screen flashes: the pictures the game lays over the whole screen (this = 0x391C70: a list at +6 of
        // entries of 0x16 bytes, their number at +0xE; an entry: order, strength, -, colour, texture, the handle of
        // the object that owns it and gives it its strength every frame through 0x113580)
        constexpr uint32_t kFlashes = 0x391C70;
        uint32_t list = kernel::R32(kFlashes) ? kernel::R32(kFlashes + 6) : 0, count = list ? kernel::R32(kFlashes + 0xE) : 0;
        if (count > 32 || (count && !mem::IsCommitted(list + count * 0x16))) count = 0;
        uint32_t table = kernel::R32(kObjectManager + 0x0C);
        static std::vector<uint32_t> owners_before;
        std::vector<uint32_t> owners;
        n += std::snprintf(line + n, sizeof line - size_t(n), " | s %u", count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t e = list + i * 0x16, h = kernel::R16(e + 0x14);
            uint32_t owner = table && h ? kernel::R32(table + h * 6) : 0;
            uint32_t cls = owner && mem::Spans(owner, 0x110) ? ClassOf(owner) : 0;
            int whose = !owner ? 0 : owner == g_player_object ? 1 : owner == g_second_object ? 2 : 0;
            float strength = ReadF(e + 4);
            char who[12];
            if (whose) std::snprintf(who, sizeof who, "P%d", whose);
            else std::snprintf(who, sizeof who, "%08X", cls);
            if (i < 3) n += std::snprintf(line + n, sizeof line - size_t(n), " %.2f %08X %X %s", strength, kernel::R32(e + 0x0C), kernel::R32(e + 0x10), who);
            owners.push_back(h);
            // (a Player keeps his entry for good, at strength 0 between hits: his are not announced)
            if (!whose && std::find(owners_before.begin(), owners_before.end(), h) == owners_before.end())
                LOGI("coop: a picture over the whole screen begins (%u now): strength %.2f, colour %08X, texture %X, owned by an object of class %08X", count, strength,
                     kernel::R32(e + 0x0C), kernel::R32(e + 0x10), cls);
            // tests: S2_TEST_FLASH_MARK=1 asks for a full-size picture when a Player's own flash comes up (three for each)
            static const bool flash_mark = std::getenv("S2_TEST_FLASH_MARK") != nullptr;
            static float strength_before[3] = {};
            static int marked[3] = {};
            if (whose) {
                if (flash_mark && strength >= 0.15f && strength_before[whose] < 0.15f && marked[whose]++ < 3) {
                    char why[96];
                    std::snprintf(why, sizeof why, "test: Player %d's own screen flash came up (strength %.2f)", whose, strength);
                    game_record::Mark(why);
                }
                strength_before[whose] = strength;
            }
        }
        owners_before.swap(owners);
    }
    {   // the flashlights: each player's, its flags (1 on, 2 its battery is giving out, 4 the battery is dead) and charge
        n += std::snprintf(line + n, sizeof line - size_t(n), " | t");
        for (uint32_t who : {g_player, second ? g_second : 0u}) {
            uint32_t light = FlashlightOf(who);
            if (light) n += std::snprintf(line + n, sizeof line - size_t(n), " %X %.0f", kernel::R32(light + 0xA8) & 7, ReadF(light + 0xAC));
            else n += std::snprintf(line + n, sizeof line - size_t(n), " -");
        }
    }
    game_detail::Line("%s", line);

    // ---- once a second: every object that is new, has moved, has changed or has ended
    if (g_frames % 60 == 7) {
        struct Seen {
            uint32_t object = 0, flags = 0, model = 0;
            float at[3] = {0, 0, 0};
            uint64_t round = 0;
        };
        static std::map<uint32_t, Seen> seen;
        static uint64_t round = 0;
        static uint32_t level = 0;
        if (level != g_player) level = g_player, seen.clear(), game_detail::Line("o fields: handle, class, template, model, x y z, the object's flags (+8), who (1 or 2: a Player; h1 or h2: in his hand)");
        round++;
        uint32_t table = kernel::R32(kObjectManager + 0x0C), written = 0;
        const uint32_t hands[4] = {Named(g_player, 0x3AC), Named(g_player, 0x3AE), second ? Named(g_second, 0x3AC) : 0, second ? Named(g_second, 0x3AE) : 0};
        // (the table has as many entries as the word at the object manager +0x14 says, and an object knows its own
        // handle, +0xE6: until 2026-11-26 this ran on to 0x4000, into memory that is not the table, and the record
        // had "objects" in it that were old copies of addresses, with wild places)
        uint32_t entries = std::min<uint32_t>(kernel::R32(kObjectManager + 0x14), 0x4000), other_handle = 0, objects = 0;
        for (uint32_t h = 1; table && h < entries && mem::IsCommitted(table + h * 6 + 4); h++) {
            uint32_t object = kernel::R32(table + h * 6);
            if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
            objects++;
            if (kernel::R16(object + 0xE6) != h) {
                other_handle++;
                continue;
            }
            Seen& was = seen[h];
            float at[3] = {ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8)};
            uint32_t flags = kernel::R32(object + 8), model = kernel::R32(object + 0x64);
            float d = 0;
            for (int i = 0; i < 3; i++) d += (at[i] - was.at[i]) * (at[i] - was.at[i]);
            bool changed = was.object != object || d > 0.05f * 0.05f || flags != was.flags || model != was.model;
            was.round = round;
            if (!changed) continue;
            was.object = object, was.flags = flags, was.model = model;
            std::memcpy(was.at, at, sizeof at);
            const char* who = object == g_player_object ? " 1" : object == g_second_object ? " 2" : object == hands[0] || object == hands[1] ? " h1" :
                              object && (object == hands[2] || object == hands[3]) ? " h2" : "";
            if (written++ < 1500)
                game_detail::Line("o %X %08X %u %X %.2f %.2f %.2f %08X%s", h, ClassOf(object), kernel::R32(object + kTemplateWord) & 0xFFFF, model, at[0], at[1], at[2], flags, who);
        }
        for (auto it = seen.begin(); it != seen.end();) {
            if (it->second.round == round) {
                ++it;
                continue;
            }
            game_detail::Line("x %X", it->first);
            it = seen.erase(it);
        }
        {
            static int said = 0;
            if (said < 2 && objects) said++, LOGI("record: the handle table has %u entries, %u of them objects, %u of those under another handle than their own (left out)", entries, objects, other_handle);
        }
    }
}

// runs a part of the record under a structured-exception guard: false when a read in it faulted
// (no C++ objects here: the guard and C++ unwinding do not mix in one function)
bool RecordGuarded(void (*part)(X86Context&), X86Context& ctx) {
    __try {
        part(ctx);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

void Frame(X86Context& ctx) {
    static const uint64_t spawn_at = [] {
        const char* v = std::getenv("S2_COOP_SPAWN");
        return v ? uint64_t(std::atoll(v)) : 0;
    }();
    static const bool forced = std::getenv("S2_COOP") != nullptr;
    ++g_frames;
    {   // The per-frame record only reads: a read it gets wrong (an object the game has just let go of) leaves that
        // frame's line out and is noted, and never ends the game (three of the four crashes of the owner's
        // versions 63 to 67 were reads in this record).
        X86Context before = ctx;
        if (!RecordGuarded(DetailFrame, ctx)) {
            ctx = before;
            static int said = 0;
            if (said < 20) said++, LOGR("record: frame %llu's line was left out after a bad read", (unsigned long long)g_frames);
        }
    }
    // ---- Both players' ammunition never runs out (the owner, 2026-11-28: "can u give player 2 infinite ammo", then
    // "give player 1 unlmitied ammo too"): the rounds in the gun in a player's hands (Player +0x140) are kept at what
    // its clip holds (0x17400; 0 for a melee weapon) every frame, so he never reloads and never takes from what he
    // carries. (A creature form, state bits 0xC0000, has no gun.)
    for (uint32_t who : {g_player, g_second}) {
        if (!who || !mem::Spans(who, 0x540) || (kernel::R32(who + 0xBC) & 0xC0000)) continue;
        uint32_t ecx = ctx.ecx, eax = ctx.eax;
        ctx.ecx = who;
        int clip = int(kernel::CallGuest(ctx, 0x17400, {}));
        ctx.ecx = ecx, ctx.eax = eax;
        int rounds = int(kernel::R32(who + 0x140));
        if (clip > 0 && clip < 1000 && rounds < clip) {
            kernel::W32(who + 0x140, uint32_t(clip));
            static int said = 0;
            if (said < 5) said++, LOGI("ammo: Player %d's gun filled again (%d of %d), infinite ammunition", who == g_second ? 2 : 1, rounds, clip);
        }
    }
    {   // tests: S2_FP_PROBE=<frame>: Player 1's model, its resource and his skeleton at that frame and 180 frames later
        // (a script goes into first person in between): what the first-person model shares with the body
        static const uint64_t probe = std::getenv("S2_FP_PROBE") ? uint64_t(std::atoll(std::getenv("S2_FP_PROBE"))) : 0;
        if (probe && (g_frames == probe || g_frames == probe + 180) && g_player && g_player_object) {
            uint32_t o = g_player_object, res = kernel::R32(o + 0x68), sk = kernel::R32(o + 0xE8);
            LOGR("fp probe: frame %llu: object %08X model id %08X resource %08X skeleton %08X; Player +760 %08X +764 %08X +768 %08X +76C %08X; state %08X",
                 (unsigned long long)g_frames, o, kernel::R32(o + 0x64), res, sk, kernel::R32(g_player + 0x760), kernel::R32(g_player + 0x764),
                 kernel::R32(g_player + 0x768), kernel::R32(g_player + 0x76C), kernel::R32(g_player + 0xBC));
            if (res && mem::Spans(res, 0x80)) Dump("fp probe resource", res, 0x80);
            if (sk && mem::Spans(sk, 0xC0)) Dump("fp probe skeleton", sk, 0xC0);
            for (uint32_t at : {0x760u, 0x764u}) {
                uint32_t r = kernel::R32(g_player + at);
                if (r && mem::Spans(r, 0x80)) Dump(at == 0x760 ? "fp probe +760" : "fp probe +764", r, 0x80);
            }
        }
    }
    {   // tests: S2_TEST_GIVE_WEAPONS=<frame>: Player 1 is given every melee and ranged weapon the game has a template
        // for (to see what hangs on his body: holsters, the bat on his back)
        static const uint64_t give_at = std::getenv("S2_TEST_GIVE_WEAPONS") ? uint64_t(std::atoll(std::getenv("S2_TEST_GIVE_WEAPONS"))) : 0;
        if (give_at && g_frames == give_at && g_player) {
            uint32_t table = kernel::R32(0x384BA4), count = table ? kernel::R32(table + 0x18) : 0, given = 0;  // (the templates, kTemplates)
            for (uint32_t i = 0; i < count && i < 20000; i++) {
                uint32_t cls = ClassOfTemplate(i);
                if (cls != 0x22E48520 && cls != 0x9710D816) continue;
                uint32_t ecx = ctx.ecx, eax = ctx.eax;
                ctx.ecx = g_player;
                kernel::CallGuest(ctx, 0x153A0, {i, 1});
                ctx.ecx = ecx, ctx.eax = eax;
                given++;
            }
            LOGR("test: Player 1 is given %u weapons", given);
        }
    }
    {   // tests: S2_DRAW_TRACE=<frame> also asks the runtime for that frame's ready-made (2D) draws with their boxes
        static const uint64_t trace_at = std::getenv("S2_DRAW_TRACE") ? uint64_t(std::atoll(std::getenv("S2_DRAW_TRACE"))) : 0;
        // (three presents: the first may be this frame's own, already under way, and Player 2's view for the stream is
        // drawn in every second frame only)
        if (trace_at && g_frames == trace_at) gpu::g_trace_2d = 3;
    }
    {   // tests: S2_TEST_MARK=<frame>[,<frame>...] asks for a mark at those frames, as F9 does
        static const std::vector<uint64_t> marks = [] {
            std::vector<uint64_t> list;
            for (const char* e = std::getenv("S2_TEST_MARK"); e && *e; e = std::strchr(e, ',') ? std::strchr(e, ',') + 1 : nullptr) list.push_back(uint64_t(std::atoll(e)));
            return list;
        }();
        for (uint64_t at : marks)
            if (at && g_frames == at) game_record::Mark("a test asked");
    }
    static const uint64_t off_at = std::getenv("S2_COOP_OFF") ? uint64_t(std::atoll(std::getenv("S2_COOP_OFF"))) : 0;  // tests
    bool test_on = (forced || (spawn_at && g_frames + 61 >= spawn_at)) && !(off_at && g_frames >= off_at);
    g_wanted = test_on || g_mode_setting == 1;
    Session(ctx, g_wanted);
    // tests: S2_TEST_GOD=1: nothing hurts Player 1 (the game's own mark, +0x1E of anything with health), for long
    // scripted runs into a level
    static const bool god = std::getenv("S2_TEST_GOD") != nullptr;
    if (god && g_player && kernel::R32(g_player + 0x1E) != 1) kernel::W32(g_player + 0x1E, 1);
    TestEnemy(ctx);
    TestLineup(ctx);
    TestPut(ctx);
    TestTorchAgain(ctx);
    TestSpot(ctx);
    TestFace(ctx);
    TestExit(ctx);
    TestCensus();
    Flashlights();
    TestMeter(ctx);
    TestCall(ctx);
    TestBody();
    TrailStep();
    Ears();
    {   // S2_TEST_MORAL=<frame>,<player 1 or 2>,<change>: the game's own morality change (0x46B60: this = Player, a
        // float) for that player at that frame, to see the moment it sets off
        static const char* v = std::getenv("S2_TEST_MORAL");
        if (v && g_player && g_frames == uint64_t(std::atoll(v))) {
            const char* c = std::strchr(v, ',');
            int who = c ? std::atoi(c + 1) : 1;
            const char* d = c ? std::strchr(c + 1, ',') : nullptr;
            float change = d ? float(std::atof(d + 1)) : -10.0f;
            uint32_t player = who == 2 && g_second ? g_second : g_player, bits, ecx = ctx.ecx;
            std::memcpy(&bits, &change, 4);
            LOGR("test: morality change %.1f for Player %d (it is %.2f)", change, who, ReadF(g_player + 0x482));
            ctx.ecx = player;
            kernel::CallGuest(ctx, 0x46B60, {bits});
            ctx.ecx = ecx;
            LOGR("test: morality is now %.2f", ReadF(g_player + 0x482));
        }
    }
    TestSound(ctx);
    TestSounds(ctx);
    {   // S2_TEST_OBJCLR=<frame>,<offset hex>,<mask hex>[,1 to set]: clears (or sets) bits of Player 2's object from that
        // frame on, and logs both players' object words once (looking for what makes two characters solid)
        static const char* v = std::getenv("S2_TEST_OBJCLR");
        if (v && g_second_object && g_player_object && g_frames >= uint64_t(std::atoll(v))) {
            uint32_t off = 0, mask = 0, set = 0;
            if (const char* c = std::strchr(v, ',')) {
                off = uint32_t(std::strtoul(c + 1, nullptr, 16));
                if (const char* d = std::strchr(c + 1, ',')) {
                    mask = uint32_t(std::strtoul(d + 1, nullptr, 16));
                    if (const char* e = std::strchr(d + 1, ',')) set = uint32_t(std::atoi(e + 1));
                }
            }
            static bool said = false;
            if (!said) {
                said = true;
                for (uint32_t o : {g_player_object, g_second_object}) {
                    char line[400];
                    int n = std::snprintf(line, sizeof line, "test: object %08X words", o);
                    for (uint32_t at = 0; at < 0x14; at += 4) n += std::snprintf(line + n, sizeof line - n, " %08X", kernel::R32(o + at));
                    n += std::snprintf(line + n, sizeof line - n, " | +E0:");
                    for (uint32_t at = 0xE0; at < 0x100; at += 4) n += std::snprintf(line + n, sizeof line - n, " %08X", kernel::R32(o + at));
                    LOGR("%s", line);
                }
            }
            uint32_t now = kernel::R32(g_second_object + off);
            if (mask) kernel::W32(g_second_object + off, set ? now | mask : now & ~mask);
        }
    }
    TestGoto(ctx);
    TestClasses(ctx);
    HitsSecond();
    OrderViews();
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
    {   // The game's time stops (timer flag bit 0) for its pause menu (flags 0x5081: bit 0x4000 is the menu's), for a
        // movie (0x1081: the visions; the owner's session 015 at 1393.9 s, a Movie Player's black picture over the
        // screen and then the film), at a level exit (0x1481), and for a frame or two at a security monitor and when a
        // level starts (which read as "Player 1 paused the game" for 10 to 30 ms in suffering2_011.log: so only a stop
        // of more than a moment counts). Until 2026-11-27 every stop was a pause: a movie read as "Player 1 paused the
        // game", and the line between the two views was drawn over the film ("sometimes the split screen thing shows
        // over cutscenes when it shouldnt (the line)", the owner). While the time is stopped the views behind are
        // still pictures: no line between them, no partner bars, no arrow, no ammunition for Player 2 (below).
        uint32_t timer = kernel::R32(0x384B78), flags = timer ? kernel::R32(timer + 0xA0) : 0;
        static int stopped = 0;
        stopped = timer && (flags & 1) && g_second_object ? stopped + 1 : 0;
        bool still = stopped >= 6, paused = still && (flags & 0x4000);
        static bool told = false;  // (a stop without the pause menu was told: its end is told too)
        if (still && !g_stopped_now && !paused) told = true, LOGI("coop: the game's time stops without its pause menu: a movie, or a level exit (timer flags %X)", flags);
        if (!still && g_stopped_now && told) told = false, LOGI("coop: the game's time goes on (timer flags %X)", flags);
        g_stopped_now = still;
        g_paused_now = paused;
        int before = g_pauser.load();
        if (paused && !before) {
            bool second = std::chrono::steady_clock::now() - g_start2_at < std::chrono::milliseconds(700);
            g_pauser = second ? 2 : 1;
            LOGI("coop: Player %d paused the game", second ? 2 : 1);
        } else if (!paused && before) {
            g_pauser = 0;
            LOGI("coop: the game goes on");
        }
        game_menu::PauseTitle(ctx, g_pauser.load());
    }
    if (g_teleport2.exchange(false) && g_second_object && !g_one_view) {
        BesideFirst(ctx);
        LOGI("coop: Player 2 teleports to Player 1 (A+B+X+Y held five seconds)");
        // (his way out of a camera that is stuck, too: when it is not in its following mode or is not his, it is made
        // anew; first person and what he uses have their own modes and are left alone)
        CameraNow c = CameraOf(g_camera2_object);
        bool busy = g_second && (kernel::R32(g_second + 0xBC) & 0x648000) != 0;
        if (!busy && c.there && (c.mode != 3 || (c.flags & 1))) RemakeCamera2(ctx, "Player 2 teleported and it was not in its following mode");
    }
    {   // tests: S2_TEST_CAM2_STUCK=<frame>: a mode that nobody moves the camera in is put on Player 2's Camera at that
        // frame, as a stuck one would have (0x103750: this = Camera, mode)
        static const uint64_t stuck_at = std::getenv("S2_TEST_CAM2_STUCK") ? uint64_t(std::atoll(std::getenv("S2_TEST_CAM2_STUCK"))) : 0;
        if (stuck_at && g_frames == stuck_at && g_camera2_object) {
            uint32_t camera = kernel::R32(g_camera2_object + kClassObject), ecx = ctx.ecx;
            ctx.ecx = camera;
            kernel::CallGuest(ctx, 0x103750, {0});
            ctx.ecx = ecx;
            LOGR("test: a mode 0 is put on Player 2's Camera: nobody moves it now");
        }
    }
    if (ViewCamera2() && !g_one_view && !g_stopped_now) {
        // this frame's picture: a bar where the two views meet, unless one of the game's own pictures over the whole
        // screen is up (a vision, a fade, the red picture after an innocent is killed: the screen flashes, this =
        // 0x391C70, a list at +6 of 0x16-byte entries, their number at +0xE, the strength a float at +4). The game
        // lays those over the whole frame, both halves as one picture, and the bar was drawn over them (the owner,
        // 2026-11-27: "the overlay pictures should go over the split in the splitscreen"). A Player's own flash
        // (when he is hurt) is in his half only and leaves the bar.
        bool over_all = false;
        {
            constexpr uint32_t kFlashes = 0x391C70;
            uint32_t list = kernel::R32(kFlashes) ? kernel::R32(kFlashes + 6) : 0, count = list ? kernel::R32(kFlashes + 0xE) : 0;
            if (count > 32 || (count && !mem::IsCommitted(list + count * 0x16))) count = 0;
            for (uint32_t i = 0; i < count && !over_all; i++) {
                float strength = ReadF(list + i * 0x16 + 4);
                uint32_t h = kernel::R16(list + i * 0x16 + 0x14), table = kernel::R32(kObjectManager + 0x0C);
                uint32_t owner = table && h ? kernel::R32(table + h * 6) : 0;
                bool players = owner && (owner == g_player_object || owner == g_second_object);
                over_all = strength > 0.02f && strength < 100.0f && !players;
            }
        }
        gpu::g_split_vertical = true;
        if (!over_all) gpu::g_split_divider = 2;
        // ... and Player 2's health bar, drawn by the runtime where his half's HUD would have it (the game's HUD
        // keeps one set of values, Player 1's). A Player's health is the float at +0x16, of the most at +0x1A.
        // Player 2's most is Player 1's: the game's own difficulty (until 2026-11-27 it was Player 1's divided by
        // 1.3; the owner then: "lets bring that back down to 0 and just have the game be regular difficulty"). Kept
        // so every frame: the game sets a Player's most itself at times, and Player 1's grows with the story.
        if (g_second && g_player) {
            float most1 = ReadF(g_player + 0x1A), most2 = ReadF(g_second + 0x1A), want = most1;
            if (most1 > 0 && std::fabs(most2 - want) > 0.5f) {
                float health = ReadF(g_second + 0x16);
                if (most2 > 0 && !g_down_until) health = std::min(want, health * want / most2);  // the same share of it
                std::memcpy(kernel::HostPtr(g_second + 0x1A), &want, 4);
                if (!g_down_until) std::memcpy(kernel::HostPtr(g_second + 0x16), &health, 4);
                LOGI("coop: Player 2's most health is %.0f (Player 1's)", want);
            }
        }
        float most = g_second ? ReadF(g_second + 0x1A) : 0;
        gpu::g_second_health = most > 0 ? (g_down_until ? 0.0f : std::clamp(ReadF(g_second + 0x16) / most, 0.0f, 1.0f)) : -1.0f;
        // ... and each player's partner's, thin, under his own (Player 2's blue): Player 1's health for Player 2's view
        // ... and Player 2's insanity meter (+0x48A, 0 to 100: the title's shows Player 1's only)
        gpu::g_second_meter = g_second && !g_down_until ? std::clamp(ReadF(g_second + 0x48A) / 100.0f, 0.0f, 1.0f) : -1.0f;
        float most1 = g_player ? ReadF(g_player + 0x1A) : 0;
        gpu::g_first_health = most1 > 0 ? std::clamp(ReadF(g_player + 0x16) / most1, 0.0f, 1.0f) : -1.0f;
        // The arrow above the partner's head (decided 2026-11-24: tiny, the game's orange, through walls, no name
        // tags). Where the partner's head is in each player's view: the view camera's object gives the eye
        // (matrix at +0x14: right, up, forward, place), its distance at +0x280 the angle (half the picture's width
        // is 0.3125 at that distance, view.cpp), the view's shape the angle up and down. Behind the viewer, or
        // out of the picture: no arrow. S2_ARROW_TEST=<metres up> moves it (1.95 above his feet unless named).
        {
            static const float up = std::getenv("S2_ARROW_TEST") ? float(std::atof(std::getenv("S2_ARROW_TEST"))) : 1.74f;
            uint32_t view = kernel::R32(kViewPointer), cameras = view ? kernel::R32(view + kViewCameras) : 0;
            uint32_t cams[2] = {cameras, ViewCamera2()};
            uint32_t partner[2] = {g_second_object, g_player_object};
            float shape = game_view::Aspect() * 0.5f;
            for (int v = 0; v < 2; v++) {
                float x = -1, y = -1;
                uint32_t eye = cams[v] && mem::Spans(cams[v], 0x284) ? kernel::R32(cams[v] + 0xA0) : 0;
                float distance = eye ? ReadF(cams[v] + 0x280) : 0;
                bool shown = !g_down_until;  // (Player 2 is out of sight while he is down, and sees nothing himself)
                if (eye && mem::Spans(eye, 0x60) && distance > 1e-4f && partner[v] && shown) {
                    float m[16], d[3];
                    std::memcpy(m, kernel::HostPtr(eye + kMatrix), 64);
                    for (int i = 0; i < 3; i++) d[i] = ReadF(partner[v] + kPosition + i * 4) - m[12 + i];
                    d[1] += up;
                    // (his head is not straight above the point his object is at: Torque stands a little turned, and
                    // the arrow hung about a hand's width to one side of his head, the same side in every picture.
                    // It is moved that much along his own first row. S2_ARROW_SIDE=<metres> for trying.)
                    static const float side = std::getenv("S2_ARROW_SIDE") ? float(std::atof(std::getenv("S2_ARROW_SIDE"))) : 0.05f;
                    for (int i = 0; i < 3; i++) d[i] += side * ReadF(partner[v] + kMatrix + i * 4);
                    float rx = d[0] * m[0] + d[1] * m[1] + d[2] * m[2], ry = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
                    float rz = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
                    // (a camera's object looks along its third row backwards: run arrow3, the partner straight ahead
                    // came out at forward -9; its first row is its right all the same, run arrow4)
                    rz = -rz;
                    static const bool trace2 = std::getenv("S2_ARROW_TRACE") != nullptr;
                    if (trace2 && g_frames % 120 == 0) {
                        char line[400];
                        int n = std::snprintf(line, sizeof line, "arrow: view %d camera floats", v);
                        for (uint32_t at = 0x260; at < 0x2C4; at += 4) n += std::snprintf(line + n, sizeof line - n, " %X:%.4g", at, ReadF(cams[v] + at));
                        LOGI("%s", line);
                    }
                    if (trace2 && g_frames % 120 == 0) LOGI("arrow: view %d to partner %.2f %.2f %.2f: right %.2f up %.2f forward %.2f", v, d[0], d[1], d[2], rx, ry, rz);
                    // The view camera keeps its own scales at +0x26C and +0x270: how many half-views a point one
                    // metre to the side (or up) at one metre ahead is from the middle (-2.366 and -2.103 for a half of
                    // a 16:9 split screen). Their inverses are the half picture's width and height at one metre.
                    // (First worked out from the camera's distance and the picture's shape, which was right for the
                    // split screen and wrong for the whole-frame view of a joined player: run arrow_join2.)
                    float kx = std::fabs(ReadF(cams[v] + 0x26C)), ky = std::fabs(ReadF(cams[v] + 0x270));
                    float half_w = kx > 1e-3f ? 1.0f / kx : 0.3125f / distance, half_h = ky > 1e-3f ? 1.0f / ky : half_w / shape;
                    if (rz > 0.3f) {
                        float sx = 0.5f + rx / rz / (2 * half_w), sy = 0.5f - ry / rz / (2 * half_h);
                        // (The point is the top of his head, 1.74 m above his object's place: measured in the joined
                        // player's picture, where the old 1.95 put the arrow a hand's width too high at three metres.
                        // The gap between head and arrow is a share of the picture, the same near and far.)
                        sy -= 0.012f;
                        if (sx > 0.0f && sx < 1.0f && sy > 0.0f && sy < 1.0f) x = sx, y = sy;
                    }
                }
                static const bool trace = std::getenv("S2_ARROW_TRACE") != nullptr;
                if (trace && g_frames % 120 == 0)
                    LOGI("arrow: view %d camera %08X eye %08X distance %.4f shape %.3f -> %.3f %.3f", v, cams[v], eye, distance, shape, x, y);
                gpu::g_partner_arrow[v].x = x;
                gpu::g_partner_arrow[v].y = y;
            }
        }
    } else {
        gpu::g_partner_arrow[0].x = gpu::g_partner_arrow[1].x = -1.0f;
        gpu::g_second_health = -1.0f;
        gpu::g_first_health = -1.0f;
    }
    {   // the record: what each player is doing, when it changes (the Player's state word, +0xBC)
        static uint32_t before[2] = {0, 0};
        static const struct {
            uint32_t bit;
            const char* on;
            const char* off;
        } kStates[] = {{0x8000, "goes into first person", "leaves first person"},
                       {0x40000, "begins to change form", "has changed form"},
                       {0x200000, "uses something", "is done using it"},
                       {0x400000, "looks at a security monitor", "leaves the security monitor"}};
        const uint32_t players[2] = {g_player, g_second};
        for (int i = 0; i < 2; i++) {
            if (!players[i] || !mem::Spans(players[i], 0xBC)) {
                before[i] = 0;
                continue;
            }
            uint32_t now = kernel::R32(players[i] + 0xBC), flags = kernel::R32(players[i] + 0xB0);
            for (const auto& st : kStates) {
                if (!((now ^ before[i]) & st.bit)) continue;
                if (st.bit != 0x200000 || !(now & st.bit)) {
                    LOGI("coop: Player %d %s (state %08X, flags %08X)", i + 1, (now & st.bit) ? st.on : st.off, now, flags);
                    continue;
                }
                // (what it is and where: the two logs of 2026-11-26 could not tell a ledge from a ladder from a box)
                uint32_t o = Named(players[i], 0x33E);
                if (!o) o = Named(players[i], 0x340);
                uint32_t cls = o ? ClassOf(o) : 0, body = i ? g_second_object : g_player_object;
                const char* name = KindName(cls);
                LOGI("coop: Player %d uses %s (class %08X at %.1f %.1f %.1f; he stands at %.2f %.2f %.2f; state %08X, flags %08X)", i + 1,
                     name ? name : "something", cls, o ? ReadF(o + kPosition) : 0.f, o ? ReadF(o + kPosition + 4) : 0.f, o ? ReadF(o + kPosition + 8) : 0.f,
                     body ? ReadF(body + kPosition) : 0.f, body ? ReadF(body + kPosition + 4) : 0.f, body ? ReadF(body + kPosition + 8) : 0.f, now, flags);
            }
            // Player 2 is off a ledge: a ledge stretches the climb to its height through the scale of the user's
            // skeleton ([object +0xE8] +0x64, three floats; 0xD93D0 sets it, 0xD92A0 puts 1 back, both for "the
            // player"). Should that way back ever be taken for Player 1 while it was Player 2 who climbed, he is
            // not left stretched.
            if (i == 1 && (before[i] & ~now & 0x200000) && g_second_object && mem::Spans(g_second_object, 0xEC)) {
                uint32_t skeleton = kernel::R32(g_second_object + 0xE8);
                if (skeleton && mem::Spans(skeleton, 0x70)) {
                    float sx = ReadF(skeleton + 0x64), sy = ReadF(skeleton + 0x68), sz = ReadF(skeleton + 0x6C);
                    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
                        const float one = 1.0f;
                        for (uint32_t at : {0x64u, 0x68u, 0x6Cu}) std::memcpy(kernel::HostPtr(skeleton + at), &one, 4);
                        LOGI("coop: Player 2's skeleton was left scaled %.2f %.2f %.2f after what he used: put back to 1", sx, sy, sz);
                    }
                }
            }
            before[i] = now;
        }
    }
    {   // S2_USE_TRACE=1: once a second, what each player has on his list of things to use (+0x342, 6 bytes each: the
        // object's handle and the touch's word; count +0x34A) and what his two handles name
        static const bool trace = std::getenv("S2_USE_TRACE") != nullptr;
        if (trace && g_frames % 60 == 30) {
            const uint32_t players[2] = {g_player, g_second};
            uint32_t table = kernel::R32(kObjectManager + 0x0C);
            for (int i = 0; i < 2; i++) {
                uint32_t pl = players[i];
                if (!pl || !mem::Spans(pl, 0x540)) continue;
                std::string line;
                char one[64];
                uint32_t list = kernel::R32(pl + 0x342), count = kernel::R32(pl + 0x34A);
                for (uint32_t k = 0; list && k < count && k < 8; k++) {
                    uint32_t h = kernel::R16(list + k * 6), o = table && h ? kernel::R32(table + h * 6) : 0;
                    std::snprintf(one, sizeof one, " %08X(%X)", o && mem::Spans(o, 0x110) ? ClassOf(o) : 0, kernel::R32(list + k * 6 + 2));
                    line += one;
                }
                uint32_t h1 = kernel::R16(pl + 0x33E), h2 = kernel::R16(pl + 0x340);
                uint32_t o1 = table && h1 ? kernel::R32(table + h1 * 6) : 0, o2 = table && h2 ? kernel::R32(table + h2 * 6) : 0;
                LOGI("use trace: Player %d can use %u things:%s; +33E names %08X, +340 names %08X; looks at %u; +B0 %08X +BC %08X", i + 1, count, line.c_str(),
                     o1 && mem::Spans(o1, 0x110) ? ClassOf(o1) : 0, o2 && mem::Spans(o2, 0x110) ? ClassOf(o2) : 0, kernel::R32(pl + 0x53A),
                     kernel::R32(pl + 0xB0), kernel::R32(pl + 0xBC));
            }
        }
    }
    {   // the record: a level comes into play, a level is left; a player's health reaches nothing
        static bool in_before = false;
        static std::chrono::steady_clock::time_point since{};
        bool in = InLevel();
        if (in != in_before) {
            if (in) LOGI("level: a level is in play (Player 1 is %08X)", g_player);
            else LOGI("level: the level was left after %.0f s of play", std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count());
            since = std::chrono::steady_clock::now(), in_before = in;
        }
        static bool down[2] = {false, false};
        const uint32_t players[2] = {g_player, g_second};
        for (int i = 0; i < 2; i++) {
            bool now = players[i] && mem::Spans(players[i], 0x20) && ReadF(players[i] + 0x16) <= 0.0f;
            if (now != down[i] && players[i]) LOGI("coop: Player %d's health %s", i + 1, now ? "is gone: he is down" : "is back");
            down[i] = now;
        }
    }
    if (g_frames % 60 == 0 && g_player && g_player_object && InLevel()) {  // (what else each player is, once a second)
        uint32_t view = kernel::R32(kViewPointer);
        char two[96] = "";
        if (g_second && mem::Spans(g_second, 0x490))
            std::snprintf(two, sizeof two, "; Player 2 %08X %08X meter %.0f", kernel::R32(g_second + 0xB0), kernel::R32(g_second + 0xBC), ReadF(g_second + 0x48A));
        LOGI("state: Player 1 %08X %08X %02X meter %.0f morality %.0f%s; %u view cameras, %s", kernel::R32(g_player + 0xB0), kernel::R32(g_player + 0xBC), kernel::R32(g_player + 0xC0) & 0x7F,
             ReadF(g_player + 0x48A), ReadF(g_player + 0x482), two, view ? kernel::R32(view + kViewCount) : 0,
             g_one_view ? "one view" : g_second_object ? "split" : "alone");
    }
    if (g_frames % 60 == 0 && !g_second_object && g_player_object && g_player && InLevel()) {  // (alone: one line a second too)
        LOGI("coop: player 1 at %.2f %.2f %.2f facing %.0f health %.0f, alone", ReadF(g_player_object + kPosition), ReadF(g_player_object + kPosition + 4),
             ReadF(g_player_object + kPosition + 8), std::atan2(ReadF(g_player_object + kMatrix + 0x20), ReadF(g_player_object + kMatrix + 0x28)) * 57.29578f,
             ReadF(g_player + 0x16));
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
        if (g_test_enemy && mem::Spans(g_test_enemy, 0x110) && !(kernel::R32(g_test_enemy + 8) & 0x80010000))
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

// for the choice of character (skins.cpp): whose body an engine object is, and the players' Players
int BodyOwner(uint32_t object) {
    if (!object) return -1;
    if (object == g_player_object) return 0;
    if (object == g_second_object) return 1;
    return DoubleObject(object);
}
uint32_t PlayerOf(int who) { return who ? (g_second_object ? g_second : 0) : g_player; }
uint32_t DoublePlayerOf(int who) { return g_doubles[who & 1].object ? g_doubles[who & 1].player : 0; }
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
    if (map && ctx.eax && mem::Spans(ctx.eax, 4)) {
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
    game_coop::AsDouble dbl(ctx.ecx);
    game_coop::AsSecond as(ctx.ecx);
    game_coop::GlobalsWatch watch(0x25480, ctx.ecx == game_coop::g_second ? 2 : 1, game_coop::g_watch_globals);
    if (int which = game_coop::DoubleOf(ctx.ecx); which >= 0 && !game_coop::g_doubles[which].out) {  // a stand-in body, put away: it does nothing
        game_coop::g_running = before;
        kernel::Return(ctx, 0, 0);
        return;
    } else if (which >= 0) {
        // A stand-in has no Detect, so nothing tells it whether it stands on anything: it is on the ground when its
        // owner is (Player +0xA8 bit 0), and in the air with him. Its own states then follow his jump: Jump or Fall
        // while he is up, Landing when he comes down.
        // It lands when he lands: the game's own landing (0x3BB00, what a Player's Detect calls when he meets the
        // ground: it ends the fall, sets the bit and begins the Landing state) the moment its owner is on the ground
        // again while it is still in the air (its states Jump, bit 8, or Fall, bit 9). A third-person jump lasts
        // longer than the hop in first person: without this it stayed in the air's pose after his first jump.
        uint32_t owner = which ? game_coop::g_second : game_coop::g_player;
        static const bool no_ground = std::getenv("S2_DOUBLE_NO_GROUND") != nullptr;  // tests: as before
        if (owner && !no_ground) {
            uint32_t player = ctx.ecx;
            bool his = (kernel::R32(owner + 0xA8) & 1) != 0, in_air = (kernel::R32(player + 0xBC) & 0x300) != 0;
            static bool was[2] = {true, true};
            if (his && in_air && (!was[which] || !(kernel::R32(player + 0xA8) & 1))) {
                uint32_t eax = ctx.eax;
                kernel::CallGuest(ctx, 0x3BB00, {});  // (this = the stand-in, still in ecx; "the player" is it: AsDouble above)
                ctx.ecx = player, ctx.eax = eax;
            }
            was[which] = his;
            kernel::W32(player + 0xA8, (kernel::R32(player + 0xA8) & ~1u) | (his ? 1u : 0u));
        }
        game_coop::MirrorStates(ctx, which);
    }
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
    game_coop::AsDouble dbl(ctx.ecx);
    game_coop::AsSecond as(ctx.ecx);
    game_coop::GlobalsWatch watch(0x24B00, ctx.ecx == game_coop::g_second ? 2 : 1, game_coop::g_watch_globals);
    if (game_coop::DoubleOf(ctx.ecx) >= 0) {  // a stand-in body has no Detect (see "the stand-in bodies")
        game_coop::g_running = before;
        kernel::Return(ctx, 0, 0);
        return;
    }
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
// The game asks a controller about an action through two routines of the input system ([0x384BC8]): 0x1A09C0 (the
// Player's input user part, the action's number, how: "was it pressed") and 0x1A0790 (the same, for a value). The
// controller is the one the user part names (+4: 0 the first, 1 the second). A stand-in body's names one that is not
// there, so every answer is "no"; for the actions it does with its owner (DoubleAction) the question is put to its
// owner's controller instead.
#define COOP_ASKED(addr)                                                                                                      X86_EXTERN_FUNC(__imp__sub_##addr);                                                                                       X86_FUNC(sub_##addr) {                                                                                                        game_coop::Asked(ctx, 0x##addr);                                                                                          uint32_t user = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8), how = kernel::R32(ctx.esp + 12);             int owner = game_coop::DoubleAction(user, action);                                                                        if (owner >= 0) kernel::W32(user + 4, uint32_t(owner));                                                                   __imp__sub_##addr(ctx, base);                                                                                             if (owner >= 0) kernel::W32(user + 4, 3);                                                                                 game_coop::Answered(ctx, 0x##addr, user, action, how);                                                                }
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
// The Player's touch method hands most of what he can use to 0x2DA30 first (this = Player; the usable's engine
// object), which puts it on the list of what he is looking at (+0x532, count +0x53A, ten bytes an entry: the prompt
// on the screen) when he stands and looks right for it. The list of what he can use is another: 0x1ABC0, below.
// S2_USE_TRACE=1: each candidate and whether he is looking at it (when either changes).
X86_EXTERN_FUNC(__imp__sub_0002DA30);
X86_FUNC(sub_0002DA30) {
    uint32_t player = ctx.ecx, object = kernel::R32(ctx.esp + 4);
    uint32_t count = player && mem::Spans(player, 0x540) ? kernel::R32(player + 0x53A) : 0;
    __imp__sub_0002DA30(ctx, base);
    static const bool trace = std::getenv("S2_USE_TRACE") != nullptr;
    if (trace && object && mem::Spans(object, 0x110) && (player == game_coop::g_player || player == game_coop::g_second)) {
        static std::map<uint64_t, int> seen;
        uint32_t after = kernel::R32(player + 0x53A);
        uint64_t key = (uint64_t(object) << 2) | (player == game_coop::g_second ? 2 : 0) | (after > count ? 1 : 0);
        if (seen.size() < 200 && !seen[key]++)
            LOGI("use trace: Player %d is offered an object of class %08X (kind %u) at %.1f %.1f %.1f: %s", player == game_coop::g_second ? 2 : 1,
                 game_coop::ClassOf(object), kernel::R32(object + 0xE0), game_coop::ReadF(object + game_coop::kPosition),
                 game_coop::ReadF(object + game_coop::kPosition + 4), game_coop::ReadF(object + game_coop::kPosition + 8),
                 after > count ? "he is looking at it" : "he is not looking at it");
    }
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
    // S2_RT_TRACE=<frame>: every other texture the renderer is pointed at in that frame and the next (shadows),
    // with who asked and whether Player 2's view is being drawn
    static const uint64_t trace_at = std::getenv("S2_RT_TRACE") ? uint64_t(std::atoll(std::getenv("S2_RT_TRACE"))) : 0;
    if (trace_at && game_coop::Frames() >= trace_at && game_coop::Frames() < trace_at + 2)
        LOGI("coop: rt trace: frame %llu texture %08X from %08X (stack %08X %08X %08X %08X) second view %d",
             (unsigned long long)game_coop::Frames(), kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp), kernel::R32(ctx.esp + 8),
             kernel::R32(ctx.esp + 12), kernel::R32(ctx.esp + 16), kernel::R32(ctx.esp + 20), int(game_coop::g_screen_pass));
    __imp__sub_001FB080(ctx, base);
}

// ... and back at the screen (this = renderer). After Player 2's view there is nothing to go back from. (A texture
// the renderer was pointed at inside his view, a shadow's, is still its own to leave: renderer +0xE0 is the target
// to go back to.)
X86_EXTERN_FUNC(__imp__sub_001FB290);
X86_FUNC(sub_001FB290) {
    if (game_coop::g_screen_pass && !kernel::R32(ctx.ecx + 0xE0)) {
        game_coop::g_screen_pass = false;
        // (S2_DRAW_TRACE: where in the frame's draws Player 2's pass ends)
        if (gpu::g_trace_2d.load() > 0) LOGI("draw trace: -- Player 2's pass ends here (the renderer goes back to the screen)");
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_001FB290(ctx, base);
}

// The camera loop (0x1B83E0: the view's cameras from the last to the first, the screen).
X86_EXTERN_FUNC(__imp__sub_001B83E0);
X86_FUNC(sub_001B83E0) {
    using namespace game_coop;
    // The stand-in bodies that are out, exactly where their owners are now. Once for each time Doubles has looked
    // them over: a frame drawn while a level is taken down or loaded must not touch what may be gone.
    if (game_coop::g_doubles_fresh) {
        game_coop::g_doubles_fresh = false;
        PinDoubles(ctx);
    }
    __imp__sub_001B83E0(ctx, base);
}

// A view camera ends (this = view camera): it releases its texture. Player 2's has none to release.
X86_EXTERN_FUNC(__imp__sub_001BB8C0);
X86_FUNC(sub_001BB8C0) {
    if (game_coop::g_no_texture && kernel::R32(ctx.ecx + game_coop::kVcTexture) == game_coop::g_no_texture) {
        kernel::W32(ctx.ecx + game_coop::kVcTexture, 0);
        LOGI("coop: Player 2's view camera %08X ends", ctx.ecx);
        game_view::SetSplit(false);
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
    return g_second && ViewCamera2() && !g_one_view && timer && !(kernel::R32(timer + kTimerFlags) & kTimeStopped);
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

bool g_flash_inside = false;      // a screen flash is being drawn into a half by the hook of 0x1131F0
}  // namespace game_coop

// The screen flashes, each player's own. A screen flash is a picture or a colour across the whole view (this =
// 0x391C70; 0x1134C0 walks the list and drops the entries whose owner is gone, 0x1131F0 draws one entry and changes
// nothing). The object that owns an entry gives it its colour and strength every frame (0x113580: owner, red,
// green, blue, strength, order, texture; 0x113480 takes an owner's away). Most belong to the game as a whole (a
// Screen Fader object: a level's fade from black, the red picture after an innocent is killed). One belongs to each
// Player: the flash when he is hurt (his method 0x43E80, from his once-a-frame method at 0x2FFA9; colours from his
// properties 0x16B and on, times from 0x170 and 0x174). The game draws them all across the whole frame, so with
// two players the one's hurt flash lay over the other's view as well.
//   - Two views side by side: a Player's own flash goes into his half only (the runtime puts the whole-picture
//     draw onto the half: gpu::g_flat2d.half with cover). The game's own stay across the whole frame.
//   S2_COOP_FLASH_ALL=1: as the game has it (tests)
namespace game_coop {
// 1 or 2: the entry is that Player's own flash; 0: the game's
int FlashOwner(uint32_t manager, uint32_t index) {
    uint32_t list = kernel::R32(manager + 6);
    if (!list || index >= kernel::R32(manager + 0xE) || !mem::IsCommitted(list + index * 0x16 + 0x16)) return 0;
    uint32_t h = kernel::R16(list + index * 0x16 + 0x14), table = kernel::R32(kObjectManager + 0x0C);
    uint32_t owner = table && h ? kernel::R32(table + h * 6) : 0;
    if (!owner) return 0;
    return owner == g_player_object ? 1 : owner == g_second_object ? 2 : 0;
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_001131F0);
X86_FUNC(sub_001131F0) {
    using namespace game_coop;
    static const bool all = std::getenv("S2_COOP_FLASH_ALL") != nullptr;
    int whose = all || g_flash_inside || !g_second || !g_second_object || !ViewCamera2() || g_one_view ? 0 : FlashOwner(ctx.ecx, kernel::R32(ctx.esp + 4));
    if (!whose) {
        __imp__sub_001131F0(ctx, base);
        return;
    }
    static bool told[3] = {};
    if (!told[whose]) {
        told[whose] = true;
        LOGI("coop: Player %d's own screen flash (when he is hurt) is drawn for him only, in his half of the screen", whose);
    }
    if (!TwoLayers()) {  // a menu has the screen: as the game has it
        __imp__sub_001131F0(ctx, base);
        return;
    }
    gpu::g_flat2d.half = whose;
    gpu::g_flat2d.cover = true;
    g_flash_inside = true;
    __imp__sub_001131F0(ctx, base);
    g_flash_inside = false;
    gpu::g_flat2d.cover = false;
    gpu::g_flat2d.half = 0;
}

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
        // In a scene (one view, Player 1 held by the story) the game leaves its crosshair out because "the player"
        // cannot aim; with Player 2 in the list of players it drew one (the owner, 2026-11-28: "crosshairs in
        // cutscenes"). For the 2D of a scene the list holds Player 1 alone.
        uint32_t list_count = game_coop::kObjectManager + 0x12C, count = 0;
        bool alone = game_coop::g_one_view && game_coop::g_second_object && !game_coop::g_layer_inside;
        if (alone) {
            count = kernel::R32(list_count);
            uint32_t list = kernel::R32(game_coop::kObjectManager + 0x124);
            if (count >= 2 && list && kernel::R32(list) == game_coop::g_player_object) kernel::W32(list_count, 1);
            else alone = false;
        }
        __imp__sub_0016C980(ctx, base);
        if (alone) kernel::W32(list_count, count);
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
// he sets off no trigger and picks nothing up (the rule of 2026-11-23: only Player 1 opens doors, uses and picks up,
// sets off triggers and scenes). Since 2026-11-26 he climbs ladders, vaults ledges and pushes boxes: see "what Player
// 2 can use", below. Since 2026-11-27 he picks up ammunition ("player 2 couldnt pick up ammo", the owner): the guns
// that lie about or that enemies drop (Ranged Weapon: picking one up gives its rounds, a shotgun 16 shells of item
// 3587, class Missile Item, and the gun when it is new to him), the rounds and grenades that lie about (Missile
// Item), and the boxes things are kept in (Ammo Box: guns, rounds, a flashlight). Player 1 is given whatever Player 2
// picks up (pickups, below), as Player 2 is given what Player 1 picks up: nothing is Player 2's alone. (A solid thing also tells its touch a second way, 0x1706F0, which is not filtered: that is how
// he has opened doors and climbed ladders all along.)
namespace game_coop {
constexpr uint32_t kTemplates = 0x384BA4;  // the template table: chunks of 1024 at +0, entry size +0x14, count +0x18
uint32_t ClassOf(uint32_t object) {
    if (!object || !mem::Spans(object, kTemplateWord) || !mem::Spans(object, kTemplateWord + 3)) return 0;
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
        // things to pick up (Ammo Box 0x202383E5, Ranged Weapon 0x9710D816 and Missile Item 0xFE9F718D were here until
        // 2026-11-27: Player 2 picks up ammunition)
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
        // things to use
        // (Ladder 0x671882A1, Clamber Point 0xEC32498A = a ledge to vault, and Pushable 0x5FEBD8E9 were here until
        // 2026-11-26: see "what Player 2 can use", below)
        case 0x3F4C759C:  // Switch
        case 0xD8E872CE:  // Security Monitor
        case 0xE4002F8D:  // Phone
        case 0x22B79066:  // Toilet
        case 0x69A9942B:  // Mountable
        case 0xC0DCDE7E:  // MountedWeapon
            return true;
    }
    return false;
}

// true: this touch is not delivered
bool RefuseTouch(uint32_t touched, uint32_t toucher) {
    if (DoubleObject(touched) >= 0 || DoubleObject(toucher) >= 0) return true;  // a stand-in body touches nothing
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
    {   // tests: S2_TOUCH_TRACE=1 logs where a touch between the two players comes from (three times)
        static const bool trace = std::getenv("S2_TOUCH_TRACE") != nullptr;
        static int said = 0;
        uint32_t a = kernel::R32(ctx.esp + 8), b = kernel::R32(ctx.esp + 12);
        bool both = game_coop::g_second_object && ((a == game_coop::g_player_object && b == game_coop::g_second_object) ||
                                                  (b == game_coop::g_player_object && a == game_coop::g_second_object));
        if (trace && both && said < 3) said++, LOGR("touch trace: the two players touch, called from %s", game_coop::CallersOf(ctx).c_str());
    }
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
    if (game_coop::g_doubles[0].object || game_coop::g_doubles[1].object) {
        // A stand-in body touches nothing, this way neither: nothing is told that it (or an object that moves it)
        // touched it, and it is told of nothing.
        uint32_t table = kernel::R32(game_coop::kObjectManager + 0x0C), handle = kernel::R32(ctx.ecx + 0x0C) & 0xFFFF;
        uint32_t touched = table && handle ? kernel::R32(table + handle * 6) : 0, contact = kernel::R32(ctx.esp + 4);
        uint32_t toucher = contact && mem::Spans(contact, 0x34) ? kernel::R32(contact + 0x30) : 0;
        bool refuse = game_coop::DoubleObject(touched) >= 0 || game_coop::DoubleObject(toucher) >= 0;
        for (const game_coop::Double& d : game_coop::g_doubles)
            if (uint32_t proxy = d.player ? game_coop::ProxyOf(d.player) : 0; proxy && toucher == proxy) refuse = true;
        if (refuse) {
            kernel::Return(ctx, 0, 1);
            return;
        }
    }
    __imp__sub_001706F0(ctx, base);
}

// ---- pickups: what Player 1 gets in the way of weapons, ammunition and health, Player 2 gets too ---------------------
// 0x153A0 (this = Player, item's template, how many) gives a Player an item: the inventory entry (0x11540), and for
// a weapon he has no better use for, the weapon in his hands. Every pickup comes through it (13 callers). When it
// is Player 1 who gets a combat item, Player 2 is given the same (the rule of 2026-11-23); keys, notes, maps and
// archive pages stay Player 1's. When it is Player 2 who picks something up (ammunition, since 2026-11-27: see "Player
// 1 only" above), Player 1 is given the same, whatever it is (a box can hold more than rounds, and nothing may be
// lost to Player 1, whose game is the one that is saved): whoever gets to it first, both have it.
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
        case 0xCF3A3FA5:  // Flashlight (each player has his own: "the flashlight", below)
            return true;
    }
    return false;
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_000153A0);
X86_FUNC(sub_000153A0) {
    uint32_t player = ctx.ecx, item = kernel::R32(ctx.esp + 4), count = kernel::R32(ctx.esp + 8);
    // (the co-op code's own gives, the copies of Player 1's things for Player 2, come through here too: a call from the
    // runtime returns to 0xFFFFFFF0, cpu/dispatch.cpp CallGuest; the game's own return into its code)
    bool ours = kernel::R32(ctx.esp) == 0xFFFFFFF0u;
    __imp__sub_000153A0(ctx, base);
    static bool inside = false;
    if (!inside && !ours && game_coop::g_second && game_coop::g_player && player == game_coop::g_second) {
        uint32_t cls = game_coop::ClassOfTemplate(item);
        inside = true;
        uint32_t ecx = ctx.ecx, eax = ctx.eax;
        ctx.ecx = game_coop::g_player;
        kernel::CallGuest(ctx, 0x153A0, {item, count});
        ctx.ecx = ecx;
        ctx.eax = eax;
        inside = false;
        LOGI("coop: Player 2 picks up item %u (class %08X) x%d; Player 1 is given the same", item, cls, int32_t(count));
        return;
    }
    if (inside || !game_coop::g_second || player != game_coop::g_player) return;
    uint32_t cls = game_coop::ClassOfTemplate(item);
    bool both = game_coop::ForBoth(cls);
    if (both) {
        inside = true;
        game_coop::AsSecond as(game_coop::g_second);
        uint32_t ecx = ctx.ecx, eax = ctx.eax;
        // (the hint Player 1's pickup has just shown is not shown a second time for Player 2's copy: Arm, +0xB8)
        kernel::W32(game_coop::g_second + 0xB8, kernel::R32(game_coop::g_second + 0xB8) | kernel::R32(player + 0xB8));
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
    if (!g_player_object || !victim || !mem::Spans(victim, 0x20)) return;
    uint32_t object = kernel::R32(victim + 4);
    if (by && !mem::Spans(by, 0x110)) by = 0;
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
    if (game_coop::DoubleOf(ctx.ecx) >= 0) {
        // a stand-in body: nothing should reach it (it is out of the physics' questions); if something does, it is
        // not hurt, and the record keeps the first times
        uint32_t by = kernel::R32(ctx.esp + 8);
        if (game_coop::g_double_hits++ < 12)
            LOGR("coop: a stand-in body was hit (%u) by %s: not hurt (the hit came through %s)", game_coop::g_double_hits,
                 by && mem::Spans(by, 0x110) ? game_coop::WhoIs(by).c_str() : "nothing named", game_coop::Callers(ctx, 12).c_str());
        float none = 0;
        std::memcpy(kernel::HostPtr(ctx.esp + 4), &none, 4);
    }
    uint32_t before = game_coop::g_hit_by;
    game_coop::g_hit_by = kernel::R32(ctx.esp + 8);
    // tests: S2_COOP_HURT_TRACE=1 logs where the first hurts of a Player by a person or a creature come from
    static const bool trace = std::getenv("S2_COOP_HURT_TRACE") != nullptr;
    static int said = 0;
    if (trace && said < 8 && game_coop::g_hit_by && mem::Spans(game_coop::g_hit_by, 0x110)) {
        uint32_t cls = game_coop::ClassOf(game_coop::g_hit_by);
        if (cls == 0x0E15C87E || cls == 0xC3F8C321 || cls == 0x4E28364F || cls == 0x127243E5 || cls == 0x285A9A1F) {
            said++;
            LOGI("coop trace: a Player (%08X) is hurt by %08X (class %08X), called from %s", ctx.ecx, game_coop::g_hit_by, cls,
                 game_coop::CallersOf(ctx).c_str());
        }
    }
    // Co-op rules (decided 2026-11-24): the two players do not hurt each other. What is not a Player hurts them as
    // it does in the game alone (until 2026-11-27 it was 30% more while both were in; the owner then: "lets bring
    // that back down to 0 and just have the game be regular difficulty").
    if (game_coop::g_second_object && mem::Spans(ctx.esp, 8)) {
        uint32_t by = game_coop::g_hit_by;
        bool partner = by && (by == game_coop::g_player_object || by == game_coop::g_second_object) &&
                       mem::Spans(ctx.ecx, 8) && kernel::R32(ctx.ecx + 4) != by;
        if (partner || game_coop::DoublesThing(by)) {  // (nor does a stand-in body hurt anybody: its blows are for show)
            float none = 0;
            std::memcpy(kernel::HostPtr(ctx.esp + 4), &none, 4);
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
        uint32_t object = mem::Spans(ctx.ecx, 8) ? kernel::R32(ctx.ecx + 4) : 0;
        if (trace && said < 12 && object && object != game_coop::g_player_object && object != game_coop::g_second_object) {
            said++;
            LOGI("coop trace: %s (class object %08X) is hurt %.1f by %s (%08X), called from %s", game_coop::WhoIs(object).c_str(), ctx.ecx, amount,
                 by && mem::Spans(by, 0x110) ? game_coop::WhoIs(by).c_str() : "nothing named", by, game_coop::CallersOf(ctx).c_str());
        }
    }
    // (Enemies had 30% more health while both players were in, until 2026-11-27: the game's own difficulty since.)
    {
        // nothing is hurt by a stand-in body or by what it holds
        if (amount > 0 && game_coop::DoublesThing(by ? by : game_coop::g_hit_by)) {
            amount = 0;
            std::memcpy(kernel::HostPtr(ctx.esp + 4), &amount, 4);
            static int said = 0;
            if (said++ < 4) LOGI("coop: a stand-in body's blow or shot reached something: it does no harm");
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
    if (!g_second_object || !hit || !mem::Spans(hit, 0x28)) return false;
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
    // (S2_COOP_ONEHIT=all: whoever hits it)
    static const char* onehit_v = std::getenv("S2_COOP_ONEHIT");
    static const bool onehit = onehit_v != nullptr, onehit_all = onehit_v && !std::strcmp(onehit_v, "all");
    if (onehit && (second || onehit_all) && mem::Spans(ctx.ecx, 0x20) && game_coop::ReadF(ctx.ecx + 0x16) > 0.5f) {
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
    bool known = who && game_coop::g_player && mem::Spans(player, 0x490) && mem::Spans(game_coop::g_player, 0x490);
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

// ---- what Player 2 can use ---------------------------------------------------------------------------------------
// (2026-11-26, after the online session of that day: "Player 2 was unable to vault", and a box he pushed "was
// bugging out".)
// What a Player can use is a list of his own (+0x342, six bytes an entry: the object's handle and the touch's word;
// count +0x34A). An object puts itself there when it touches him: his touch method 0x1B0D0 calls 0x1ABC0 (this =
// Player; the object, the word) every frame it touches. His once-a-frame method then goes through the list (0x41BA0,
// called at 0x25524): with the jump button it asks the ledges (class Clamber Point: the game's "clamber" is the
// vault), with the use button everything else. Each is asked through its method +0xAC whether he stands and faces
// right for it, and the nearest that says yes is used. +0x33E is the handle of what he is using, +0x340 of what he
// could use now.
//   - A ledge's touch was refused to Player 2 (NotForSecond), so no ledge was ever on his list: he could not vault.
//     Ladders and boxes were refused the same way, but they are solid and tell their touch the second way too: so he
//     climbed ladders and took hold of boxes all the same.
//   - The usable things ask for "the player", the first in the list, in sixteen places (work/re/player_owners.tsv):
//     0xD7900 "can he use it from where he stands" when no user is named, a ledge's place along its edge 0xD8B70 and
//     its height check 0xD8AD0, the scale of the climb 0xD93D0, the start and end of a use 0xD7EE0 (which writes the
//     first Player down as the user) and 0xD92A0, and a box's own once-a-frame method 0xE3440, which moves the box
//     with the hands of "the player's" skeleton. Inside Player 2's once-a-frame method he is the first (AsSecond).
//     A box's own method runs outside it: there the box went by Player 1's hands while Player 2 pushed it. These
//     run with Player 2 as the first whenever he is the one on the object and no Player's own method is running.
//   - A box has one user: while one player has hold of it, it is not put on the other's list.
//   - What stays Player 1's, although its touch can reach Player 2 the second way: the security monitors (decided
//     2026-11-25), mounted weapons (the truck's gun comes later), phones, toilets and seats (they start scenes).
namespace game_coop {
uint32_t Named(uint32_t player, uint32_t at) {
    if (!player || !mem::Spans(player, 0x540)) return 0;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = kernel::R16(player + at);
    uint32_t object = table && h && h < kernel::R32(kObjectManager + 0x14) ? kernel::R32(table + h * 6) : 0;  // (as many entries as +0x14 says)
    // (both ends: the owner's version 67 crashed when a handle named 0000FF10, whose +0x110 lies in the image at
    // 0x10000 while its +0xE4, read for its class, does not)
    return object && Whole(object, 0x114) ? object : 0;
}
const char* KindName(uint32_t cls) {
    switch (cls) {
        case 0x671882A1: return "a ladder";
        case 0xEC32498A: return "a ledge (a vault)";
        case 0x5FEBD8E9: return "a box to push";
        case 0x3F4C759C: return "a switch or a door";
        case 0x69A9942B: return "a seat or a mount";
        case 0xC0DCDE7E: return "a mounted weapon";
        case 0xD8E872CE: return "a security monitor";
        case 0xE4002F8D: return "a phone";
        case 0x22B79066: return "a toilet";
        case 0x0E15C87E: return "a person or a creature";
        case 0xC3F8C321: return "a person";
    }
    return nullptr;
}
bool FirstOnly(uint32_t cls) {
    switch (cls) {
        case 0xD8E872CE:  // Security Monitor
        case 0xE4002F8D:  // Phone
        case 0x22B79066:  // Toilet
        case 0x69A9942B:  // Mountable
        case 0xC0DCDE7E:  // MountedWeapon
            return true;
    }
    return false;
}
// Player 2 is the one on this object: he is using it, or it is what he could use now and Player 1 is not on it
bool SecondOn(uint32_t object) {
    if (!object || !g_second || !g_second_object) return false;
    if (Named(g_second, 0x33E) == object) return true;
    if (g_player && (Named(g_player, 0x33E) == object || Named(g_player, 0x340) == object)) return false;
    return Named(g_second, 0x340) == object;
}
// ... for a usable thing's method (this = its class object, the engine object at +4), outside the Players' own
bool ForSecond(uint32_t self) {
    static const bool off = std::getenv("S2_COOP_NO_USE_SWAP") != nullptr;  // tests: as before 2026-11-26
    if (off || g_running || !g_second_object || !self || !mem::Spans(self, 8)) return false;
    return SecondOn(kernel::R32(self + 4));
}
// the record: each thing a player can use, once for each player and level (and each that is kept off his list)
void UseRecord(int who, uint32_t object, uint32_t cls, const char* refused) {
    static std::map<uint64_t, int> seen;
    static uint32_t level = 0;
    if (level != g_player) level = g_player, seen.clear();
    uint64_t key = (uint64_t(object) << 3) | (uint64_t(who) << 1) | (refused ? 1 : 0);
    if (seen.size() >= 800 || seen[key]++) return;
    const char* name = KindName(cls);
    char what[48];
    if (!name) std::snprintf(what, sizeof what, "an object of class %08X", cls), name = what;
    if (refused) LOGI("use: %s at %.1f %.1f %.1f touches Player %d and is kept off his list: %s", name, ReadF(object + kPosition), ReadF(object + kPosition + 4),
                      ReadF(object + kPosition + 8), who, refused);
    else LOGI("use: Player %d can use %s at %.1f %.1f %.1f (it touches him)", who, name, ReadF(object + kPosition), ReadF(object + kPosition + 4),
              ReadF(object + kPosition + 8));
}
// S2_USE_TRACE=1: a usable thing's answer when a player presses the button for it
bool g_use_quiet = false;  // a test is asking by the thousand (TestSpot)
void UseAnswer(X86Context& ctx, uint32_t self, uint32_t user, uint32_t answer) {
    static const bool trace = std::getenv("S2_USE_TRACE") != nullptr;
    static int said = 0;
    if (!trace || g_use_quiet || said > 400 || !self || !mem::Spans(self, 8)) return;
    uint32_t object = kernel::R32(self + 4);
    if (!object || !mem::Spans(object, 0x110)) return;
    said++;
    const char* name = KindName(ClassOf(object));
    LOGI("use trace: %s at %.1f %.1f %.1f is asked by Player %d (at %.2f %.2f %.2f): answer %u (3 is no); the first in the list is Player %d", name ? name : "something",
         ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8), user == g_second_object ? 2 : user == g_player_object ? 1 : 0,
         user && mem::Spans(user, 0x60) ? ReadF(user + kPosition) : 0.f, user && mem::Spans(user, 0x60) ? ReadF(user + kPosition + 4) : 0.f,
         user && mem::Spans(user, 0x60) ? ReadF(user + kPosition + 8) : 0.f, answer,
         kernel::R32(kernel::R32(kObjectManager + kPlayerList)) == g_second_object ? 2 : 1);
    (void)ctx;
}
}  // namespace game_coop

// The game's events (0x1A1E40: this = the sender at [0x384B9C], the event's number; numbers 0x186A0 to 0x186C5, sent
// from the Player's code among others: 0x186A7 "a weapon is in his hand" at 0x14078, on which the HUD takes its weapon
// parts away and slides them in again with the weapon's name; others set off the hints on the screen). The stand-in
// body sends none: while it is "the player" (AsDouble: its own Run, being handed its weapon, taking it in hand) the
// event is dropped. Why: with the stand-in taking its owner's weapon in hand, "Colt 1911" slid across Player 1's
// screen for two seconds each time a player went into first person (runs dbl24 to dbl49).
X86_EXTERN_FUNC(__imp__sub_001A1E40);
X86_FUNC(sub_001A1E40) {
    static const bool keep = std::getenv("S2_COOP_DOUBLE_EVENTS") != nullptr;  // tests: its events go out
    if (game_coop::g_double_scope > 0 && !keep) {
        static int said = 0;
        if (said < 8) said++, LOGI("coop: an event of the stand-in body's (%X) is not sent", kernel::R32(ctx.esp + 4));
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_001A1E40(ctx, base);
}
// A weapon comes into a Player's hand (0x13A00, this = Player; the weapon's template) and into his other hand
// (0x18020, this = Player). The game calls them from the Player's Run and also from outside it (the moment in the
// drawing movement at which the weapon is taken, reached through 0x1A0600), and they send 0x186A7 at their end. For
// the stand-in body they run with it as "the player", like its own Run, so that the event is dropped.
X86_EXTERN_FUNC(__imp__sub_00013A00);
X86_FUNC(sub_00013A00) {
    game_coop::AsDouble dbl(ctx.ecx);
    __imp__sub_00013A00(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00018020);
X86_FUNC(sub_00018020) {
    game_coop::AsDouble dbl(ctx.ecx);
    __imp__sub_00018020(ctx, base);
}
// The weapon a Player takes in hand brings three HUD parts with it (0x1DE10, this = Player: made from the weapon's
// properties 2 to 4, kept at Player +0x7D4, +0x7D8 and +0x7DC, added to the HUD, [0x391D64] method +8), and the
// Player's Run shows them every frame (0x256F6: the ammunition counter among them). None for the stand-in body: the
// function does nothing for it, nor while it is being made (a Player's code passes over parts that are not there,
// 0x25702).
X86_EXTERN_FUNC(__imp__sub_0001DE10);
X86_FUNC(sub_0001DE10) {
    if (game_coop::g_double_spawning || game_coop::DoubleOf(ctx.ecx) >= 0) {
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_0001DE10(ctx, base);
}

// an object that touches a Player goes on his list of things to use (this = Player; the object, the touch's word)
X86_EXTERN_FUNC(__imp__sub_0001ABC0);
X86_FUNC(sub_0001ABC0) {
    using namespace game_coop;
    uint32_t player = ctx.ecx, object = kernel::R32(ctx.esp + 4);
    int who = player && player == g_player ? 1 : g_second && player == g_second ? 2 : 0;
    if (who && object && mem::Spans(object, 0x110)) {
        uint32_t cls = ClassOf(object);
        const char* refused = nullptr;
        if (who == 2 && FirstOnly(cls)) refused = "it is for Player 1 only";
        else if (cls == 0x5FEBD8E9 && g_second && Named(who == 2 ? g_player : g_second, 0x33E) == object) refused = "the other player has hold of it";
        UseRecord(who, object, cls, refused);
        if (refused) {
            kernel::Return(ctx, 0, 2);
            return;
        }
    }
    __imp__sub_0001ABC0(ctx, base);
}
// the usable things' methods that work on "the player" (this = the thing's class object)
#define COOP_USABLE(addr)                                \
    X86_EXTERN_FUNC(__imp__sub_##addr);                  \
    X86_FUNC(sub_##addr) {                               \
        game_coop::ListAsSecond as(game_coop::ForSecond(ctx.ecx)); \
        __imp__sub_##addr(ctx, base);                    \
    }
COOP_USABLE(000E3440)  // a box's once-a-frame method: it goes by the hands of the player's skeleton
COOP_USABLE(000D93D0)  // a ledge: the scale of the climb, written into the player's skeleton
COOP_USABLE(000D92A0)  // a ledge: the use ends, the scale goes back to 1
COOP_USABLE(000D7EE0)  // any of them: the use begins (the player is written down as the user) or ends
COOP_USABLE(000D8AD0)  // a ledge: is the player at a height to take it
COOP_USABLE(000D8B70)  // a ledge: the place along its edge nearest the player
COOP_USABLE(000D9300)  // a ledge: which way of climbing fits the player
COOP_USABLE(000D77B0)  // any of them: hands itself to the player
// "can he use it from where he stands" (the object, its matrix by value, the way of using it, the user: 0 = "the
// player")
X86_EXTERN_FUNC(__imp__sub_000D7900);
X86_FUNC(sub_000D7900) {
    uint32_t object = kernel::R32(ctx.esp + 4), user = kernel::R32(ctx.esp + 0x4C);
    static const bool off = std::getenv("S2_COOP_NO_USE_SWAP") != nullptr;
    game_coop::ListAsSecond as(!off && !user && !game_coop::g_running && game_coop::g_second_object && game_coop::SecondOn(object));
    __imp__sub_000D7900(ctx, base);
}
// the answers of a ledge, a box and a ladder to "can this one use you" (this; the user's object, a flag): traced
#define COOP_CANUSE(addr)                                \
    X86_EXTERN_FUNC(__imp__sub_##addr);                  \
    X86_FUNC(sub_##addr) {                               \
        uint32_t self = ctx.ecx, user = kernel::R32(ctx.esp + 4); \
        __imp__sub_##addr(ctx, base);                    \
        game_coop::UseAnswer(ctx, self, user, ctx.eax);  \
    }
COOP_CANUSE(000D9870)
COOP_CANUSE(000E3D60)
COOP_CANUSE(000DB1D0)

// Tests: S2_COOP_EXIT=<frame> sets off the level's exit at that frame without walking to it: the first Level
// Transition object that is loaded gets its own "go" (0x160B70, this = the Level Transition; it stops Player 1, puts
// up the loading screen and leaves itself at 0x392138, from where the game's frame calls 0x160D20).
namespace game_coop {
void TestExit(X86Context& ctx) {
    static const char* v = std::getenv("S2_COOP_EXIT");
    if (!v || !g_player_object || g_frames != uint64_t(std::atoll(v))) return;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), entries = std::min<uint32_t>(kernel::R32(kObjectManager + 0x14), 0x4000);
    int seen = 0;
    for (uint32_t i = 0; table && i < entries && mem::IsCommitted(table + i * 6 + 4); i++) {
        uint32_t object = kernel::R32(table + i * 6);
        // (a free entry holds a small number, the next free one, not an object)
        if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
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
        if (game_coop::g_second && mem::Spans(game_coop::g_second, 0x490) && game_coop::ReadF(game_coop::g_second + 0x1A) > 0) {
            game_coop::g_carry.valid = true;
            game_coop::g_carry.share = game_coop::ReadF(game_coop::g_second + 0x16) / game_coop::ReadF(game_coop::g_second + 0x1A);
            game_coop::g_carry.meter = game_coop::ReadF(game_coop::g_second + 0x48A);
            game_coop::g_carry.at = std::chrono::steady_clock::now();
        }
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
    if (!mem::Spans(behaviour, 0x20)) return false;
    uint32_t holder = kernel::R32(behaviour + 0x1C);
    if (holder < 0x10000 || !mem::Spans(holder, 0x14)) return false;
    uint32_t owner = kernel::R32(holder + 0x10);
    if (owner < 0x400000 || !mem::IsCommitted(owner) || !mem::Spans(owner, 0x110)) return false;
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
    uint32_t table = kernel::R32(kObjectManager + 0x0C), entries = std::min<uint32_t>(kernel::R32(kObjectManager + 0x14), 0x4000);
    for (uint32_t i = 0; table && i < entries && mem::IsCommitted(table + i * 6 + 4); i++) {
        uint32_t object = kernel::R32(table + i * 6);
        if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
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
    // S2_TEST_KIND=<class id in hex>: every object of that class too, with its flags (which ledges are switched on)
    if (const char* kind = std::getenv("S2_TEST_KIND")) {
        uint32_t want = uint32_t(std::strtoul(kind, nullptr, 16));
        int n = 0;
        EachObject([&](uint32_t object) {
            if (ClassOf(object) != want || n > 80) return;
            uint32_t self = kernel::R32(object + kClassObject);
            LOGR("census: %08X #%d at %.1f %.1f %.1f, object flags %08X, its own flags (+0x46) %08X", want, n++, ReadF(object + kPosition), ReadF(object + kPosition + 4),
                 ReadF(object + kPosition + 8), kernel::R32(object + 8), self && mem::Spans(self, 0x4C) ? kernel::R32(self + 0x46) : 0);
        });
    }
    for (const Kind& k : kKinds) {
        auto it = seen.find(k.cls);
        if (it == seen.end()) continue;
        uint32_t o = it->second.second;
        LOGR("census: %s x%d, the first at %.1f %.1f %.1f (Player 1 at %.1f %.1f %.1f)", k.name, it->second.first, ReadF(o + kPosition),
             ReadF(o + kPosition + 4), ReadF(o + kPosition + 8), ReadF(g_player_object + kPosition), ReadF(g_player_object + kPosition + 4),
             ReadF(g_player_object + kPosition + 8));
    }
}

// S2_TEST_PUT=<frame>,<who 1 or 2>,<x>,<y>,<z>,<facing in degrees>[;<frame>,<who>,...]: that player is put at that
// place facing that way (0 is +z, 90 is +x: the "facing" of the position lines), by the game's own teleport. Places
// taken from a session log (where a player stood when he used something) can be tried again that way.
void TestPut(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_PUT");
    if (!v || !g_player_object || !g_player) return;
    for (const char* c = v; c && *c; c = std::strpbrk(c, ";|") ? std::strpbrk(c, ";|") + 1 : nullptr) {  // ("|" too: tools\run.ps1 splits its -Vars at ";")
        unsigned long long frame = 0;
        int who = 1;
        float x = 0, y = 0, z = 0, facing = 0;
        if (std::sscanf(c, "%llu,%d,%f,%f,%f,%f", &frame, &who, &x, &y, &z, &facing) < 5 || g_frames != frame) continue;
        uint32_t player = who == 2 ? g_second : g_player, object = who == 2 ? g_second_object : g_player_object;
        if (!player || !object) {
            LOGR("put: there is no Player %d", who);
            continue;
        }
        float m[16] = {};
        float a = facing / 57.29578f, fx = std::sin(a), fz = std::cos(a);
        float old[16];
        std::memcpy(old, kernel::HostPtr(object + kMatrix), 64);
        // (the right vector keeps the hand of the matrix he has: as TestGoto does)
        float rx = old[5] * old[10] - old[6] * old[9], rz = old[4] * old[9] - old[5] * old[8];
        float sign = rx * old[0] + rz * old[2] < 0 ? -1.0f : 1.0f;
        m[8] = fx, m[9] = 0, m[10] = fz;
        m[0] = sign * fz, m[1] = 0, m[2] = -sign * fx;
        m[4] = 0, m[5] = 1, m[6] = 0;
        m[12] = x, m[13] = y, m[14] = z, m[15] = 1;
        m[3] = old[3], m[7] = old[7], m[11] = old[11];
        static uint32_t matrix = mem::AllocSystem(64);
        std::memcpy(kernel::HostPtr(matrix), m, 64);
        uint32_t ecx = ctx.ecx;
        ctx.ecx = player;
        kernel::CallGuest(ctx, kPlayerStop, {});
        ctx.ecx = player;
        kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
        ctx.ecx = ecx;
        LOGR("put: Player %d put at %.2f %.2f %.2f facing %.0f", who, x, y, z, facing);
    }
}

// S2_TEST_SPOT=<frame>,<class id in hex>,<which>[,<who 1 or 2>[,<floor>]]: finds a place from which that player can
// use the <which>-th loaded object of that class. From that frame on he is put at one place after another round the
// object (0.8, 1.2 and 1.6 m from it, every 30 degrees, facing it, at the object's height plus <floor> metres: a
// player put in the air stays there, so the test says where the floor is, 0.05 above it), left there for a fifth
// of a second, and the object is asked (its method +0xAC, as the Player's once-a-frame method asks when the button
// is pressed). The first place it says yes from is logged and he stays
// there: S2_TEST_PUT with that place repeats it in another run. (A ledge, a box or a ladder says yes only from the
// right spot facing the right way with nothing between his head and it, and a place read off a session log, one a
// second, is rarely it.)
void TestSpot(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_SPOT");
    if (!v || !g_player_object || !g_player) return;
    static unsigned long long frame = 0;
    static unsigned cls = 0;
    static int which = 0, who = 1, step = -1, parsed = 0;
    static float facing = 0, floor = 0;
    if (!parsed) parsed = std::sscanf(v, "%llu,%x,%d,%d,%f", &frame, &cls, &which, &who, &floor) >= 3 ? 1 : -1;
    if (parsed < 0 || g_frames < frame || step == -2 || (g_frames - frame) % 12) return;
    uint32_t player = who == 2 ? g_second : g_player, body = who == 2 ? g_second_object : g_player_object;
    uint32_t target = 0;
    int n = 0;
    EachObject([&](uint32_t object) {
        if (ClassOf(object) == cls && n++ == which) target = object;
    });
    uint32_t self = target ? kernel::R32(target + kClassObject) : 0;
    if (!player || !body || !self || !mem::Spans(self, 0x90)) {
        LOGR("spot: no Player %d, or no object %d of class %08X (%d loaded)", who, which, cls, n);
        step = -2;
        return;
    }
    float t[3] = {ReadF(target + kPosition), ReadF(target + kPosition + 4), ReadF(target + kPosition + 8)};
    uint32_t ecx = ctx.ecx;
    if (step >= 0) {  // where he was put half a second ago: can he use it from there?
        ListAsSecond as(who == 2);
        uint32_t method = kernel::R32(kernel::R32(self) + 0xAC);
        ctx.ecx = self;
        uint32_t plain = kernel::CallGuest(ctx, method, {body, 0});
        ctx.ecx = self;
        uint32_t looking = kernel::CallGuest(ctx, method, {body, 1});
        ctx.ecx = ecx;
        if (plain != 3 || looking != 3) {
            LOGR("spot: object %d of class %08X at %.1f %.1f %.1f says yes to Player %d (answer %u; %u when it is what he looks at) at %.2f %.2f %.2f facing %.0f", which,
                 cls, t[0], t[1], t[2], who, plain, looking, ReadF(body + kPosition), ReadF(body + kPosition + 4), ReadF(body + kPosition + 8), facing);
            step = -2;
            return;
        }
    }
    step++;
    static const float kFar[3] = {0.8f, 1.2f, 1.6f};
    if (step >= 36) {
        LOGR("spot: object %d of class %08X at %.1f %.1f %.1f said no to Player %d from all 36 places (its own flags +0x46: %08X)", which, cls, t[0], t[1], t[2], who,
             kernel::R32(self + 0x46));
        step = -2;
        return;
    }
    float r = kFar[step / 12], a = float(step % 12) * 30.0f / 57.29578f;
    float px = t[0] + r * std::sin(a), pz = t[2] + r * std::cos(a);
    // (he walks against his matrix's third axis: to face the object that axis points away from it)
    float fx = (px - t[0]) / r, fz = (pz - t[2]) / r;
    facing = std::atan2(fx, fz) * 57.29578f;
    float old[16], m[16] = {};
    std::memcpy(old, kernel::HostPtr(body + kMatrix), 64);
    float rx = old[5] * old[10] - old[6] * old[9], rz = old[4] * old[9] - old[5] * old[8];
    float sign = rx * old[0] + rz * old[2] < 0 ? -1.0f : 1.0f;
    m[8] = fx, m[9] = 0, m[10] = fz;
    m[0] = sign * fz, m[1] = 0, m[2] = -sign * fx;
    m[4] = 0, m[5] = 1, m[6] = 0;
    m[12] = px, m[13] = t[1] + floor, m[14] = pz, m[15] = 1;
    m[3] = old[3], m[7] = old[7], m[11] = old[11];
    static uint32_t matrix = mem::AllocSystem(64);
    std::memcpy(kernel::HostPtr(matrix), m, 64);
    ctx.ecx = player;
    kernel::CallGuest(ctx, kPlayerStop, {});
    ctx.ecx = player;
    kernel::CallGuest(ctx, kPlayerTeleport, {matrix});
    ctx.ecx = ecx;
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

// The story changes "the player's" form by itself at some moments (the end of the prison riot, before the dock: the
// meter is filled and he becomes the creature; later he is turned back). Three script functions do it, each for the
// first Player of the object manager's list alone:
//   0x100930  the insanity meter is filled (0x2A490 gives the most, 0x2ADD0 sets it)
//   0x100840  into the creature form, unless he is in it (0x2B230)
//   0x1008B0  out of it, when he is in it (0x2B230, then the meter to 0)
// Player 2 stayed human there, with enemies around that only the creature can hurt (the owner, 2026-11-26: "when player
// 2 played, he didn't transform when player 1 transform"). The same is now done for Player 2 right after.
// S2_TEST_STORY_FORM=<frame>: the fill and the change at that frame, the change back 600 frames later (tests).
namespace game_coop {
// While the story keeps Player 1 as the creature his meter does not run down, and Player 2's did: in the prison
// riot's last fight (suffering2_011.log) Player 2 was a man again after 21 s, was changed once more when the script
// repeated its step, and was a man again 6 s later, while Player 1 stayed the creature for 35 s. From the story's
// change until its change back, Player 2's meter is kept at Player 1's, and he is changed again if he is not the
// creature.
bool g_story_creature = false;
bool g_story_form_call = false;  // the story's change of form for Player 2 is being made (the hook of 0x25CD0 lets it through)
void StoryHold(X86Context& ctx) {
    if (!g_story_creature) return;
    if (!g_player || !mem::Spans(g_player, 0x490)) {
        g_story_creature = false;
        return;
    }
    if (!g_second || !mem::Spans(g_second, 0x490)) return;
    uint32_t one = kernel::R32(g_player + 0xBC), two = kernel::R32(g_second + 0xBC);
    if (!(one & 0x80000)) {  // Player 1 is on his way in (or out): nothing to hold yet
        if (!(one & 0x40000)) g_story_creature = false, LOGI("coop: the story's creature form is over for Player 1: Player 2's is no longer held");
        return;
    }
    float meter = ReadF(g_player + 0x48A);
    if (ReadF(g_second + 0x48A) < meter) std::memcpy(kernel::HostPtr(g_second + 0x48A), &meter, 4);
    static uint64_t tried = 0;
    if (!(two & 0x80000) && !(two & 0x40000) && g_frames - tried > 120) {
        tried = g_frames;
        uint32_t ecx = ctx.ecx;
        ctx.ecx = g_second;
        g_story_form_call = true, kernel::CallGuest(ctx, 0x2B230, {}), g_story_form_call = false;
        ctx.ecx = ecx;
        LOGI("coop: the story keeps the player as the creature: Player 2 is changed again");
    }
}
void StoryForm(X86Context& ctx, int what) {
    static const char* const kSteps[3] = {"fills the insanity meter", "turns the player into the creature", "turns the player back into a man"};
    LOGI("story: the script %s (Player 1's meter is now %.1f, his state %08X)", kSteps[what], g_player ? ReadF(g_player + 0x48A) : 0.0f,
         g_player ? kernel::R32(g_player + 0xBC) : 0);
    if (what == 1) g_story_creature = true;
    if (what == 2) g_story_creature = false;
    uint32_t p2 = g_second;
    if (!p2 || !mem::Spans(p2, 0x490)) return;
    bool creature = (kernel::R32(p2 + 0xBC) & 0x80000) != 0;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = p2;
    if (what == 0) {
        kernel::CallGuest(ctx, 0x2ADD0, {0x461C4000});  // (10000.0: the function keeps it to the most)
        LOGI("coop: the story fills the insanity meter: Player 2's too (%.1f)", ReadF(p2 + 0x48A));
    } else if (what == 1 && !creature) {
        g_story_form_call = true, kernel::CallGuest(ctx, 0x2B230, {}), g_story_form_call = false;
        LOGI("coop: the story turns the player into the creature: Player 2 too");
    } else if (what == 2 && creature) {
        g_story_form_call = true, kernel::CallGuest(ctx, 0x2B230, {}), g_story_form_call = false;
        ctx.ecx = p2;
        kernel::CallGuest(ctx, 0x2ADD0, {0});
        LOGI("coop: the story turns the player back: Player 2 too");
    }
    ctx.ecx = ecx;
}
void TestStoryForm(X86Context& ctx) {
    static const uint64_t at = std::getenv("S2_TEST_STORY_FORM") ? uint64_t(std::atoll(std::getenv("S2_TEST_STORY_FORM"))) : 0;
    if (!at || !g_player) return;
    if (g_frames == at) LOGR("test: the story's fill and change of form"), kernel::CallGuest(ctx, 0x100930, {0}), kernel::CallGuest(ctx, 0x100840, {0});
    static const char* comma = std::getenv("S2_TEST_STORY_FORM") ? std::strchr(std::getenv("S2_TEST_STORY_FORM"), ',') : nullptr;
    static const uint64_t length = comma ? uint64_t(std::atoll(comma + 1)) : 600;  // (S2_TEST_STORY_FORM=<frame>,<frames until the change back>)
    if (g_frames == at + length) LOGR("test: the story's change back"), kernel::CallGuest(ctx, 0x1008B0, {0});
}
}  // namespace game_coop
X86_EXTERN_FUNC(__imp__sub_00100930);
X86_FUNC(sub_00100930) {
    __imp__sub_00100930(ctx, base);
    game_coop::StoryForm(ctx, 0);
}
X86_EXTERN_FUNC(__imp__sub_00100840);
X86_FUNC(sub_00100840) {
    __imp__sub_00100840(ctx, base);
    game_coop::StoryForm(ctx, 1);
}
X86_EXTERN_FUNC(__imp__sub_001008B0);
X86_FUNC(sub_001008B0) {
    __imp__sub_001008B0(ctx, base);
    game_coop::StoryForm(ctx, 2);
}

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
    TestStoryForm(ctx);
    StoryHold(ctx);
    {   // S2_TEST_SAVEOUT=<frame>: Player 2's health to 60% and his meter to 40 at that frame; 60 frames later he is
        // taken out as for a save (BeforeSave) and comes back by himself: with what he had?
        static const uint64_t at = std::getenv("S2_TEST_SAVEOUT") ? uint64_t(std::atoll(std::getenv("S2_TEST_SAVEOUT"))) : 0;
        if (at && g_second && g_frames == at) {
            float health = ReadF(g_second + 0x1A) * 0.6f, meter = 40.0f;
            std::memcpy(kernel::HostPtr(g_second + 0x16), &health, 4);
            std::memcpy(kernel::HostPtr(g_second + 0x48A), &meter, 4);
            LOGR("test: Player 2's health set to %.0f of %.0f, his meter to 40", health, ReadF(g_second + 0x1A));
        }
        if (at && g_second && g_frames == at + 60) LOGR("test: Player 2 is taken out as for a save"), BeforeSave(ctx);
    }
    static uint32_t form[2] = {0, 0};
    uint32_t players[2] = {g_player, g_second};
    for (int i = 0; i < 2; i++) {
        if (!players[i] || !mem::Spans(players[i], 0x490)) { form[i] = 0; continue; }
        uint32_t now = kernel::R32(players[i] + 0xBC) & 0x80000;
        if (now != form[i]) LOGI("coop: Player %d is now in %s form (insanity meter %.1f)", i + 1, now ? "creature" : "human", ReadF(players[i] + 0x48A));
        form[i] = now;
    }
    // S2_TEST_FLAGS=1: a line whenever a player's state words change (+0xB0; +0xBC and +0xC0: what he is doing, by
    // name too), at most 400
    static const bool flags = std::getenv("S2_TEST_FLAGS") != nullptr;
    static uint32_t was[2][3] = {};
    static int said = 0;
    for (int i = 0; flags && i < 2; i++) {
        if (!players[i] || !mem::Spans(players[i], 0x490)) continue;
        uint32_t b0 = kernel::R32(players[i] + 0xB0), bc = kernel::R32(players[i] + 0xBC), c0 = kernel::R32(players[i] + 0xC0) & 0x7F;
        if ((b0 != was[i][0] || bc != was[i][1] || c0 != was[i][2]) && said < 400) {
            said++;
            uint32_t object = kernel::R32(players[i] + 4);
            LOGI("flags: frame %llu: Player %d +B0 %08X +BC %08X %02X [%s] at %.1f %.1f %.1f", (unsigned long long)g_frames, i + 1, b0, bc, c0, StateNames(bc, c0).c_str(),
                 ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8));
        }
        was[i][0] = b0, was[i][1] = bc, was[i][2] = c0;
    }
    // S2_TEST_MARK_STATE=<state>,<frames>[,<frames>...][|<state>,<frames>...]: a mark (the full-size pictures) that
    // many frames after a player begins that state, whenever the level took to load: for pictures of what the other
    // player sees then
    struct MarkState {
        int state = 0;
        std::vector<int> after;
        uint64_t began[2] = {};
        bool was_on[2] = {};
    };
    static std::vector<MarkState> mark_states = [] {
        std::vector<MarkState> list;
        for (const char* g = std::getenv("S2_TEST_MARK_STATE"); g && *g; g = std::strchr(g, '|') ? std::strchr(g, '|') + 1 : nullptr) {
            MarkState m;
            const char* end = std::strchr(g, '|');
            m.state = std::atoi(g);
            for (const char* e = std::strchr(g, ','); e && (!end || e < end); e = std::strchr(e + 1, ',')) m.after.push_back(std::atoi(e + 1));
            if (m.state > 0 && m.state < 39 && !m.after.empty()) list.push_back(m);
        }
        return list;
    }();
    for (MarkState& m : mark_states)
        for (int i = 0; i < 2; i++) {
            if (!players[i] || !mem::Spans(players[i], 0x490)) continue;
            bool on = ((kernel::R32(players[i] + 0xBC + (m.state >> 5) * 4) >> (m.state & 31)) & 1) != 0;
            if (on && !m.was_on[i]) m.began[i] = g_frames;
            m.was_on[i] = on;
            for (int after : m.after)
                if (m.began[i] && g_frames == m.began[i] + uint64_t(after)) game_record::Mark("a test asked");
        }
}

// S2_TEST_CALL=<frame>,<class id in hex>,<function in hex>[,<which>]: calls that function (this = the class object of
// the <which>-th loaded object of that class; one argument, 0), for setting off things a level sets off by message:
// an Autosave (0x108400), a Movie Player.
// S2_TEST_BODY=<frame>: each player's mover (the object at Player +0xF0 that carries him through the level) as
// words, for finding what makes two players bump into each other.
void TestBody() {
    static const char* v = std::getenv("S2_TEST_BODY");
    if (!v || g_frames != uint64_t(std::atoll(v))) return;
    uint32_t players[2] = {g_player, g_second};
    for (int i = 0; i < 2; i++) {
        uint32_t body = players[i] && mem::Spans(players[i], 0xF4) ? kernel::R32(players[i] + 0xF0) : 0;
        if (!body || !mem::Spans(body, 0x140)) continue;
        for (uint32_t at = 0; at < 0x140; at += 0x20) {
            char line[200];
            int n = std::snprintf(line, sizeof line, "body: Player %d +%03X:", i + 1, at);
            for (uint32_t k = 0; k < 0x20; k += 4) n += std::snprintf(line + n, sizeof line - n, " %08X", kernel::R32(body + at + k));
            LOGR("%s", line);
        }
    }
}
// The game's own "play this sound" (0x191070, this = the sound manager [0x384BBC]; the sound's resource, a place
// (0x3932AC: none, heard everywhere), a kind, 0), as the Player's code calls it for the full insanity meter.
// (A wave made for a place in the level (its header's nearest and farthest distances; the tonal hum 0xA500F4 has
// 0 and 42 m) is not heard with "no place": it is played where Player 1's object is, which is where the listener is.)
uint32_t PlaySound(X86Context& ctx, uint32_t sound, bool at_player, uint32_t flags) {
    uint32_t manager = kernel::R32(0x384BBC), ecx = ctx.ecx;
    if (!manager || !sound) return 0;
    ctx.ecx = manager;
    // (S2_TEST_SOUND_ARGS=<place: 0 none at all, 1 the "no place" vector, 2 Player 1>,<flags in hex>: for trying)
    static const char* args = std::getenv("S2_TEST_SOUND_ARGS");
    uint32_t place = at_player && g_player_object ? g_player_object + kPosition : 0x3932AC;
    if (args) {
        int which = std::atoi(args);
        place = which == 0 ? 0 : which == 2 && g_player_object ? g_player_object + kPosition : 0x3932AC;
        if (const char* comma = std::strchr(args, ',')) flags = uint32_t(std::strtoul(comma + 1, nullptr, 16));
    }
    uint32_t voice = kernel::CallGuest(ctx, 0x191070, {sound, place, flags, 0});
    if (args) LOGR("test: sound %08X place %08X flags %X gave %08X", sound, place, flags, voice);
    ctx.ecx = ecx;
    return voice;
}
// S2_TEST_SOUND=<frame>,<sound in hex>[,<frame>,<sound>...]: plays those sounds at those frames (finding a sound)
void TestSound(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_SOUND");
    if (!v) return;
    for (const char* c = v; c && *c;) {
        uint64_t frame = uint64_t(std::atoll(c));
        const char* comma = std::strchr(c, ',');
        if (!comma) break;
        uint32_t sound = uint32_t(std::strtoul(comma + 1, nullptr, 16));
        if (frame == g_frames) {
            static const bool at_player = std::getenv("S2_TEST_SOUND_AT") != nullptr;
            LOGR("test: sound %08X%s", sound, at_player ? " at Player 1" : "");
            PlaySound(ctx, sound, at_player);
        }
        c = std::strchr(comma + 1, ',');
        if (c) c++;
    }
}
// S2_TEST_SOUNDS=<frame>[,<first property>]: from that frame on, every sound named in Player 1's own properties is
// played, one every second and a half, each with a log line (its property and resource): a recording of the run
// (run.ps1 -Audio) is then searched for a sound that is known only as a file. A property's value counts as a sound
// when the sound manager knows it (0x1A26F0, this = manager; the resource).
void TestSounds(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_SOUNDS");
    if (!v || !g_player || !g_player_object) return;
    static std::vector<std::pair<uint32_t, uint32_t>> list;  // property, resource
    static size_t next = 0;
    static std::chrono::steady_clock::time_point at;
    if (g_frames == uint64_t(std::atoll(v))) {
        uint32_t first = std::strchr(v, ',') ? uint32_t(std::atoi(std::strchr(v, ',') + 1)) : 0;
        uint32_t props = kernel::R32(g_player_object + 0xEC), manager = kernel::R32(0x384BBC), ecx = ctx.ecx;
        std::map<uint32_t, bool> seen;
        for (uint32_t k = first; props && manager && k < 0x1C0; k++) {
            ctx.ecx = props;
            uint32_t where = kernel::CallGuest(ctx, kernel::R32(kernel::R32(props) + 8), {k, 0xFFFFFFFF});
            if (!where || !mem::IsCommitted(where)) continue;
            uint32_t value = kernel::R32(where);
            if (value < 0x10000 || value == 0xFFFFFFFF || seen[value]) continue;
            seen[value] = true;
            ctx.ecx = manager;
            if (kernel::CallGuest(ctx, 0x1A26F0, {value})) list.push_back({k, value});
        }
        ctx.ecx = ecx;
        LOGR("test: %zu of Player 1's properties name a sound", list.size());
        at = std::chrono::steady_clock::now();
    }
    if (next < list.size() && std::chrono::steady_clock::now() >= at) {
        LOGR("test: sound %zu, property 0x%X, resource %08X", next, list[next].first, list[next].second);
        PlaySound(ctx, list[next].second, false);
        next++;
        at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }
}
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

// ---- the arrow above the partner's head, and Player 2's ammunition, on the screen -----------------------------------
// Drawn in the runtime's ImGui layer, over the finished frame (through walls), as a smooth triangle at the window's
// own resolution (the first version was six stacked bars and looked pixelated). Orange with a dark edge; kArrowAlpha
// is how solid it is.
#include <imgui.h>
namespace game_coop {
constexpr float kArrowAlpha = 0.6f;  // (decided 2026-11-25: orange, see-through at 60%)
ImFont* g_number_font = nullptr;     // the numbers of Player 2's ammunition (Fonts, below)

// The font for the numbers: Windows' own Arial, digits only, large enough to be drawn smaller and stay sharp. Made
// once, before the first frame (GameInfo::window): the renderer builds its font texture with the first frame.
void Fonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();  // (the first font stays the runtime's own, for its overlay)
    static const ImWchar digits[] = {0x0020, 0x0020, 0x0030, 0x0039, 0};
    char path[512] = {};
    const char* windir = std::getenv("WINDIR");
    std::snprintf(path, sizeof path, "%s\\Fonts\\arial.ttf", windir ? windir : "C:\\Windows");
    if (FILE* f = std::fopen(path, "rb")) {
        std::fclose(f);
        g_number_font = io.Fonts->AddFontFromFileTTF(path, 64.0f, nullptr, digits);
    }
    LOGI("coop: the font for Player 2's ammunition: %s", g_number_font ? path : "none (the runtime's own, larger)");
}

// Player 2's ammunition in his half, where the game's own HUD has Player 1's in the left half (asked for
// 2026-11-26, "he also doesnt have an ammo hud", and again 2026-11-27 for split screen: the game's HUD keeps one set
// of numbers, Player 1's, and these are read from Player 2 himself, DetailFrame): a mark for each round in his
// weapon, the clip's empty places faint, and in a box the number the game shows there, what is in the weapon and
// what he carries for it. Placed and sized as the game's own for Player 1, measured in the owner's picture of
// 2026-11-27 (mark_00170.4 of session 015, 3840 x 1600, the shotgun) and in a test's (split_ammo5, 1920 x 800, the
// pistol), in the 640 x 480 layout of a half: the box from 525 to 553 across and 39 to 56 down, its number's digits
// half its height, 2.7 from its left edge; the rounds end 2 short of the box. A shotgun's are red shells with a
// brass base, 8.1 apart, 5.6 wide, 40.5 to 60.6 down; the others' silver rounds, 4.8 apart, 2.7 wide, 39 to 57 down.
void AmmoOfSecond(ImDrawList* draw, float left, float top, float vw, float vh) {
    int rounds = g_ammo2_rounds.load(), clip = g_ammo2_clip.load(), more = g_ammo2_more.load();
    bool shells = g_ammo2_shells.load() != 0;
    if (rounds < 0 || clip <= 0) return;
    constexpr float kOverscan = 2.0f, kRows = 480.0f - 2 * kOverscan;
    float down = gpu::g_flat2d.half_sy, across = gpu::g_flat2d.half_sx;
    // (the right half's layout: put into the half as the runtime puts the game's 2D, xdk/d3d8.cpp ForDraw)
    auto X = [&](float lx) { return left + (320.0f + 160.0f * (1.0f - across) + lx * 0.5f * across) / 640.0f * vw; };
    auto Y = [&](float ly) { return top + (ly * down - kOverscan) / kRows * vh; };
    float edge = std::max(1.0f, std::round(vh / 800.0f));
    // the box with the number
    int total = std::clamp(rounds + std::max(more, 0), 0, 9999);
    char text[8];
    std::snprintf(text, sizeof text, "%d", total);
    float box_t = Y(39.2f), box_b = Y(55.9f), box_r = X(553.3f), box_h = box_b - box_t;
    ImFont* font = g_number_font ? g_number_font : ImGui::GetFont();
    // (a font of size s has its digits 0.64 s high and their baseline 0.81 s under the top of its line: Arial's)
    float size = box_h * 0.78f;
    ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    float pad = X(2.7f) - X(0.0f), box_w = std::max(X(553.3f) - X(525.3f), text_size.x + 2 * pad);
    float box_l = box_r - box_w;
    draw->AddRectFilled(ImVec2(box_l, box_t), ImVec2(box_r, box_b), IM_COL32(0, 0, 0, 235));
    draw->AddRect(ImVec2(box_l, box_t), ImVec2(box_r, box_b), IM_COL32(100, 100, 100, 255), 0.0f, 0, edge * 1.5f);
    float baseline = (box_t + box_b) / 2 + 0.32f * size;
    draw->AddText(font, size, ImVec2(box_l + pad, baseline - 0.81f * size), IM_COL32(238, 238, 238, 255), text);
    // the rounds, right to left from the box (a big clip: at most twenty marks, each for several rounds)
    int each = (clip + 19) / 20, places = (clip + each - 1) / each, full = (rounds + each - 1) / each;
    float pitch = X(shells ? 8.1f : 4.8f) - X(0.0f), wide = X(shells ? 5.6f : 2.7f) - X(0.0f), gap = X(2.0f) - X(0.0f);
    float mark_t = Y(shells ? 40.5f : 39.2f), mark_b = Y(shells ? 60.6f : 56.8f), round = wide * (shells ? 0.35f : 0.5f);
    float base = (mark_b - mark_t) * (shells ? 0.16f : 0.22f);
    float x1 = box_l - gap;
    for (int i = 0; i < places; i++, x1 -= pitch) {
        float x0 = x1 - wide;
        if (i >= full) {  // an empty place
            draw->AddRect(ImVec2(x0, mark_t), ImVec2(x1, mark_b), IM_COL32(110, 104, 96, 120), round, 0, edge);
            continue;
        }
        draw->AddRectFilled(ImVec2(x0 - edge, mark_t - edge), ImVec2(x1 + edge, mark_b + edge), IM_COL32(8, 6, 4, 190), round);
        if (shells) {  // a shell: red, a brass base
            draw->AddRectFilled(ImVec2(x0, mark_t), ImVec2(x1, mark_b - base), IM_COL32(205, 72, 84, 255), round, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilled(ImVec2(x0, mark_b - base), ImVec2(x1, mark_b), IM_COL32(222, 190, 112, 255), round * 0.5f, ImDrawFlags_RoundCornersBottom);
        } else {  // a round: a silver bullet, a darker case
            draw->AddRectFilled(ImVec2(x0, mark_t), ImVec2(x1, mark_b - base), IM_COL32(196, 190, 180, 255), round, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilled(ImVec2(x0, mark_b - base), ImVec2(x1, mark_b), IM_COL32(150, 140, 126, 255), round * 0.3f, ImDrawFlags_RoundCornersBottom);
        }
    }
}

void Ui(double) {
    if (!SplitActive() || g_stopped_now) return;
    float left = gpu::g_picture[0].load(), top = gpu::g_picture[1].load(), vw = gpu::g_picture[2].load(), vh = gpu::g_picture[3].load();
    if (vw < 16 || vh < 16) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    AmmoOfSecond(draw, left, top, vw, vh);
    for (int view = 0; view < 2; view++) {
        float ax = gpu::g_partner_arrow[view].x.load(), ay = gpu::g_partner_arrow[view].y.load();
        if (ax < 0) continue;
        float x0 = left + vw * 0.5f * float(view), width = vw * 0.5f;
        float cx = x0 + std::clamp(ax, 0.03f, 0.97f) * width, tip = top + std::clamp(ay, 0.06f, 0.97f) * vh;
        float h = vh / 48.0f, w = h * 0.95f, edge = std::max(1.5f, vh / 540.0f);
        ImVec2 a(cx - w, tip - h), b(cx + w, tip - h), c(cx, tip);
        // (the dark edge is a line around it, not a larger triangle under it: under a see-through orange that
        // would make the orange muddy)
        draw->AddTriangleFilled(a, b, c, IM_COL32(237, 133, 26, int(255 * kArrowAlpha)));
        draw->AddTriangle(ImVec2(a.x - edge * 0.8f, a.y - edge * 0.5f), ImVec2(b.x + edge * 0.8f, b.y - edge * 0.5f), ImVec2(c.x, c.y + edge * 0.9f),
                          IM_COL32(12, 8, 3, int(220 * kArrowAlpha)), edge);
    }
}
}  // namespace game_coop

// ---- shadows for both players --------------------------------------------------------------------------------
// The shadow manager (0x1E2270, once a frame) keeps a character's shadow only near "the eye": it takes the eye's
// place from [0x3934D4] +0x8C (Player 1's camera), the caster's place from 0x18CD30, and fades the shadow out with
// the distance squared ([0x3A7C44] to [0x3A7C48]), dropping it beyond. With two players that left Player 2 without
// a shadow as soon as he was some metres from Player 1's camera, in both views: he looked as if he stood on nothing
// (seen by the owner, 2026-11-25). Here the eye is, for each caster, the nearer of the two players' cameras: the
// copy of the eye's place the manager holds on its stack is replaced when Player 2's camera is the nearer one.
// S2_COOP_NO_SHADOW_FIX=1 leaves the game's rule.
X86_EXTERN_FUNC(__imp__sub_0018CD30);
X86_FUNC(sub_0018CD30) {
    constexpr uint32_t kFromShadows = 0x001E23B9, kEyeOnStack = 0x28;
    uint32_t esp = ctx.esp, caster = ctx.ecx, record = ctx.esi;
    bool shadows = kernel::R32(esp) == kFromShadows;
    __imp__sub_0018CD30(ctx, base);
    if (!shadows || !game_coop::g_second_object) return;
    static const bool trace = std::getenv("S2_SHADOW_TRACE") != nullptr;  // each caster the manager looks at, every 120th frame
    if (trace && game_coop::Frames() % 120 == 0 && ctx.eax && mem::Spans(ctx.eax, 12))
        LOGI("coop: shadow trace: frame %llu caster %08X%s at %.1f %.1f %.1f eye %.1f %.1f %.1f size %.2f", (unsigned long long)game_coop::Frames(), caster,
             caster == game_coop::g_second_object ? " (Player 2)" : caster == game_coop::g_player_object ? " (Player 1)" : "",
             game_coop::ReadF(ctx.eax), game_coop::ReadF(ctx.eax + 4), game_coop::ReadF(ctx.eax + 8), game_coop::ReadF(esp + 4 + kEyeOnStack),
             game_coop::ReadF(esp + 8 + kEyeOnStack), game_coop::ReadF(esp + 12 + kEyeOnStack), game_coop::ReadF(caster + 0x58));
    if (trace && game_coop::Frames() % 120 == 0)
        LOGI("coop: shadow trace: record %08X strength %.2f (last frame) limits %.1f %.1f %.1f texture %08X flags %02X", record, game_coop::ReadF(record + 8),
             game_coop::ReadF(0x3A7C44), game_coop::ReadF(0x3A7C48), game_coop::ReadF(0x3A7C4C), kernel::R32(record + 0x2A), kernel::R32(record + 0x0C) >> 16);
    if (trace && game_coop::Frames() % 120 == 0) {
        std::string line;
        char one[32];
        for (uint32_t at = 0x0E; at < 0x2A; at += 4) std::snprintf(one, sizeof one, " %.2f", game_coop::ReadF(record + at)), line += one;
        line += " |";
        for (uint32_t at = 0x2E; at < 0x6E; at += 4) std::snprintf(one, sizeof one, " %.2f", game_coop::ReadF(record + at)), line += one;
        LOGI("coop: shadow trace: record %08X +0E:%s", record, line.c_str());
    }
    static const bool off = std::getenv("S2_COOP_NO_SHADOW_FIX") != nullptr;
    uint32_t camera = game_coop::ViewCamera2(), eye = camera && mem::Spans(camera, 0xA4) ? kernel::R32(camera + 0xA0) : 0;
    uint32_t place = ctx.eax, held = esp + 4 + kEyeOnStack;
    if (off || !eye || !mem::Spans(eye, 0x50) || !place || !mem::Spans(place, 12)) return;
    float mine = 0, other = 0, second[3];
    for (int i = 0; i < 3; i++) {
        float at = game_coop::ReadF(place + i * 4);
        second[i] = game_coop::ReadF(eye + 0x44 + i * 4);
        float a = game_coop::ReadF(held + i * 4) - at, b = second[i] - at;
        mine += a * a, other += b * b;
    }
    if (other < mine) std::memcpy(kernel::HostPtr(held), second, sizeof second);
}

// The world is drawn for a view camera (0x1D1E00, this = 0x3972E8). It runs the shadow manager only when the view's
// switch at +0x50 is set, and the camera loop (0x1B83E0) sets that for camera 0 alone, "the screen": so shadows were
// drawn into Player 1's view only, and nobody had a shadow in Player 2's. While Player 2's camera is drawn the
// switch is set here as well (and put back after).
X86_EXTERN_FUNC(__imp__sub_001D1E00);
X86_FUNC(sub_001D1E00) {
    static const bool off = std::getenv("S2_COOP_NO_SHADOW_FIX") != nullptr;
    uint32_t view = kernel::R32(game_coop::kViewPointer);
    bool second = game_coop::g_screen_pass && view && !off && !kernel::R32(view + 0x50);
    if (second) kernel::W32(view + 0x50, 1);
    __imp__sub_001D1E00(ctx, base);
    if (second && kernel::R32(game_coop::kViewPointer) == view) kernel::W32(view + 0x50, 0);
}

// A shadow is dropped by the manager when its volume is outside the camera being drawn (0x1E0A00: the test at
// 0x1E0D7F, 0x17C450 with this = that camera's frustum at +0x110; "outside" makes the manager drop the record,
// 0x1E11F0). With the manager now run for both cameras, a character one player sees and the other does not would
// be dropped in one pass and made anew in the other, every frame. Here a shadow counts as inside while either
// player's camera sees it.
X86_EXTERN_FUNC(__imp__sub_0017C450);
X86_FUNC(sub_0017C450) {
    constexpr uint32_t kFromShadow = 0x001E0D84, kFrustum = 0x110;
    bool shadow = kernel::R32(ctx.esp) == kFromShadow;
    uint32_t mine = ctx.ecx, volume = kernel::R32(ctx.esp + 4);
    __imp__sub_0017C450(ctx, base);
    if (!shadow || !(ctx.eax & 0xFF) || !game_coop::g_second_object) return;
    static const bool off = std::getenv("S2_COOP_NO_SHADOW_FIX") != nullptr;
    uint32_t view = kernel::R32(game_coop::kViewPointer), first = view ? kernel::R32(view + game_coop::kViewCameras) : 0;
    uint32_t second = game_coop::ViewCamera2();
    if (off || !first || !second) return;
    uint32_t other = mine == first + kFrustum ? second + kFrustum : first + kFrustum;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = other;
    uint32_t outside = kernel::CallGuest(ctx, 0x0017C450, {volume});
    ctx.ecx = ecx;
    ctx.eax = (ctx.eax & ~0xFFu) | (outside & 0xFF);
}

// ---- which draws are Player 2's model ------------------------------------------------------------------------------
// The runtime asks for every draw (gpu::g_draw_variant). S2_DRAW_TRACE=<frame>: every draw of that frame with its
// first texture, the world transform's place and who called (the code addresses on the game's stack).
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include <filesystem>
#include <fstream>
#include "app/app.h"
namespace game_coop {
bool g_drawing_second = false;  // the engine is drawing Player 2's object (0x1D1970)
int g_draw_depth = 0;           // how many object draws (0x1D1970) are open, one inside another
int g_skin_depth = -1;          // the depth of the open draw of a player's body that wears another character (skins.cpp)
uint32_t g_skin_place = 0;      // ... and how many of its own textured draws went out: the place being drawn
int g_skin_char = 0;            // ... and which character it wears (1 Blackmore, 2 Chief)

// Player 2's outfit (decided 2026-11-24: blue trousers and a black tank top; straps, boots, skin and blood as they
// are). His model is drawn with the same textures as Player 1's. Two of them are the clothes: the trousers
// (256 x 256, all of it the orange cloth) and the one with the tank top in its lower part (from row 172; above it
// boots and straps). The runtime draws a texture pack's "player2" images in their place for the draws marked as his
// (State::variant 1). Those two images are made here, on the player's PC, out of the game's own textures the first
// time his model is drawn with them, and kept in textures\replacement\player2 beside the program: nothing of the
// game's art is in the program. The textures are known by the name the texture dumps give them.
constexpr uint64_t kTrousers = 0x9bda0bdae0112ab5ull, kTankTop = 0x6011d07b0583df74ull;
void Outfit(const gpu::State& s) {
    static std::map<uint32_t, uint64_t> seen;  // texture data address -> its first bytes when it was looked at
    static const bool off = std::getenv("S2_COOP_NO_OUTFIT") != nullptr;
    gpu::TextureDesc d;
    if (off || !s.textures[0] || !gpu::Describe(s.textures[0], d) || d.width != 256 || d.height != 256) return;
    const gpu::FormatInfo& fi = gpu::Format(d.format);
    if (fi.host != gpu::Host::BC1 || !mem::Spans(d.data, 32768)) return;
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    uint64_t first;
    std::memcpy(&first, guest + 4096, 8);
    auto it = seen.find(d.data);
    if (it != seen.end() && it->second == first) return;
    seen[d.data] = first;
    uint64_t key = gpu::texturedumper::KeyOf(d, fi, guest, nullptr);
    if (key != kTrousers && key != kTankTop) return;
    namespace fs = std::filesystem;
    fs::path dir = app::ExeDir() / "textures" / "replacement" / "player2";
    char name[64];
    std::snprintf(name, sizeof name, "256x256_%016llx_coop.dds", (unsigned long long)key);
    std::error_code gone;
    std::filesystem::remove(dir / name, gone);  // (the earlier versions' images, with parts of the top left light)
    std::snprintf(name, sizeof name, "256x256_%016llx_coop2.dds", (unsigned long long)key);
    std::filesystem::remove(dir / name, gone);
    std::snprintf(name, sizeof name, "256x256_%016llx_coop3.dds", (unsigned long long)key);
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return;
    // DXT1 blocks: two 565 colours and 16 two-bit picks
    std::vector<uint8_t> blocks;
    uint32_t pitch = 0;
    gpu::DecodeLevel(d, 0, 0, guest, nullptr, blocks, pitch);
    if (pitch < 64 * 8 || blocks.size() < size_t(pitch) * 64) return;
    std::vector<uint8_t> file(128 + 256 * 256 * 4, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(&file[at], &v, 4); };
    put(0, 0x20534444), put(4, 124), put(8, 0x100F), put(12, 256), put(16, 256), put(20, 256 * 4);  // "DDS ", caps, height, width, pitch
    put(76, 32), put(80, 0x41), put(88, 32), put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000);
    put(108, 0x1000);
    for (uint32_t by = 0; by < 64; by++)
        for (uint32_t bx = 0; bx < 64; bx++) {
            const uint8_t* b = blocks.data() + size_t(by) * pitch + size_t(bx) * 8;
            uint32_t c[2] = {uint32_t(b[0] | b[1] << 8), uint32_t(b[2] | b[3] << 8)}, rgb[4][3];
            for (int k = 0; k < 2; k++) {
                uint32_t r = c[k] >> 11, g = (c[k] >> 5) & 63, bl = c[k] & 31;
                rgb[k][0] = (r << 3) | (r >> 2), rgb[k][1] = (g << 2) | (g >> 4), rgb[k][2] = (bl << 3) | (bl >> 2);
            }
            for (int ch = 0; ch < 3; ch++) {
                rgb[2][ch] = c[0] > c[1] ? (2 * rgb[0][ch] + rgb[1][ch]) / 3 : (rgb[0][ch] + rgb[1][ch]) / 2;
                rgb[3][ch] = c[0] > c[1] ? (rgb[0][ch] + 2 * rgb[1][ch]) / 3 : 0;
            }
            uint32_t picks = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
            for (uint32_t t = 0; t < 16; t++) {
                uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                const uint32_t* col = rgb[(picks >> (2 * t)) & 3];
                float r = float(col[0]), g = float(col[1]), bl = float(col[2]);
                if (key == kTrousers) {
                    // orange cloth to blue denim: its brightness is its red; what is dark (the stripe, the seams) stays dark
                    float v = r;
                    r = v * 0.20f, g = v * 0.40f, bl = std::min(255.0f, v * 0.86f);
                } else {
                    // The light cloth to black, wherever it is in the texture (the top fills rows 94 to 255 and
                    // has a patch higher up; the first version only did rows 172 on and left his chest and
                    // shoulders white). Cloth: not dark, and grey or beige. The boots and straps are dark already;
                    // blood (much more red than green) is left as it is.
                    float low = std::min({r, g, bl}), high = std::max({r, g, bl});
                    bool cloth = low > 55.0f && high - low < 80.0f, blood = r > g * 1.45f && r > 60.0f;
// (from row 96 down everything is the top: its shaded and dirty parts are darker than "cloth" and
                    // stayed as khaki blotches in the second version)
                    if ((cloth || y >= 96) && !blood) r *= 0.17f, g *= 0.17f, bl *= 0.19f;
                }
                uint8_t* out = &file[128 + (size_t(y) * 256 + x) * 4];
                out[0] = uint8_t(bl), out[1] = uint8_t(g), out[2] = uint8_t(r), out[3] = 255;
            }
        }
    fs::create_directories(dir, ec);
    std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    LOGR("coop: Player 2's outfit: %s made from the game's own texture, in %s", key == kTrousers ? "blue trousers" : "black tank top",
         dir.string().c_str());
}
uint32_t DrawVariant(X86Context& ctx, const gpu::State& s) {
    {   // the detail record: what each view drew this frame (see DetailFrame)
        PassStats& p = g_pass[g_screen_pass ? 1 : 0];
        p.draws++;
        uint32_t fog = s.rs[gpu::kRsFogEnable], z = s.rs[gpu::kRsZEnable];
        if (fog) p.fogged++;
        uint64_t h = (uint64_t(fog ? s.rs[gpu::kRsFogColor] : 0) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(fog ? s.rs[gpu::kRsFogStart] : 0) << 17) ^
                     (uint64_t(fog ? s.rs[gpu::kRsFogEnd] : 0) << 3) ^ (uint64_t(s.rs[gpu::kRsAmbient]) * 0xC2B2AE3D27D4EB4Full) ^ s.light_enable;
        p.hash += h * 0xFF51AFD7ED558CCDull + 1;
        if (z && !p.have) {  // the first draw of the pass that tests depth: the view's own state
            p.have = true;
            p.fog_on = fog != 0, p.fog_color = s.rs[gpu::kRsFogColor], p.fog_start = s.rs[gpu::kRsFogStart], p.fog_end = s.rs[gpu::kRsFogEnd];
            p.ambient = s.rs[gpu::kRsAmbient], p.lights = s.light_enable;
            p.proj[0] = s.transforms[3][0], p.proj[1] = s.transforms[3][5], p.proj[2] = s.transforms[3][10], p.proj[3] = s.transforms[3][14];
            p.eye[0] = s.transforms[2][12], p.eye[1] = s.transforms[2][13], p.eye[2] = s.transforms[2][14];
            p.vw = s.viewport.w, p.vh = s.viewport.h, p.rt = s.rt.data;
        }
    }
    static const uint64_t trace_at = std::getenv("S2_DRAW_TRACE") ? uint64_t(std::atoll(std::getenv("S2_DRAW_TRACE"))) : 0;
    // S2_CAPTURE_FRAME=<frame> (with XBE_CAPTURE_DIR): the runtime's frame capture, as F3 would, at that frame
    static const uint64_t cap_at = std::getenv("S2_CAPTURE_FRAME") ? uint64_t(std::atoll(std::getenv("S2_CAPTURE_FRAME"))) : 0;
    static bool cap_done = false;
    if (cap_at && !cap_done && g_frames + 1 >= cap_at) cap_done = true, gpu::g_capture = true;
    if (trace_at && g_frames == trace_at) {
        gpu::TextureDesc d;
        bool has = s.textures[0] && gpu::Describe(s.textures[0], d);
        char line[600];
        int n = std::snprintf(line, sizeof line, "draw trace: tex %08X %ux%u fmt %X data %08X world %.2f %.2f %.2f view2 %d from", s.textures[0], has ? d.width : 0,
                              has ? d.height : 0, has ? d.format : 0, has ? d.data : 0, s.transforms[256][12], s.transforms[256][13], s.transforms[256][14],
                              int(g_screen_pass));
        int found = 0;
        for (uint32_t at = ctx.esp; at < ctx.esp + 0x400 && found < 12; at += 4) {
            if (!mem::Spans(at, 4)) break;
            uint32_t v = kernel::R32(at);
            if (v >= 0x11000 && v < 0x24A000) n += std::snprintf(line + n, sizeof line - n, " %06X", v), found++;
        }
        // where the players are in the vertex program's constants: as a register's x y z, or as the w of three
        // registers in a row (the last column of a 3 x 4 matrix)
        if (g_second_object && g_player_object) {
            float p1[3], p2[3];
            for (int i = 0; i < 3; i++) p1[i] = ReadF(g_player_object + kPosition + i * 4), p2[i] = ReadF(g_second_object + kPosition + i * 4);
            auto near = [&](const float* v, const float* p) {
                float dx = v[0] - p[0], dy = v[1] - p[1] - 1.0f, dz = v[2] - p[2];
                return dx * dx + dy * dy + dz * dz;
            };
            int c1 = 0, c2 = 0, first = -1, kind = 0;
            for (int r = 0; r + 2 < 192; r++) {
                float a[3] = {s.vsc[r][0], s.vsc[r][1], s.vsc[r][2]}, b[3] = {s.vsc[r][3], s.vsc[r + 1][3], s.vsc[r + 2][3]};
                for (int k = 0; k < 2; k++) {
                    const float* v = k ? b : a;
                    float d1 = near(v, p1), d2 = near(v, p2);
                    if (d1 > 1.6f && d2 > 1.6f) continue;
                    (d1 < d2 ? c1 : c2)++;
                    if (first < 0) first = r, kind = k;
                }
            }
            n += std::snprintf(line + n, sizeof line - n, " | consts near P1 %d near P2 %d first %d kind %d", c1, c2, first, kind);
        }
        LOGI("%s", line);
    }
    // A body that wears another character (skins.cpp): its own draws (not those of what it holds, drawn by calls
    // inside its own) are drawn with that character's image for the place (gpu::g_draw_force_image). The
    // small blood layers (32 x 32) are drawn as they are and do not count as a place.
    if (g_skin_depth >= 0 && g_draw_depth == g_skin_depth + 1) {
        gpu::TextureDesc d;
        if (s.textures[0] && gpu::Describe(s.textures[0], d) && d.width >= 64 && d.height >= 64)
            gpu::g_draw_force_image = game_skins::SkinKey(g_skin_char, g_skin_place++);
        return 0;
    }
    if (!g_drawing_second) return 0;
    {   // tests: S2_P2_DRAWS=<frame>: every draw of Player 2's model in that frame, with each stage's texture
        static const uint64_t at = std::getenv("S2_P2_DRAWS") ? uint64_t(std::atoll(std::getenv("S2_P2_DRAWS"))) : 0;
        if (at && g_frames == at) {
            char line[400];
            int n = std::snprintf(line, sizeof line, "p2 draw: view %d", int(g_screen_pass));
            for (int stage = 0; stage < 4; stage++) {
                gpu::TextureDesc d;
                if (!s.textures[stage] || !gpu::Describe(s.textures[stage], d)) continue;
                uint64_t key = gpu::texturedumper::Key(d, gpu::Format(d.format), static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr);
                n += std::snprintf(line + n, sizeof line - n, " | stage %d %08X %ux%u_%016llx", stage, s.textures[stage], d.width, d.height, (unsigned long long)key);
            }
            n += std::snprintf(line + n, sizeof line - n, " | from %s", CallersOf(ctx).c_str());
            LOGR("%s", line);
        }
    }
    Outfit(s);
    // S2_P2_TEXTURES=1: the textures Player 2's model is drawn with, each once, by the name the texture dumps give them
    static const bool list = std::getenv("S2_P2_TEXTURES") != nullptr;
    if (list) {
        static std::map<uint32_t, int> seen;
        for (int stage = 0; stage < 4; stage++) {
            gpu::TextureDesc d;
            if (!s.textures[stage] || !gpu::Describe(s.textures[stage], d) || seen[d.data]++) continue;
            uint64_t key = gpu::texturedumper::Key(d, gpu::Format(d.format), static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr);
            LOGI("coop: Player 2's model: stage %d texture %ux%u_%016llx (format %X)", stage, d.width, d.height, (unsigned long long)key, d.format);
        }
    }
    return 1;
}
static const int g_draw_variant_set = (gpu::g_draw_variant = DrawVariant, 0);
}  // namespace game_coop

// An object is drawn (0x1D1970, the object on the stack): its meshes go out from inside this call, so the draws made
// meanwhile are that object's. Player 2's are marked for the runtime (texture pack variant 1: his own outfit).
//
// One call can draw several objects. The renderer sorts what a view sees by model (0x1D1840 puts each object into
// one of 128 lists by its model number, the word at +0x64), links the objects that are drawn alike into a chain (the
// next one at +0xD4; 0x1D1BCA) and calls 0x1D1970 for the first of a chain only: the rest are drawn from inside that
// call (0x1D1AAD: the next of the chain, its link cleared, and round again). Everything this hook decides, it
// decided for the first one: the others went by unseen. Found 2026-11-26 in a picture of the flashlight tests: with
// Player 2 in first person a second body in his clothes stood where the stand-in bodies had been made. It was
// Player 1's stand-in, put away and not to be drawn, linked behind Player 2's (two objects of one model) and drawn
// with it, in its clothes. The same way a put-away stand-in at the front of a chain took the bodies or the weapons
// behind it out of the picture, and one player's weapon could be drawn in the other's outfit pass. A chain that has
// one of the players' things in it is now taken apart: each object gets a call of its own and is judged by itself.
X86_EXTERN_FUNC(__imp__sub_001D1970);
namespace game_coop {
void DrawOne(X86Context& ctx, uint8_t* base);
// the players, their stand-in bodies, and what any of them holds
bool PlayersThing(uint32_t object) {
    if (!object) return false;
    if (object == g_player_object || object == g_second_object) return true;
    for (const Double& d : g_doubles)
        if (OfDouble(d, object) || (d.object && (object == d.owner_held[0] || object == d.owner_held[1] || object == d.owner_loose[0] || object == d.owner_loose[1]))) return true;
    for (uint32_t player : {g_player, g_second})
        if (player && (object == Named(player, 0x3AC) || object == Named(player, 0x3AE))) return true;
    return false;
}
}  // namespace game_coop
X86_FUNC(sub_001D1970) {
    using namespace game_coop;
    constexpr uint32_t kNextAlike = 0xD4;
    uint32_t first = kernel::R32(ctx.esp + 4), renderer = ctx.ecx;
    uint32_t rest[64];
    int count = 0;
    static const bool whole = std::getenv("S2_COOP_WHOLE_CHAINS") != nullptr;  // tests: as before 2026-11-26
    if (!whole && g_second_object && first && mem::Spans(first, 0x110) && kernel::R32(first + kNextAlike)) {
        bool ours = PlayersThing(first);
        int length = 0;
        for (uint32_t o = kernel::R32(first + kNextAlike); o && length < 64 && mem::Spans(o, 0x110); o = kernel::R32(o + kNextAlike), length++) ours |= PlayersThing(o);
        if (ours && length < 64) {
            for (uint32_t o = kernel::R32(first + kNextAlike); o && count < 64;) {
                uint32_t next = kernel::R32(o + kNextAlike);
                kernel::W32(o + kNextAlike, 0);
                rest[count++] = o;
                o = next;
            }
            kernel::W32(first + kNextAlike, 0);
            static bool said = false;
            if (!said) said = true, LOGI("coop: a chain of %d objects drawn alike has one of the players' things in it: each is drawn by its own call", count + 1);
        }
    }
    DrawOne(ctx, base);
    for (int i = 0; i < count; i++) {
        uint32_t eax = ctx.eax;
        ctx.ecx = renderer;
        kernel::CallGuest(ctx, 0x1D1970, {rest[i]});
        ctx.eax = eax;
    }
}
void game_coop::DrawOne(X86Context& ctx, uint8_t* base) {
    {
        using namespace game_coop;
        // A stand-in body and what it holds: in every view but its owner's, and nowhere while it is put away or the
        // picture is one view. Its owner's arms and what they hold, while it is out: in his own view only.
        // (whose view is being drawn: Player 2's while the engine draws his view camera, else Player 1's)
        uint32_t object = kernel::R32(ctx.esp + 4);
        int view = g_screen_pass ? 2 : 1;
        // (tests: S2_BODY_TRACE=<frame> lists every Player's object the engine draws in that frame, and what is done with it)
        static const uint64_t body_trace = std::getenv("S2_BODY_TRACE") ? uint64_t(std::atoll(std::getenv("S2_BODY_TRACE"))) : 0;
        bool traced = body_trace && g_frames == body_trace && object && mem::Spans(object, 0x110) && ClassOf(object) == 0xC8BC44E1;
        auto who = [&]() -> const char* {
            return object == g_player_object ? "Player 1" : object == g_second_object ? "Player 2" : object == g_doubles[0].object ? "Player 1's stand-in" :
                   object == g_doubles[1].object ? "Player 2's stand-in" : "ANOTHER Player object";
        };
        for (int which = 0; object && which < 2; which++) {
            const Double& d = g_doubles[which];
            if (!d.object) continue;
            // (what hangs on either of them too: the thing ready to throw and the one in the hand for the throw. His
            // hang on his first-person arms, the stand-in's on its belt: the other player sees the stand-in's, he
            // sees his own.)
            bool its = OfDouble(d, object);
            bool his = d.out && (object == (which ? g_second_object : g_player_object) || object == d.owner_held[0] || object == d.owner_held[1] ||
                                 object == d.owner_loose[0] || object == d.owner_loose[1]);
            if ((its && (!d.out || view == which + 1 || g_one_view)) || (his && view != which + 1 && !g_one_view)) {
                if (traced) LOGI("body trace: view %d: %s (object %08X at %.2f %.2f %.2f, flags %08X) is NOT drawn", view, who(), object, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8), kernel::R32(object + 8));
                kernel::Return(ctx, 0, 1);
                return;
            }
        }
        if (traced) LOGI("body trace: view %d: %s (object %08X at %.2f %.2f %.2f, flags %08X) is drawn, called from %s", view, who(), object, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8), kernel::R32(object + 8), Callers(ctx, 6).c_str());
    }
    bool before = game_coop::g_drawing_second;
    // (Player 2's outfit: for his own object, and for his stand-in)
    game_coop::g_drawing_second = game_coop::g_second_object && (kernel::R32(ctx.esp + 4) == game_coop::g_second_object ||
                                                                 (game_coop::g_doubles[1].object && kernel::R32(ctx.esp + 4) == game_coop::g_doubles[1].object));
    {   // (S2_BODY_TRACE: every object drawn in that frame within four metres of Player 1, with how deep inside another's draw it is)
        using namespace game_coop;
        static const uint64_t body_trace = std::getenv("S2_BODY_TRACE") ? uint64_t(std::atoll(std::getenv("S2_BODY_TRACE"))) : 0;
        uint32_t object = kernel::R32(ctx.esp + 4);
        if (body_trace && g_frames == body_trace && object && mem::Spans(object, 0x110) && g_player_object) {
            float d2 = 0;
            for (int k = 0; k < 3; k++) {
                float d = ReadF(object + kPosition + k * 4) - ReadF(g_player_object + kPosition + k * 4);
                d2 += d * d;
            }
            if (d2 < 16.0f)
                LOGI("body trace: view %d depth %d: object %08X class %08X template %u at %.2f %.2f %.2f flags %08X, outfit %d, called from %s", g_screen_pass ? 2 : 1, g_draw_depth, object,
                     ClassOf(object), kernel::R32(object + kTemplateWord) & 0xFFFF, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8), kernel::R32(object + 8),
                     int(g_drawing_second), Callers(ctx, 5).c_str());
        }
    }
    int skin_before = game_coop::g_skin_depth;
    uint32_t place_before = game_coop::g_skin_place;
    int char_before = game_coop::g_skin_char;
    if (int c = game_skins::BodySkin(kernel::R32(ctx.esp + 4))) game_coop::g_skin_depth = game_coop::g_draw_depth, game_coop::g_skin_place = 0, game_coop::g_skin_char = c;
    else if (game_coop::g_skin_depth >= 0 && kernel::R32(ctx.esp + 4)) game_coop::g_skin_depth = -1;  // (something else drawn from inside it)
    game_coop::g_draw_depth++;
    __imp__sub_001D1970(ctx, base);
    game_coop::g_draw_depth--;
    game_coop::g_drawing_second = before;
    game_coop::g_skin_depth = skin_before, game_coop::g_skin_place = place_before, game_coop::g_skin_char = char_before;
}

namespace game_listener { uint32_t NoPlaceStandIn(); }  // listener.cpp
// S2_SOUND_TRACE=1: every sound the game asks for (0x191070: the wave, the place, flags, a float), who asked and what
// came back.
X86_EXTERN_FUNC(__imp__sub_00191070);
X86_FUNC(sub_00191070) {
    static const bool trace = std::getenv("S2_SOUND_TRACE") != nullptr;
    uint32_t from = kernel::R32(ctx.esp), sound = kernel::R32(ctx.esp + 4), place = kernel::R32(ctx.esp + 8), flags = kernel::R32(ctx.esp + 12);
    float scale = game_coop::ReadF(ctx.esp + 16);
    // a sound "heard everywhere" sits where the game hears from (src\game\listener.cpp: after Load Game that is
    // Player 1, not the origin the "no place" vector names)
    if (place == 0x3932AC)
        if (uint32_t here = game_listener::NoPlaceStandIn()) kernel::W32(ctx.esp + 8, here);
    __imp__sub_00191070(ctx, base);
    {   // the record: how many sounds started, how many were refused, which packages they came from
        static uint32_t started = 0, refused = 0, packages[8] = {}, kinds = 0, said = 0;
        static std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        uint32_t package = sound >> 16;
        if (ctx.eax) started++;
        else if (!sound) {}  // (a call with no sound named: nothing was asked for)
        else if (++refused, said < 6 && ++said) {
            // why (notes 2026-11-26): the device off, the manager's +0x67E set, or its voice counts (+0x676 3D, +0x67A 2D)
            // at the device's limits (+0x2E 3D, +0x32 2D)
            uint32_t manager = kernel::R32(0x384BBC), device = kernel::R32(0x398CB0);
            LOGI("sound: the sound %08X was asked for and not played (place %08X, flags %X, asked from %08X; voices 3D %u of %u, 2D %u of %u, +67E %u, device %08X)",
                 sound, place, flags, from, manager ? kernel::R32(manager + 0x676) : 0, device ? kernel::R32(device + 0x2E) : 0,
                 manager ? kernel::R32(manager + 0x67A) : 0, device ? kernel::R32(device + 0x32) : 0, manager ? kernel::R32(manager + 0x67E) : 0, device);
        }
        bool known = false;
        for (uint32_t i = 0; i < kinds; i++) known |= packages[i] == package;
        if (!known && kinds < 8) packages[kinds++] = package;
        auto now = std::chrono::steady_clock::now();
        if (now - since >= std::chrono::seconds(10)) {
            char list[80] = "";
            int n = 0;
            for (uint32_t i = 0; i < kinds; i++) n += std::snprintf(list + n, sizeof list - size_t(n), " %X", packages[i]);
            uint32_t manager = kernel::R32(0x384BBC);  // its voice counts as the last sound asked for left them (0x1909B0)
            LOGI("sound: %u sounds started in the last 10 s, %u not played; from the packages%s; voices 3D %u, 2D %u", started,
                 refused, list, manager ? kernel::R32(manager + 0x676) : 0, manager ? kernel::R32(manager + 0x67A) : 0);
            since = now, started = refused = kinds = said = 0;
        }
    }
    if (trace) {
        float at[3] = {0, 0, 0};
        if (place && mem::Spans(place, 12)) std::memcpy(at, kernel::HostPtr(place), 12);
        LOGI("sound trace: wave %08X place %08X (%.1f %.1f %.1f) flags %X scale %.2f from %08X gave %08X", sound, place, at[0], at[1], at[2], flags, scale, from, ctx.eax);
    }
}

// ---- the flashlight --------------------------------------------------------------------------------------------------
// Reported 2026-11-26: "i also noticed player 2 doesnt get a flashlight", and asked for the same day: "can we make the
// flashlight always have infinite battery too".
// How the game has it:
//   - The flashlight is a thing a Player carries: an item of the class Flashlight (0xCF3A3FA5) in his inventory, and
//     an object of that class fixed to his skeleton (0x13A00: bone 0x10, 0x18AD50), its handle at Player +0x16C.
//     0x12570 (this = Player) gives his Flashlight (the class object). 0x153A0, the one way things are given, makes
//     it for the first Flashlight he is given and gives batteries (its property 0x4E, an item) for every further one.
//   - The Flashlight: flags at +0xA8 (1 on, 2 the battery is giving out, 4 the battery is dead), the charge in
//     seconds at +0xAC (full: property 0x4C), the time it takes to give out at +0xB0, the handle of who carries it at
//     +0x56, its light (class 54E01F83) at +0xC0 and its cone at +0xC2. Method +0x98 (0xEAEB0) switches it on, +0x9C
//     (0xEAD60) off; 0xEA550 puts a new battery in. Its once-a-frame method +0x1C (0xEA840) runs the battery down
//     while it is on and points the beam: by the flashlight's own place for "the player" in first person (0x2F7D0),
//     else by the game's Camera.
//   - The button (action 0x3F6, the right stick's click) is read in the Player's input method (0x1BC90 at 0x1C6EB)
//     and switches HIS flashlight. His once-a-frame method (at 0x30043) puts in a spare battery when one gives out.
// Cause of the report: Player 2 had no flashlight to switch. He is given what Player 1 carries of weapons,
// ammunition, grenades and health (ForBoth), and the Flashlight's class was not among them.
// What is done:
//   - The Flashlight is for both: Player 2 is given one when he comes in and whenever Player 1 is given one, the
//     game's own way. His starts as Player 1's is at that moment, on or off.
//   - A Flashlight's once-a-frame method runs as Player 2 when he carries it (AsSecond): "the player" is him and
//     the game's Camera is his, so his beam goes where HE looks.
//   - The battery never runs down for a flashlight a Player carries, alone or with two: before its once-a-frame
//     method its charge is filled up, and one that came back dead from a saved game gets a new battery from nowhere
//     the first time it is on. Spare batteries are never used up.
//   S2_TORCH_BATTERY=1: the game's own battery (tests)
namespace game_coop {
uint32_t FlashlightOf(uint32_t player) {
    if (!player || !mem::Spans(player, 0x170)) return 0;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = kernel::R16(player + 0x16C);
    uint32_t object = table && h ? kernel::R32(table + h * 6) : 0;
    if (!object || !mem::Spans(object, 0x110) || ClassOf(object) != kFlashlightClass) return 0;
    return kernel::R32(object + kClassObject);
}
// one of an object's properties as a float (the object's own property reader: its method +8, number and -1)
float PropertyF(X86Context& ctx, uint32_t object, uint32_t number, float otherwise) {
    uint32_t props = object && mem::Spans(object, 0xF0) ? kernel::R32(object + 0xEC) : 0;
    if (!props || !mem::IsCommitted(props)) return otherwise;
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = props;
    uint32_t at = kernel::CallGuest(ctx, kernel::R32(kernel::R32(props) + 8), {number, 0xFFFFFFFF});
    ctx.ecx = ecx, ctx.eax = eax;
    return at && mem::Spans(at, 4) ? ReadF(at) : otherwise;
}
// the players' flashlights, once a frame: what they are doing, into the log when it changes
// S2_TEST_TORCH_AGAIN=<frame>[,<frames between>[,<times>]]: Player 1's flashlight is ended at that frame and he is
// given a new one <frames between> (120) later, switched on; <times> times over (1), each time <frames between>
// after the last. What a scripted scene of the apartment level does (it takes his things and gives them back):
// after it the flashlight's light was stripes and dots in the owner's session of 2026-11-27. (Tests of the
// renderer's targets; see the SDK's D3D12Backend::SamplesTarget.)
void TestTorchAgain(X86Context& ctx) {
    static const char* v = std::getenv("S2_TEST_TORCH_AGAIN");
    if (!v || !g_player || !g_player_object) return;
    static uint64_t at = 0;
    static int gap = 120, times = 1, parsed = 0, done = 0;
    static uint32_t item = 0xFFFF;
    static uint64_t give_at = 0;
    if (!parsed) parsed = 1, std::sscanf(v, "%llu,%d,%d", reinterpret_cast<unsigned long long*>(&at), &gap, &times);
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    if (done < times && !give_at && g_frames >= at + uint64_t(done) * uint64_t(gap) * 2) {
        uint32_t table = kernel::R32(kObjectManager + 0x0C), h = kernel::R16(g_player + 0x16C);
        uint32_t object = table && h ? kernel::R32(table + h * 6) : 0;
        if (object && mem::Spans(object, 0x110) && ClassOf(object) == kFlashlightClass && int32_t(kernel::R32(object + 8)) >= 0) {
            item = kernel::R32(object + kTemplateWord) & 0xFFFF;
            ctx.ecx = object;
            kernel::CallGuest(ctx, kObjectEnd, {});
            give_at = g_frames + uint64_t(gap);
            LOGR("test: Player 1's flashlight (object %08X, item %u) is ended at frame %llu; he gets a new one in %d frames", object, item, static_cast<unsigned long long>(g_frames), gap);
        } else if (g_frames == at) {
            LOGR("test: Player 1 has no flashlight to end at frame %llu", static_cast<unsigned long long>(g_frames));
        }
    }
    if (give_at && g_frames >= give_at && item != 0xFFFF) {
        give_at = 0, done++;
        ctx.ecx = g_player;
        kernel::CallGuest(ctx, 0x153A0, {item, 1});
        uint32_t light = FlashlightOf(g_player);
        if (light) {
            ctx.ecx = light;
            kernel::CallGuest(ctx, kernel::R32(kernel::R32(light) + 0x98), {});  // (its method "on")
        }
        LOGR("test: Player 1 is given a flashlight again (item %u): %s", item, light ? "he has one, switched on" : "HE HAS NONE");
    }
    ctx.ecx = ecx, ctx.eax = eax;
}
void Flashlights() {
    {   // (once: where the game's message manager is, for the notes)
        static bool said = false;
        uint32_t m = kernel::R32(0x384B8C);
        if (!said && m && mem::Spans(m, 4) && mem::Spans(kernel::R32(m), 0x40)) {
            said = true;
            uint32_t v = kernel::R32(m);
            LOGI("coop: the message manager [0x384B8C] is %08X, its methods at %08X: +0x04 %06X, +0x10 %06X, +0x14 %06X, +0x18 %06X, +0x1C %06X, +0x20 %06X", m, v, kernel::R32(v + 4),
                 kernel::R32(v + 0x10), kernel::R32(v + 0x14), kernel::R32(v + 0x18), kernel::R32(v + 0x1C), kernel::R32(v + 0x20));
        }
    }
    if (g_trace_left_at && g_frames >= g_trace_left_at) {
        uint32_t table = kernel::R32(kObjectManager + 0x0C);
        for (const auto& [object, index] : g_trace_left) {
            bool there = table && kernel::R32(table + index * 6) == object && mem::Spans(object, 0x110) && int32_t(kernel::R32(object + 8)) >= 0;
            LOGI("held trace: object %08X (handle %X), still there right after he was taken out, is %s 90 frames later (class now %08X, flags %08X)", object, index,
                 there ? "STILL THERE" : "gone", there ? ClassOf(object) : 0, there ? kernel::R32(object + 8) : 0);
        }
        g_trace_left.clear();
        g_trace_left_at = 0;
    }
    static uint32_t before[2] = {0xFF, 0xFF};
    uint32_t players[2] = {g_player, g_second};
    for (int i = 0; i < 2; i++) {
        uint32_t light = FlashlightOf(players[i]);
        uint32_t now = light ? kernel::R32(light + 0xA8) & 7 : 0x80;
        if (now == before[i]) continue;
        if (before[i] != 0xFF || light) {
            if (!light) LOGI("coop: Player %d has no flashlight%s", i + 1, players[i] ? "" : " (he is not in)");
            else
                LOGI("coop: Player %d's flashlight is %s%s (charge %.0f s)", i + 1, now & 1 ? "on" : "off",
                     now & 4 ? ", its battery dead" : now & 2 ? ", its battery giving out" : "", ReadF(light + 0xAC));
        }
        before[i] = now;
    }
    // once a second: how many Flashlight objects there are in the level, and how many of them are carried by a
    // player (one left behind when Player 2 is taken out would stay alight where he stood)
    if (g_frames % 60 == 11) {
        static int said_all = -1, said_carried = -1;
        uint32_t table = kernel::R32(kObjectManager + 0x0C), entries = std::min<uint32_t>(kernel::R32(kObjectManager + 0x14), 0x4000);
        int all = 0, carried = 0;
        for (uint32_t i = 1; table && i < entries && mem::IsCommitted(table + i * 6 + 4); i++) {
            uint32_t object = kernel::R32(table + i * 6);
            // (a free entry holds a small number, the next free one, not an object; an object knows its own handle)
            if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
            if (kernel::R16(object + 0xE6) != i || ClassOf(object) != kFlashlightClass) continue;
            all++;
            uint32_t light = kernel::R32(object + kClassObject);
            if (light && (light == FlashlightOf(g_player) || light == FlashlightOf(g_second))) carried++;
        }
        if (all != said_all || carried != said_carried) {
            LOGI("coop: %d flashlight%s in the level, %d carried by a player", all, all == 1 ? "" : "s", carried);
            said_all = all, said_carried = carried;
            // (each of them: which object, who carries it by its own word, where it is)
            for (uint32_t i = 1; table && i < entries && mem::IsCommitted(table + i * 6 + 4); i++) {
                uint32_t object = kernel::R32(table + i * 6);
                if (object < 0x400000 || !mem::IsCommitted(object) || !mem::Spans(object, 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
                if (kernel::R16(object + 0xE6) != i || ClassOf(object) != kFlashlightClass) continue;
                uint32_t light = kernel::R32(object + kClassObject), h = light && mem::Spans(light, 0xD0) ? kernel::R16(light + 0x56) : 0;
                uint32_t owner = h ? kernel::R32(table + h * 6) : 0;
                LOGI("coop:   flashlight object %08X (handle %X), its Flashlight %08X, flags %X, carried by %s (object %08X), at %.2f %.2f %.2f", object, i, light,
                     light ? kernel::R32(light + 0xA8) & 7 : 0,
                     !owner ? "nobody" : owner == g_player_object ? "Player 1" : owner == g_second_object ? "Player 2" : owner == g_doubles[0].object ? "Player 1's stand-in" :
                     owner == g_doubles[1].object ? "Player 2's stand-in" : "another object",
                     owner, ReadF(object + kPosition), ReadF(object + kPosition + 4), ReadF(object + kPosition + 8));
            }
        }
    }
}
// Player 2 has just been given his things (Arm): his flashlight is on or off as Player 1's is
void FlashlightAsFirst(X86Context& ctx) {
    uint32_t mine = FlashlightOf(g_second), his = FlashlightOf(g_player);
    if (!mine) {
        LOGI("coop: Player 2 has no flashlight (Player 1 %s)", his ? "has one: it was not given" : "has none either");
        return;
    }
    bool kept = g_torch2 >= 0 && std::chrono::steady_clock::now() - g_torch2_at < std::chrono::seconds(120);
    bool on = (kernel::R32(mine + 0xA8) & 1) != 0, want = kept ? g_torch2 == 1 : his && (kernel::R32(his + 0xA8) & 1);
    g_torch2 = -1;
    if (on != want) {
        AsSecond as(g_second);
        uint32_t ecx = ctx.ecx, eax = ctx.eax;
        ctx.ecx = mine;
        kernel::CallGuest(ctx, kernel::R32(kernel::R32(mine) + (want ? 0x98 : 0x9C)), {});
        ctx.ecx = ecx, ctx.eax = eax;
    }
    LOGI("coop: Player 2 has his own flashlight (%s, as %s)", want ? "on" : "off", kept ? "he had it when he was taken out" : "Player 1's is");
}
}  // namespace game_coop

X86_EXTERN_FUNC(__imp__sub_000EA840);
X86_FUNC(sub_000EA840) {
    using namespace game_coop;
    uint32_t light = ctx.ecx;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = mem::Spans(light, 0xD0) ? kernel::R16(light + 0x56) : 0;
    uint32_t owner = table && h ? kernel::R32(table + h * 6) : 0;
    bool second = owner && g_second_object && owner == g_second_object;
    bool players = owner && (owner == g_player_object || second);
    static const bool battery = std::getenv("S2_TORCH_BATTERY") != nullptr;
    if (players && !battery) {
        uint32_t flags = kernel::R32(light + 0xA8);
        if ((flags & 1) && (flags & 6)) {  // on, and its battery has given out: a new one (what the game does with a spare)
            AsSecond as(second ? g_second : 0);
            uint32_t eax = ctx.eax;
            kernel::CallGuest(ctx, 0xEA550, {});  // (this = the Flashlight, still in ecx)
            ctx.ecx = light, ctx.eax = eax;
            static int said = 0;
            if (said++ < 4) LOGI("coop: Player %d's flashlight had a battery that gave out: a new one is put in (the battery is endless)", second ? 2 : 1);
        } else if (!(flags & 6)) {
            float full = PropertyF(ctx, kernel::R32(light + 4), 0x4C, 0.0f);
            if (full > 0 && ReadF(light + 0xAC) < full) std::memcpy(kernel::HostPtr(light + 0xAC), &full, 4);
        }
    }
    AsSecond as(second ? g_second : 0);
    __imp__sub_000EA840(ctx, base);
}

// ---- Player 2's shots go where he aims (the owner, 2026-11-28: "player 2's bullets are shooting behind him").
// A ranged weapon fires (0xF1E70, this = the weapon's own part: its owner's handle at +0x56, the weapon at +4) and
// throws out its shell (0xF1D20) when its Fire message comes, from the objects' run, not inside its owner's own
// update: the place and way of the shot came from "the player" and the game's camera, which are Player 1's then,
// so Player 2's shots started on Player 1's side of him and flew where Player 1 looked. When the weapon is Player
// 2's, it fires as his own update does: he first in the player list, his Camera the game's, his view current.
namespace game_coop {
uint32_t WeaponShooter(uint32_t part) {
    if (!mem::Spans(part, 0x58)) return 0;
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = kernel::R16(part + 0x56);
    uint32_t owner = table && h && h < kernel::R32(kObjectManager + 0x14) ? kernel::R32(table + h * 6) : 0;
    return owner && g_second_object && owner == g_second_object ? g_second : 0;
}
}  // namespace game_coop
// ---- an object with no model is not drawn (the owner's version 70 crashed in 0x1A9CF0, the draw of one object:
// it reads the object's model at +0x68 and calls through it, and the object being drawn had none, just after the
// pause menu closed with Player 2 coming back up). The draw is left out and the object named in the log.
// ---- Player 2 does not change form by his own button for now (2026-11-28). In the owner's session 31 Player 2's
// creature was drawn broken (the creature with Torque's things on it) and its attack crashed the game: 0x23C70, his
// state function, read through a null at 0x24291 (property 0x127 of his body gave no template). Until his creature
// is made right, the button's change (0x1C21E: state 18, Morph, through 0x25CD0) is refused for him; the story's
// changes (StoryForm, StoryHold) still go through. S2_P2_CREATURE=1 lets his button through (tests).
X86_EXTERN_FUNC(__imp__sub_00025CD0);
X86_FUNC(sub_00025CD0) {
    static const bool allow = std::getenv("S2_P2_CREATURE") != nullptr;
    if (!allow && game_coop::g_second && ctx.ecx == game_coop::g_second && kernel::R32(ctx.esp + 4) == 0x12 && !game_coop::g_story_form_call &&
        !(kernel::R32(game_coop::g_second + 0xBC) & 0x80000)) {
        static int said = 0;
        if (said < 5) said++, LOGI("coop: Player 2's own change of form is refused (his creature is not ready yet)");
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_00025CD0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001A9CF0);
X86_FUNC(sub_001A9CF0) {
    uint32_t object = ctx.ecx;
    uint32_t model = mem::Spans(object, 0x104) ? kernel::R32(object + 0x68) : 0;
    if (!model || !mem::Spans(model, 0x28)) {
        static int said = 0;
        if (said < 10) said++, LOGR("draw: an object with no model is left out (%08X, %s, model %08X)", object,
                                    mem::Spans(object, 0x114) ? game_coop::WhoIs(object).c_str() : "not there", model);
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_001A9CF0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000F1E70);
X86_FUNC(sub_000F1E70) {
    game_coop::AsSecond as(game_coop::WeaponShooter(ctx.ecx));
    __imp__sub_000F1E70(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000F1D20);
X86_FUNC(sub_000F1D20) {
    game_coop::AsSecond as(game_coop::WeaponShooter(ctx.ecx));
    __imp__sub_000F1D20(ctx, base);
}

// Tests: S2_SPAWN_TRACE=<class id in hex> logs every object of that class the game makes from a template
// (0x18C370: owner, matrix, -, template), with the place and who asked. (Who makes the short pale flash at a hit:
// a Screen Fader, class 21C015A3.)
X86_EXTERN_FUNC(__imp__sub_0018C370);
X86_FUNC(sub_0018C370) {
    static const uint32_t want = std::getenv("S2_SPAWN_TRACE") ? uint32_t(std::strtoul(std::getenv("S2_SPAWN_TRACE"), nullptr, 16)) : 0;
    // (S2_SPAWN_TRACE=all with S2_SPAWN_FROM=<frame>: everything made in the sixty frames from that one)
    static const bool all = std::getenv("S2_SPAWN_TRACE") && !std::strcmp(std::getenv("S2_SPAWN_TRACE"), "all");
    static const uint64_t from = std::getenv("S2_SPAWN_FROM") ? uint64_t(std::atoll(std::getenv("S2_SPAWN_FROM"))) : 0;
    static const uint64_t span = std::getenv("S2_SPAWN_FOR") ? uint64_t(std::atoll(std::getenv("S2_SPAWN_FOR"))) : 60;
    if (all && game_coop::Frames() >= from && game_coop::Frames() < from + span) {
        uint32_t owner = kernel::R32(ctx.esp + 4), matrix = kernel::R32(ctx.esp + 8), tmpl = kernel::R32(ctx.esp + 16);
        float at[3] = {0, 0, 0};
        float way[3] = {0, 0, 0};
        if (matrix && mem::Spans(matrix, 64)) std::memcpy(at, kernel::HostPtr(matrix + 48), 12), std::memcpy(way, kernel::HostPtr(matrix + 32), 12);
        LOGI("spawn trace: way %.2f %.2f %.2f", way[0], way[1], way[2]);
        LOGI("spawn trace: frame %llu: template %u of class %08X at %.2f %.2f %.2f, owner %s (class %08X), asked from %s", (unsigned long long)game_coop::Frames(), tmpl,
             tmpl < 0x10000 ? game_coop::ClassOfTemplate(tmpl) : 0, at[0], at[1], at[2], owner && mem::Spans(owner, 0x110) ? game_coop::WhoIs(owner).c_str() : "none",
             owner && mem::Spans(owner, 0x110) ? game_coop::ClassOf(owner) : 0, game_coop::Callers(ctx, 10).c_str());
    } else if (want && !all) {
        uint32_t owner = kernel::R32(ctx.esp + 4), matrix = kernel::R32(ctx.esp + 8), tmpl = kernel::R32(ctx.esp + 16);
        if (tmpl < 0x10000 && game_coop::ClassOfTemplate(tmpl) == want) {
            float at[3] = {0, 0, 0};
            if (matrix && mem::Spans(matrix, 64)) std::memcpy(at, kernel::HostPtr(matrix + 48), 12);
            LOGI("spawn trace: frame %llu: template %u of class %08X at %.2f %.2f %.2f, owner %08X (class %08X), inside the method of Player %08X, asked from %s",
                 (unsigned long long)game_coop::Frames(), tmpl, want, at[0], at[1], at[2], owner,
                 owner && mem::Spans(owner, 0x110) ? game_coop::ClassOf(owner) : 0, game_coop::g_running, game_coop::Callers(ctx, 14).c_str());
        }
    }
    __imp__sub_0018C370(ctx, base);
}

// ---- the messages a stand-in body sends ------------------------------------------------------------------------------
// Things happen in the game through messages: 0x1D7110 (this = the message manager 0x384BE8, its methods at 0x348A10,
// +0x18; from, to, the message, a number, flags) hands one object's message to another (0x1D6C60), and with flag 0x10
// to what hangs on it. A message is a number (its names are in work\re\riot_messages.tsv) or a record that starts with
// one. A Player's Attack state fires his weapon by sending his own object "Fire" (0x66, at 0x3260B).
//   S2_MESSAGE_TRACE=1: every kind of message a stand-in's object, or what it holds, sends or is sent (tests)
X86_EXTERN_FUNC(__imp__sub_001D7110);
X86_FUNC(sub_001D7110) {
    using namespace game_coop;
    uint32_t from = kernel::R32(ctx.esp + 4), to = kernel::R32(ctx.esp + 8), message = kernel::R32(ctx.esp + 12);
    uint32_t id = message > 3 && mem::Spans(message, 4) ? kernel::R32(message) : message;
    int sender = -1, taker = -1;
    for (int which = 0; which < 2; which++) {
        const Double& d = g_doubles[which];
        if (!d.object) continue;
        if (OfDouble(d, from)) sender = which;
        if (OfDouble(d, to)) taker = which;
    }
    static const bool trace = std::getenv("S2_MESSAGE_TRACE") != nullptr;
    if (trace && (sender >= 0 || taker >= 0)) {
        static std::map<uint64_t, int> seen;
        uint32_t cf = from && mem::Spans(from, 0x110) ? ClassOf(from) : 0, ct = to && mem::Spans(to, 0x110) ? ClassOf(to) : 0;
        uint64_t key = (uint64_t(id) << 40) ^ (uint64_t(cf) << 8) ^ ct ^ (uint64_t(sender + 1) << 62) ^ (uint64_t(taker + 1) << 60);
        if (seen.size() < 300 && !seen[key]++)
            LOGI("message trace: frame %llu: message %X from %s (class %08X) to %s (class %08X), number %X, flags %X, inside the method of Player %08X, from %s",
                 (unsigned long long)g_frames, id, sender >= 0 ? (from == g_doubles[sender].object ? "a stand-in" : "what a stand-in holds") : "another object", cf,
                 taker >= 0 ? (to == g_doubles[taker].object ? "a stand-in" : "what a stand-in holds") : "another object", ct, kernel::R32(ctx.esp + 16),
                 kernel::R32(ctx.esp + 20), g_running, Callers(ctx, 5).c_str());
    }
    // A stand-in's weapon fires blanks: the shots are its owner's own, from the weapon in his first-person hands
    // (their flash, their sound, their bullets). "Fire" (0x66) is not handed to a stand-in or to what it holds: the
    // Attack state sends it to the Player's own object when it begins (0x31F3F) and the weapon's animation sends it
    // to the weapon for every round. With it the target was hit twice for every shot (run ds4: 11 hits in a burst of
    // five or six). And nothing a stand-in sends hurts anybody: the damage message 0x2001 from one is dropped too.
    static const bool live = std::getenv("S2_DOUBLE_LIVE") != nullptr;  // tests: as it was
    // The engine's own messages (0x2000 on: 0x2001 the hurt, 0x2005 a blow's touch, 0x2010 "you hit me") from a
    // stand-in or from what it holds go to nobody but itself: its second attack met its own owner, who stands in
    // the same place (run dm1: Player 1 hurt 32.5 by his own stand-in).
    // ... and none of them reaches a stand-in from anybody else: its owner's own blow in first person met it (he
    // stands inside it) and blood sprang from it at every swing (run da1).
    bool its_own = sender >= 0 && taker == sender;
    if (!live && ((taker >= 0 && id == 0x66) || ((sender >= 0 || taker >= 0) && !its_own && id >= 0x2000 && id < 0x3000))) {
        static int said = 0;
        if (said++ < 4) LOGI("coop: a stand-in body does nothing to the world: its message %X is not handed on (its weapon fires blanks, its blows meet nobody)", id);
        kernel::Return(ctx, 0, 5);
        return;
    }
    __imp__sub_001D7110(ctx, base);
}

// A stand-in body's throw is for show (game_coop::DoubleTakesInHand has the game's three steps): its "the thing is
// taken in hand" (0x32E40) and its "the thing is let go" (0x30FD0; the throw state's end calls it too) are done here
// without the inventory, the next grenade and the flight; and the thing to throw it is handed brings no HUD part
// with it (0x1E3E0, this = Player; the item: the part goes to Player +0x7E4 and onto the screen).
//   S2_DOUBLE_LIVE=1: as the game does it for any Player (tests)
namespace game_coop {
void ThrowTrace(uint32_t player, const char* what) {  // S2_DOUBLE_TRACE=1: the frame of each step of anybody's throw
    static const bool trace = std::getenv("S2_DOUBLE_TRACE") != nullptr;
    static int said = 0;
    if (!trace || said++ > 200) return;
    int its = DoubleOf(player);
    LOGI("double trace: frame %llu: %s%d%s %s", (unsigned long long)g_frames, its >= 0 ? "the stand-in of Player " : "Player ", its >= 0 ? its + 1 : player == g_second ? 2 : 1,
         its < 0 && player != g_player && player != g_second ? " (?)" : "", what);
}
}  // namespace game_coop
X86_EXTERN_FUNC(__imp__sub_00032E40);
X86_FUNC(sub_00032E40) {
    static const bool live = std::getenv("S2_DOUBLE_LIVE") != nullptr;
    game_coop::ThrowTrace(ctx.ecx, "takes the thing to throw in hand (message E7)");
    if (int which = game_coop::DoubleOf(ctx.ecx); which >= 0 && !live) {
        game_coop::DoubleTakesInHand(ctx, which, kernel::R32(ctx.esp + 4));
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_00032E40(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00030FD0);
X86_FUNC(sub_00030FD0) {
    static const bool live = std::getenv("S2_DOUBLE_LIVE") != nullptr;
    game_coop::ThrowTrace(ctx.ecx, "lets go (message E6, or the throw's end)");
    if (int which = game_coop::DoubleOf(ctx.ecx); which >= 0 && !live) {
        game_coop::DoubleLetsGo(ctx, which, kernel::R32(ctx.esp + 4));
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_00030FD0(ctx, base);
}
// The pace of a stand-in's throw. A Player starts a movement with 0x259B0 (this = Player; the movement, its channel,
// three numbers, two flags; the fifth argument is the pace, 1 everywhere in the throw), and the throw's state does so
// when it begins (0x304A0) and whenever he turns (0x30650). In first person the throw's movement is a shorter one:
// measured from the state's begin, he takes the thing in hand after 5 frames and lets go after 29, his stand-in in
// third person after 8 and after 44. So that both let go at once, the stand-in's goes at 1.5 times the pace.
//   S2_DOUBLE_THROW_PACE=<number>: another pace (tests; 1 = the game's own)
namespace game_coop {
int g_double_throw = 0;  // a stand-in's throw state is at work (its begin or its update)
}
#define COOP_THROW_STATE(addr)                                                     X86_EXTERN_FUNC(__imp__sub_##addr);                                            X86_FUNC(sub_##addr) {                                                             bool its = game_coop::DoubleOf(ctx.ecx) >= 0;                                  if (its) game_coop::g_double_throw++;                                          __imp__sub_##addr(ctx, base);                                                  if (its) game_coop::g_double_throw--;                                      }
COOP_THROW_STATE(000304A0)
COOP_THROW_STATE(00030650)
X86_EXTERN_FUNC(__imp__sub_000259B0);
X86_FUNC(sub_000259B0) {
    if (game_coop::g_double_throw > 0 && game_coop::DoubleOf(ctx.ecx) >= 0) {
        static const float pace = std::getenv("S2_DOUBLE_THROW_PACE") ? float(std::atof(std::getenv("S2_DOUBLE_THROW_PACE"))) : 1.5f;
        float asked = game_coop::ReadF(ctx.esp + 0x14);
        if (asked > 0 && pace > 0.1f && pace < 4.0f) {
            float now = asked * pace;
            std::memcpy(kernel::HostPtr(ctx.esp + 0x14), &now, 4);
        }
    }
    __imp__sub_000259B0(ctx, base);
}
// A crouching Player's body is made lower: 0x3B0C0 (this = Player; 1 "crouching", 0 "upright") switches the parts
// of his body that his property 0xAD lists (the body at +0xF0, its parts at body +0x122, 0x3C bytes each, found by
// 0x199790). A stand-in's body has no parts (they come with the Collider, which is made in a Player's Detect, and a
// stand-in has none): 0x199790 says -1, and for "upright" the routine writes through that all the same, 0x3C bytes
// before address 0. The game stopped with that the moment a first-person player crouched (runs thr3 and thr5,
// 2026-11-27: the stand-in crouches with him since it reads his crouch button). Nothing is switched for a stand-in.
X86_EXTERN_FUNC(__imp__sub_0003B0C0);
X86_FUNC(sub_0003B0C0) {
    if (game_coop::DoubleOf(ctx.ecx) >= 0) {
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_0003B0C0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0001E3E0);
X86_FUNC(sub_0001E3E0) {
    if (game_coop::g_double_spawning || game_coop::DoubleOf(ctx.ecx) >= 0) {
        kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_0001E3E0(ctx, base);
}

// A weapon's blow meets something (0xEFE9D, for each thing the weapon touched: this = the weapon, its carrier's handle
// at +0x56). Three things are done with it, each given the object met first: 0xEF320 lands the blow (the record of
// the hit, 0x28280, handed to what was met: its health part's method +0x58, for a Player the hurt, the flash and the
// hit's reaction; then the touch message 0x2005), 0xEF6B0 gives it its push, and 0xEEFA0 makes what the blow throws
// up (an Object Generator, template 1698 for a body: blood). A stand-in body is not there for blows: not for its own
// owner's, who stands inside it (in first person his second attack met his own stand-in at every swing: blood sprang
// from it and the grenade on its belt flew off, runs da2 and da5), not for anybody else's; and its own blows meet
// nothing.
namespace game_coop {
bool BlowIsForShow(X86Context& ctx) {
    uint32_t weapon = ctx.ecx, met = kernel::R32(ctx.esp + 4);
    uint32_t table = kernel::R32(kObjectManager + 0x0C), h = weapon && mem::Spans(weapon, 0x60) ? kernel::R16(weapon + 0x56) : 0;
    uint32_t carrier = table && h ? kernel::R32(table + h * 6) : 0;
    static const bool live = std::getenv("S2_DOUBLE_LIVE") != nullptr;  // tests: as it was
    if (live || !(DoublesThing(met) || DoubleObject(carrier) >= 0)) return false;
    static int said = 0;
    if (said++ < 4) LOGI("coop: a blow %s: nothing comes of it", DoubleObject(carrier) >= 0 ? "of a stand-in body's" : "met a stand-in body");
    return true;
}
}  // namespace game_coop
#define COOP_BLOW(addr, words)                                                                                            \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                                                                   \
    X86_FUNC(sub_##addr) {                                                                                                \
        if (game_coop::BlowIsForShow(ctx)) {                                                                              \
            kernel::Return(ctx, 0, words);                                                                                \
            return;                                                                                                       \
        }                                                                                                                 \
        __imp__sub_##addr(ctx, base);                                                                                     \
    }
COOP_BLOW(000EF320, 4)
COOP_BLOW(000EF6B0, 3)
COOP_BLOW(000EEFA0, 5)
