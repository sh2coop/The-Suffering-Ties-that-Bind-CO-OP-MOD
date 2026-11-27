// Display settings and co-op in the game's own Options menu.
//
// The Options menu has six entries on its paper strip and no room for a seventh, so the "Brightness Setting" entry
// becomes "Display and Co-op": the game's own page (the brightness slider with its picture and instructions) gets
// rows under the slider, made of the game's own text widgets in the page's own style, one section at a time:
//     Display   Resolution (every size the monitor offers, with its shape, and "Desktop"), Window mode (Full
//               screen, Borderless, Windowed), Picture shape (Automatic, 4:3, 16:10, 16:9, 21:9, 32:9), Frame rate
//               (60, 120, the monitor's, no limit), VSync, HD textures (Off, Automatic, 2x, 4x: the set made on this
//               PC by tools\launcher\hdtex.h; switching it on the first time starts the making)
//     Co-op     Co-op (Off, Split screen), Player 1 look and Player 2 look (Normal or Inverted, each player's own),
//               Player 2 look speed
//     Sound     HD sound (Off, On: the set made on this PC by tools\launcher\hdsound.h; switching it on the first time
//               starts the making, src\game\hdsound.cpp)
// Up and down move between the slider and the rows, left and right (or A) change the row's value, and a change
// takes effect at once and is saved. Before a new game starts the game shows its own brightness page, without
// these rows (since 2026-11-26: the constructor's hook, below). The main menu's "Co-op" entry opens the page at its
// Co-op section; Player 2's pause menu opens it as his own page, with his two rows only.
// (Online co-op was here until 2026-11-27, when the owner dropped it: "I want to abandon online co-op entirely".)
//
// How the game's menus work (read from the code; xbox\NOTES.md has the longer account):
//   - A screen is a widget (base constructor 0x13F010(template)), its children at +0x50 (array), +0x58 (count);
//     +0x0C template handle, +0x24 state (2 hidden, 4 idle, 5 highlighted), +0x38 x, +0x3C y, +0x40 width, +0x44
//     height, in 640 x 480 units. Method +0x38 sets the state, +0x14 takes an input event, +0x5C runs once a frame.
//   - A text widget (0xBC bytes, constructor 0x140450(template), methods 0x340E40) shows a string: 0x13F610(wide
//     string) sets it, 0x1403E0(id) sets one of the game's own (the id stays at +0x80; the characters are at +0x68,
//     their count and one at +0x6C). 0x13C900(child) adds a widget to a screen.
//   - The Brightness screen: constructor 0x14BF00 (methods 0x341850), slider at +0x68, input 0x14BDA0, once a frame
//     0x14C340, destructor 0x14BEE0. Input events: 0x40A up, 0x40B down, 0x40C left, 0x40D right, 0x41A accept (A,
//     START), 0x421 back (B, BACK), 0x450 (X). Up and down do nothing on the original page.
//   - The Options menu: constructor 0x14AE60, the Brightness entry's button at +0x7C.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game_view {
extern uint32_t g_shape_setting;  // width * 1000 / height of the picture, 0 for the output's own shape (view.cpp)
}

namespace game_coop {             // coop.cpp
extern uint32_t g_mode_setting;   // 0 off, 1 split screen (kept in the settings file as "coop")
extern uint32_t g_p2_invert, g_p2_speed;  // Player 2's own look: inverted or not, and his speed in per cent
int Pauser();  // 0 not paused, 1 or 2: whose pause menu is up
std::string CallersOf(X86Context& ctx);  // (traces)
}
namespace game_hdsound {  // hdsound.cpp
extern uint32_t g_setting;  // settings.toml hd_sound
void Apply();
int State(int* made, int* total);  // 0 off, 1 on, 2 on and being made
}
namespace gpu::texturedumper {
bool StartHdMaker();  // texturedumper.cpp: Build Recomp's HD textures maker, in the background
}
namespace game_menu {
extern uint32_t g_invert_pitch;  // Player 1's Invert Pitch Axis, kept between runs (KeepInvert, below)
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kAlloc = 0x229E10, kTextCtor = 0x140450, kAddChild = 0x13C900, kSetText = 0x13F610, kSetTextId = 0x1403E0;
constexpr uint32_t kTextMethods = 0x340E40;
constexpr uint32_t kChildren = 0x50, kCount = 0x58, kTemplate = 0x0C, kState = 0x24, kX = 0x38, kY = 0x3C, kW = 0x40, kH = 0x44;
constexpr uint32_t kString = 0x68, kStringSize = 0x6C, kTextId = 0x80;
constexpr uint32_t kSlider = 0x68, kBrightnessEntry = 0x7C;
constexpr uint32_t kSetState = 0x38;
constexpr uint32_t kUp = 0x40A, kDown = 0x40B, kLeft = 0x40C, kRight = 0x40D, kAccept = 0x41A, kBack = 0x421;
constexpr uint32_t kHidden = 2, kIdle = 4, kHighlighted = 5;

// The rows. The page shows one section at a time (the first row chooses it: Display, or Co-op), so that it is
// never crowded: a section has at most five rows under its heading, 20 apart. Player 2's own page (he opened it
// from his pause menu) has his two rows and nothing else.
enum Row { kSection, kResolution, kWindow, kShape, kRate, kVsync, kHd, kCoop, kLook1, kLook, kSpeed, kHdSound, kRows };
int g_section = 0;  // 0 Display, 1 Co-op, 2 Sound
constexpr int kSections = 3;
constexpr const char* kSectionNames[kSections] = {"Display", "Co-op", "Sound"};
enum Coop { kOff, kSplit, kCoops };

struct Page {
    uint32_t screen = 0, slider = 0;
    uint32_t caption = 0;  // "Brightness" over the slider: it shows when the slider is the control in focus
    uint32_t words = 0;    // the page's instructions; help for the co-op rows while one of them is in focus
    uint32_t words_id = 0;            // the game's own text in them (a string id), to put back,
    std::vector<uint16_t> words_own;  // or its characters when it has no id
    uint32_t rows[kRows] = {};
    std::string shown[kRows];  // what each row says now
    float slot_top = 0, slot_step = 0, margin = 0;  // where the visible rows go, one after another
    float row_y[kRows] = {};
    uint32_t title = 0;
    bool seconds = false;      // laid out as Player 2's own page
    std::string title_text;    // what the title says now
    uint32_t prompt = 0;       // the button line ("A Accept, B Cancel")
    bool plain = false;        // no slider: Player 2's own page
    std::string help;          // what the instructions say now (empty: the game's own text)
    int focus = 0;  // 0 the slider, 1.. the rows
};
Page g_page;
uint32_t g_scratch = 0;  // guest memory for a string handed to the game
constexpr uint32_t kScratchChars = 400;

uint32_t ThisCall(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}
uint32_t Method(X86Context& ctx, uint32_t self, uint32_t slot, std::initializer_list<uint32_t> args) {
    return ThisCall(ctx, kernel::R32(kernel::R32(self) + slot), self, args);
}
float ReadF(uint32_t at) {
    float f;
    std::memcpy(&f, kernel::HostPtr(at), 4);
    return f;
}
void WriteF(uint32_t at, float f) { std::memcpy(kernel::HostPtr(at), &f, 4); }

// a template's property by index: the game's own lookup (object table at [0x384BA4], 0x113E0(handle) gives the
// object, its property bag at +4, the bag's method +8 (index, -1) gives the value's address)
uint32_t Property(X86Context& ctx, uint32_t handle, uint32_t index) {
    uint32_t object = ThisCall(ctx, 0x113E0, kernel::R32(0x384BA4), {handle});
    if (!object) return 0;
    uint32_t bag = kernel::R32(object + 4);
    return bag ? Method(ctx, bag, 8, {index, 0xFFFFFFFFu}) : 0;
}

void SetWide(X86Context& ctx, uint32_t widget, const uint16_t* text, size_t count) {
    if (!g_scratch) g_scratch = mem::AllocSystem((kScratchChars + 1) * 2);
    uint32_t n = uint32_t(std::min<size_t>(count, kScratchChars));
    for (uint32_t i = 0; i < n; i++) kernel::W16(g_scratch + i * 2, text[i]);
    kernel::W16(g_scratch + n * 2, 0);
    ThisCall(ctx, kSetText, widget, {g_scratch});
}
void SetText(X86Context& ctx, uint32_t widget, const std::string& text) {
    std::vector<uint16_t> wide(text.size());
    for (size_t i = 0; i < text.size(); i++) wide[i] = uint8_t(text[i]);
    SetWide(ctx, widget, wide.data(), wide.size());
}

// ---- the settings behind the rows ---------------------------------------------------------------------------

struct Size {
    uint32_t w, h;
    bool operator<(const Size& o) const { return w != o.w ? w < o.w : h < o.h; }
    bool operator==(const Size& o) const { return w == o.w && h == o.h; }
};

// every size the game window's monitor offers, smallest first; the desktop's own is {0, 0} at the front
const std::vector<Size>& Sizes() {
    static std::vector<Size> sizes;
    if (!sizes.empty()) return sizes;
    std::vector<Size> found;
#ifdef _WIN32
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
            if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) found.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    }
#endif
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    sizes.push_back({0, 0});
    sizes.insert(sizes.end(), found.begin(), found.end());
    return sizes;
}

// "16:9" and the like for a size; the common names first, then the reduced fraction
std::string ShapeName(uint32_t w, uint32_t h) {
    if (!w || !h) return "";
    static const struct {
        float ratio;
        const char* name;
    } known[] = {{4.0f / 3, "4:3"}, {5.0f / 4, "5:4"}, {3.0f / 2, "3:2"}, {16.0f / 10, "16:10"}, {16.0f / 9, "16:9"},
                 {21.0f / 9, "21:9"}, {2.4f, "21:9"}, {32.0f / 9, "32:9"}, {17.0f / 9, "17:9"}};
    float r = float(w) / float(h);
    for (const auto& k : known)
        if (std::fabs(r - k.ratio) < 0.035f) return k.name;
    uint32_t a = w, b = h;
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    char out[32];
    std::snprintf(out, sizeof out, "%u:%u", w / a, h / a);
    return out;
}

constexpr uint32_t kShapes[] = {0, 1333, 1600, 1778, 2370, 3556};
constexpr const char* kShapeNames[] = {"Automatic", "4:3", "16:10", "16:9", "21:9", "32:9"};
constexpr uint32_t kRates[] = {60, 120, app::kFpsCapRefresh, 0};
constexpr const char* kRateNames[] = {"60", "120", "Monitor's", "No limit"};
constexpr const char* kWindowNames[] = {"Full screen", "Borderless", "Windowed"};
constexpr const char* kCoopNames[kCoops] = {"Off", "Split screen"};

template <typename T, size_t N>
int IndexOf(const T (&list)[N], T value) {
    for (size_t i = 0; i < N; i++)
        if (list[i] == value) return int(i);
    return 0;
}

// ---- co-op: the choice ------------------------------------------------------------------------------------------

// The Co-op row's choice: the saved setting. Split screen takes effect at once (coop.cpp: Player 2 comes in when a
// level is in play, and is taken out when it is switched off).
int CoopChoice() { return game_coop::g_mode_setting == 1 ? kSplit : kOff; }
void SetCoop(int choice) { game_coop::g_mode_setting = choice == kSplit ? 1 : 0; }

// ---- the look of each player ----------------------------------------------------------------------------------------
// The game has one "Invert Pitch Axis" (Options, Controller Settings) and applies it to every Player (the Pitch
// state's update, 0x2D460, turns the stick's value round when bit 0x10 of the options' switches is set). It is
// Player 1's: the Player 1 look row changes it, as the game's own page does, and it is kept between runs
// (KeepInvert, invert_pitch). Player 2's is his own (p2_look_inverted): coop.cpp turns his stick round when his
// choice and the game's differ, so the game's setting does not change him. ("both players need to be able to
// change inverted options", the owner, 2026-11-27.)
constexpr uint32_t kOptions = 0x392158, kSwitches = kOptions + 0x50, kInverted = 0x10, kOptionsMethods = 0x347C44;
bool GameInverted() {
    return mem::IsCommitted(kSwitches) && kernel::R32(kOptions) == kOptionsMethods && (kernel::R32(kSwitches) & kInverted) != 0;
}
void SetGameInverted(bool on) {
    g_invert_pitch = on ? 1 : 0;  // (first: KeepInvert puts back any other value than this one)
    if (!mem::IsCommitted(kSwitches) || kernel::R32(kOptions) != kOptionsMethods) return;
    uint32_t switches = kernel::R32(kSwitches);
    kernel::W32(kSwitches, on ? switches | kInverted : switches & ~kInverted);
}

// ---- the HD textures (tools\launcher\hdtex.h writes textures\hd beside the game: version.txt once it has started,
// progress.txt "made total" as it goes, made.txt when the disc's textures are all made) -------------------------------
struct HdState {
    bool started = false, whole = false;
    int made = 0, total = 0;
};
HdState ReadHd() {
    static HdState last;
    static Clock::time_point when;
    if (when != Clock::time_point{} && Clock::now() - when < std::chrono::seconds(1)) return last;
    when = Clock::now();
    HdState st;
    std::error_code ec;
    std::filesystem::path dir = app::ExeDir() / "textures" / "hd";
    st.started = std::filesystem::exists(dir / "version.txt", ec);
    st.whole = std::filesystem::exists(dir / "made.txt", ec);
    if (FILE* f = _wfopen((dir / "progress.txt").c_str(), L"r")) {
        if (std::fscanf(f, "%d %d", &st.made, &st.total) != 2) st.made = st.total = 0;
        std::fclose(f);
    }
    last = st;
    return st;
}
// the set the game draws now, as texturedumper.cpp chooses it (HdScale)
const char* HdNow(uint32_t setting) {
    if (setting == 2) return "2x";
    if (setting == 3) return "4x";
    return app::Output().height > 1080 ? "4x" : "2x";
}

// ---- what the rows say -----------------------------------------------------------------------------------------

// Player 2 opened this page from his own pause menu: it is his settings page, and only his rows are on it
bool SecondsPage() { return game_coop::Pauser() == 2; }

std::string Value(int row) {
    const app::Settings& s = app::Get();
    char out[96];
    switch (row) {
        case kResolution: {
            app::Mode m = s.width && s.height ? app::Mode{s.width, s.height} : app::Desktop();
            std::snprintf(out, sizeof out, "%s%u x %u (%s)", s.width && s.height ? "" : "Desktop ", m.width, m.height,
                          ShapeName(m.width, m.height).c_str());
            return out;
        }
        case kWindow: return kWindowNames[std::min<uint32_t>(uint32_t(s.display_mode), 2)];
        case kShape: return kShapeNames[IndexOf(kShapes, game_view::g_shape_setting)];
        case kRate: return kRateNames[IndexOf(kRates, s.fps_cap)];
        case kVsync: return s.vsync ? "On" : "Off";
        case kHd: {
            if (!s.hd_textures || !ReadHd().started) return "Off";
            if (s.hd_textures == 1) return std::string("Automatic (") + HdNow(1) + ")";
            return s.hd_textures == 2 ? "2x" : "4x";
        }
        case kCoop: return kCoopNames[CoopChoice()];
        case kSection: return kSectionNames[std::clamp(g_section, 0, kSections - 1)];
        case kHdSound: {
            int made, total;
            int st = game_hdsound::State(&made, &total);
            if (!st) return "Off";
            if (st == 2 && total) {
                std::snprintf(out, sizeof out, "On (being made, %d%%)", made * 100 / total);
                return out;
            }
            return st == 2 ? "On (being made)" : "On";
        }
        case kLook1: return GameInverted() ? "Inverted" : "Normal";
        case kLook: return game_coop::g_p2_invert ? "Inverted" : "Normal";
        case kSpeed:
            std::snprintf(out, sizeof out, "%u%%", std::clamp<uint32_t>(game_coop::g_p2_speed, 40, 100));
            return out;
    }
    return "";
}

constexpr const char* kLabels[kRows] = {"Settings", "Resolution", "Window mode", "Picture shape", "Frame rate", "VSync", "HD textures", "Co-op", "Player 1 look", "Player 2 look", "Player 2 look speed", "HD sound"};

// What is on the page now: Player 2's own page, his two rows; otherwise the section's rows under the row that
// chooses the section.
void MoveFocusTo(int focus);
bool Visible(int row) {
    if (SecondsPage()) return row == kLook || row == kSpeed;
    switch (row) {
        case kSection: return true;
        case kResolution: case kWindow: case kShape: case kRate: case kVsync: case kHd: return g_section == 0;
        case kCoop: case kLook1: case kLook: case kSpeed: return g_section == 1;
        case kHdSound: return g_section == 2;
    }
    return false;
}
bool Focusable(int) { return true; }

std::string RowText(int row, bool focused) {
    std::string v = Value(row);
    if (row == kSection) return focused ? "<  " + v + " settings  >" : v + " settings";
    return std::string(kLabels[row]) + (focused ? ":  < " : ":  ") + v + (focused ? " >" : "");
}

// What the page's instructions say while a co-op row is in focus (empty: the game's own words about the slider).
// The box holds five lines of about thirty characters.
std::string Help() {
    int row = g_page.focus - 1;
    if (SecondsPage() && row != kLook && row != kSpeed)
        return "Player 2's own settings. Go down to change how you look around. Nothing here changes Player 1.";
    switch (row) {
        case kShape:
            return game_coop::g_mode_setting == 1 ? "The picture's shape. Split screen is on: the picture fills the whole screen whatever is chosen here."
                                                  : "The picture's shape. Automatic is the screen's own; another leaves bars at the sides or above and below.";
        case kSection:
            if (g_section == 2) return "Sound: HD sound. Left or right goes to the other settings.";
            return g_section ? "Co-op: two players on this PC. Left or right goes to the other settings."
                             : "Display: resolution, window, picture shape and frame rate. Left or right goes to the other settings.";
        case kCoop:
            return "Split screen: two players on this PC, side by side. Player 2 uses the second controller.";
        case kHd: {
            HdState hd = ReadHd();
            const app::Settings& s = app::Get();
            char text[200];
            if (!hd.started || !s.hd_textures)
                return "Sharper textures, upscaled from your own game on this PC. Right turns them on (the first time they are made in the background).";
            if (!hd.whole && hd.total) {
                std::snprintf(text, sizeof text, "Being made in the background: %d of %d. The ones you see come first and show up as they are done.", hd.made, hd.total);
                return text;
            }
            return "Automatic: 2x up to 1080p, 4x above (1440p, 4K). 2x and 4x always use that set. Off shows the game's own textures.";
        }
        case kHdSound: {
            int made, total;
            int st = game_hdsound::State(&made, &total);
            char text[200];
            if (!st)
                return "Clearer sound: the voices and effects recorded at low rates get their treble back. Right turns it on (the first time it is made, in about a minute).";
            if (st == 2) {
                std::snprintf(text, sizeof text, "Being made in the background: %d of %d sounds. The ones made already play in HD.", made, total);
                return total ? text : "Being made in the background. The sounds made already play in HD.";
            }
            return "On: the treble of the low-rate sounds is made back and every sound is resampled smoothly. Off plays them as the console did.";
        }
        case kLook1: return "Player 1's up and down: Inverted turns the right stick round. It is the game's Invert Pitch Axis.";
        case kLook: return "Player 2's up and down: Inverted turns his right stick round. It only changes Player 2.";
        case kSpeed: return "How fast Player 2 turns, as a share of the game's own look speed. It only changes Player 2.";
    }
    return "";
}

// one step left or right through a row's values; the change is applied and saved
void Step(int row, int dir) {
    app::Settings& s = app::Get();
    auto wrap = [dir](int i, int n) { return (i + dir + n) % n; };
    switch (row) {
        case kResolution: {
            const std::vector<Size>& sizes = Sizes();
            int n = int(sizes.size()), cur = 0;
            for (int i = 0; i < n; i++)
                if (sizes[i].w == s.width && sizes[i].h == s.height) cur = i;
            cur = wrap(cur, n);
            s.width = sizes[cur].w;
            s.height = sizes[cur].h;
            app::ApplyDisplay();
            break;
        }
        case kWindow:
            s.display_mode = app::DisplayMode(wrap(int(std::min<uint32_t>(uint32_t(s.display_mode), 2)), 3));
            app::ApplyDisplay();
            break;
        case kShape: game_view::g_shape_setting = kShapes[wrap(IndexOf(kShapes, game_view::g_shape_setting), int(std::size(kShapes)))]; break;
        case kRate: s.fps_cap = kRates[wrap(IndexOf(kRates, s.fps_cap), int(std::size(kRates)))]; break;
        case kVsync:
            s.vsync = !s.vsync;
            app::ApplyDisplay();
            break;
        case kHd: {  // Off, Automatic, 2x, 4x; the first time on, the set starts being made
            bool started = ReadHd().started;
            uint32_t now = started ? s.hd_textures : 0;
            now = uint32_t((int(now) + dir + 4) % 4);
            s.hd_textures = now;
            if (now && !ReadHd().whole) gpu::texturedumper::StartHdMaker();
            break;
        }
        case kCoop: SetCoop(CoopChoice() == kOff ? kSplit : kOff); break;
        case kSection: g_section = (g_section + dir + kSections) % kSections; break;
        case kHdSound:
            game_hdsound::g_setting = game_hdsound::g_setting ? 0 : 1;
            game_hdsound::Apply();  // on and not made: the maker starts
            break;
        case kLook1: SetGameInverted(!GameInverted()); break;
        case kLook: game_coop::g_p2_invert = game_coop::g_p2_invert ? 0 : 1; break;
        case kSpeed: {
            int percent = int(std::clamp<uint32_t>(game_coop::g_p2_speed, 40, 100)) + dir * 10;
            game_coop::g_p2_speed = uint32_t(percent > 100 ? 40 : percent < 40 ? 100 : percent);
            break;
        }
    }
    app::Save();
    LOGI("menu: %s is now %s", kLabels[row], Value(row).c_str());
}


// ---- the page ---------------------------------------------------------------------------------------------------

// The widgets follow the state: focus, what each row says, and what the instructions say. Called on every input
// and once a frame; nothing is touched that already says the right thing.
void MoveFocusTo(int focus) { g_page.focus = focus; }
void Refresh(X86Context& ctx) {
    if (!g_page.screen) return;
    auto state = [&](uint32_t widget, uint32_t want) {
        if (widget && kernel::R32(widget + kState) != want) Method(ctx, widget, kSetState, {want});
    };
    bool seconds = SecondsPage();
    if (seconds != g_page.seconds) {  // (the page is built once and can be either: who opened it decides)
        g_page.seconds = seconds;
        if (seconds && (g_page.focus == 0 || !Visible(g_page.focus - 1))) MoveFocusTo(kLook + 1);
        else if (!seconds && g_page.focus > 0 && !Visible(g_page.focus - 1)) MoveFocusTo(0);
    }
    std::string title = seconds ? "PLAYER 2 SETTINGS" : "DISPLAY AND CO-OP";
    if (g_page.title && title != g_page.title_text) {
        SetText(ctx, g_page.title, title);
        g_page.title_text = title;
    }
    bool plain = seconds;  // (no brightness on Player 2's page)
    if (plain != g_page.plain) {
        g_page.plain = plain;
        if (g_page.slider) Method(ctx, g_page.slider, kSetState, {plain ? kHidden : kIdle});
    }
    state(g_page.caption, plain ? kHidden : g_page.focus == 0 ? kHighlighted : kIdle);
    int slot = 0;
    for (int r = 0; r < kRows; r++) {
        if (!g_page.rows[r]) continue;
        bool focused = g_page.focus == r + 1, visible = Visible(r);
        if (visible) {  // the next free line (Player 2's page starts higher: there is no slider on it)
            float y = (seconds ? g_page.slot_top - 36.0f : g_page.slot_top) + g_page.slot_step * float(slot++) - g_page.margin;
            if (std::fabs(y - g_page.row_y[r]) > 0.01f) WriteF(g_page.rows[r] + kY, y), g_page.row_y[r] = y;
        }
        state(g_page.rows[r], !visible ? kHidden : focused ? kHighlighted : kIdle);
        std::string text = visible ? RowText(r, focused) : std::string();
        if (text != g_page.shown[r]) {
            SetText(ctx, g_page.rows[r], text);
            g_page.shown[r] = text;
        }
    }
    std::string help = Help();
    if (help == g_page.help || !g_page.words) return;
    uint32_t words = g_page.words;
    if (g_page.help.empty()) {  // the game's own words are up: what they are, to put them back later
        g_page.words_id = kernel::R32(words + kTextId);
        g_page.words_own.clear();
        uint32_t at = kernel::R32(words + kString), size = kernel::R32(words + kStringSize);
        if (!g_page.words_id && at && size > 1 && size <= kScratchChars)
            for (uint32_t i = 0; i + 1 < size; i++) g_page.words_own.push_back(kernel::R16(at + i * 2));
    }
    if (!help.empty()) SetText(ctx, words, help);
    else if (g_page.words_id) ThisCall(ctx, kSetTextId, words, {g_page.words_id});
    else SetWide(ctx, words, g_page.words_own.data(), g_page.words_own.size());
    g_page.help = help;
}

void MoveFocus(int dir) {
    int f = g_page.focus;
    for (int tries = 0; tries <= kRows + 1; tries++) {
        f = (f + dir + kRows + 1) % (kRows + 1);
        if (f == 0 ? !SecondsPage() : (Visible(f - 1) && Focusable(f - 1))) break;  // (0 is the brightness: not on Player 2's page)
    }
    g_page.focus = f;
}

// once a frame while the page is up (a setting can change elsewhere: the game's own Invert Pitch Axis)
void Tick(X86Context& ctx) { Refresh(ctx); }


bool g_open_coop = false;  // the page being made is the main menu's "Co-op": it opens at the Co-op section
void Build(X86Context& ctx, uint32_t screen) {
    g_page = {};
    uint32_t children = kernel::R32(screen + kChildren), count = kernel::R32(screen + kCount);
    uint32_t slider = kernel::R32(screen + kSlider);
    // the page's own texts: the title is the highest, the button line the lowest, the instructions lie between
    // (their strings are not loaded yet, only their places are known)
    uint32_t title = 0, prompt = 0, words = 0;
    for (uint32_t i = 0; i < count && i < 32; i++) {
        uint32_t c = kernel::R32(children + i * 4);
        if (!c || kernel::R32(c) != kTextMethods) continue;
        if (!title || ReadF(c + kY) < ReadF(title + kY)) title = c;
        if (!prompt || ReadF(c + kY) > ReadF(prompt + kY)) prompt = c;
    }
    for (uint32_t i = 0; i < count && i < 32; i++) {
        uint32_t c = kernel::R32(children + i * 4);
        if (!c || kernel::R32(c) != kTextMethods || c == title || c == prompt) continue;
        if (!words || ReadF(c + kH) > ReadF(words + kH)) words = c;
    }
    if (!slider || !title || !prompt || !words) {
        LOGW("menu: the Brightness page is not laid out as expected (%u children), no display rows", count);
        return;
    }
    // The page is laid out again, evenly, from what was measured on the original (640 x 480 units, tops of the
    // drawn lines): title 77, instructions 196 to 271 (five lines 16.5 apart), slider 291 to 312, button line 410.
    // Now: title, instructions from 116, "Brightness" at 206, slider from 222, rows 20 apart from 261 (at most
    // six lines: a section's heading and its five rows), button line;
    // the gaps between the blocks are 21, 15, 7, 18 and 19.
    //
    // A text widget draws its lines in the middle of its box less the template's margins (properties 0x1E above
    // and 0x1F below, fractions of the layout height [0x391D7C] times [0x3384D4]; 0 and about 54 for the
    // instructions' template) and clips them to the box, so a one-line box is margin + line + margin high and
    // starts the upper margin above its line. A line's glyphs start 3.4 under the line's top. The boxes overlap,
    // the lines do not.
    uint32_t tmpl = kernel::R32(words + kTemplate);
    float unit = ReadF(0x391D7C) * ReadF(0x3384D4);
    uint32_t p_top = Property(ctx, tmpl, 0x1E), p_bottom = Property(ctx, tmpl, 0x1F);
    float margin_top = p_top ? ReadF(p_top) * unit : 0, margin_bottom = p_bottom ? ReadF(p_bottom) * unit : 0;
    constexpr float kLine = 17.0f, kGlyphTop = 3.4f, kGlyphHeight = 10.0f;
    constexpr float kWordsUp = 80.0f;         // the instructions move up by this much
    constexpr float kSliderUp = 69.0f;        // and the slider by this much
    constexpr float kSliderDrawnTop = 6.2f, kSliderDrawnBottom = 27.4f;  // where its picture lies in its box
    constexpr float kCaptionGap = 7.0f, kRowsGap = 18.0f, kStep = 20.0f;  // (at most six lines show at a time: a section)
    if (margin_top < 0 || margin_top > 100 || margin_bottom < 0 || margin_bottom > 100) {
        LOGW("menu: unexpected text margins %.1f %.1f, no display rows", margin_top, margin_bottom);
        return;
    }
    WriteF(words + kY, ReadF(words + kY) - kWordsUp);
    float slider_y = ReadF(slider + kY) - kSliderUp;
    WriteF(slider + kY, slider_y);
    float caption_top = slider_y + kSliderDrawnTop - kCaptionGap - kGlyphHeight - kGlyphTop;
    float top = slider_y + kSliderDrawnBottom + kRowsGap - kGlyphTop;
    float step = kStep;
    constexpr int kMostLines = 7;  // the section row and the six rows of the longer section (Display)
    float last_bottom = top + step * (kMostLines - 1) + kGlyphTop + kGlyphHeight;
    if (last_bottom > ReadF(prompt + kY)) {
        LOGW("menu: no room for the display rows (%.0f to %.0f)", top, ReadF(prompt + kY));
        return;
    }
    // wider than the instructions' box (the longest row, a 5120 x 1440 desktop, is about 300 wide), about the
    // same middle, inside the picture
    constexpr float kRowWidth = 336.0f;
    float w = std::max(ReadF(words + kW), kRowWidth);
    float x = std::clamp(ReadF(words + kX) + (ReadF(words + kW) - w) / 2, 8.0f, 632.0f - w);
    auto line = [&](float line_top) -> uint32_t {
        uint32_t t = kernel::CallGuest(ctx, kAlloc, {0, 0xC0});
        if (!t) return 0;
        ThisCall(ctx, kTextCtor, t, {tmpl});
        WriteF(t + kX, x);
        WriteF(t + kY, line_top - margin_top);
        WriteF(t + kW, w);
        WriteF(t + kH, margin_top + kLine + margin_bottom);
        ThisCall(ctx, kAddChild, screen, {t});
        return t;
    };
    g_page.caption = line(caption_top);
    if (g_page.caption) SetText(ctx, g_page.caption, "Brightness");
    for (int r = 0; r < kRows; r++) {
        g_page.rows[r] = line(top);  // (Refresh gives each visible row its line)
        g_page.row_y[r] = top - margin_top;
        if (!g_page.rows[r]) return;
    }
    g_page.slot_top = top, g_page.slot_step = step, g_page.margin = margin_top;
    g_page.title = title;
    g_page.prompt = prompt;
    g_page.seconds = false;
    g_section = g_open_coop ? 1 : 0;
    g_page.screen = screen;
    g_page.slider = slider;
    g_page.words = words;
    // the title is one line: its box is made wide enough for the longer name (the same middle)
    constexpr float kTitleWider = 80.0f;
    WriteF(title + kX, ReadF(title + kX) - kTitleWider / 2);
    WriteF(title + kW, ReadF(title + kW) + kTitleWider);
    SetText(ctx, title, "DISPLAY AND CO-OP");  // (the title font's P is drawn with a straight right side)
    g_page.title_text = "DISPLAY AND CO-OP";
    if (g_open_coop && !SecondsPage()) MoveFocusTo(kCoop + 1);  // (Player 2's page: his rows, Refresh)
    Refresh(ctx);
    LOGI("menu: Display and Co-op page built, rows from %.0f, %.1f apart (text margins %.1f and %.1f), %zu resolutions", top, step,
         margin_top, margin_bottom, Sizes().size());
}

}  // namespace

std::vector<uint32_t> g_p1_only;  // the pause screen's items that are Player 1's alone
std::atomic<bool> g_p1_item{false};
bool PauseItemP1Only() { return g_p1_item.load(); }

// The pause screen's title says who paused, in co-op: "PLAYER 2 PAUSED" where the game writes "PAUSE". The title is
// one of the screen's own text widgets (the newest screen of the menu manager [0x391D64]; screens at +0x50, count
// +0x58; a screen's widgets the same way; a text widget's words at +0x68, their count with the end at +0x6C), so
// the game draws it, in its font, on its strip of paper, once for both players. pauser 0: not paused.
// tests: S2_MENU_TRACE=1 logs the words of the newest screen's widgets whenever another screen comes to the top
void TraceTop() {
    // (always since 2026-11-26: where in the menus the player is belongs in every session log; the widgets' words
    // only with S2_MENU_TRACE, except the first few with any, which name the screen)
    static const bool trace = std::getenv("S2_MENU_TRACE") != nullptr;
    static uint32_t before = 0;
    uint32_t manager = kernel::R32(0x391D64);
    uint32_t screens = manager ? kernel::R32(manager + kChildren) : 0, count = manager ? kernel::R32(manager + kCount) : 0;
    uint32_t top = screens && count && count <= 64 ? kernel::R32(screens + (count - 1) * 4) : 0;
    if (top == before || !top || !mem::IsCommitted(top + kCount + 4)) return;
    before = top;
    uint32_t widgets = kernel::R32(top + kChildren), n = kernel::R32(top + kCount);
    LOGI("menu: screen %08X is on top (%u screens, %u widgets, methods %08X)", top, count, n, kernel::R32(top));
    int named = 0;
    for (uint32_t i = 0; widgets && i < n && i < 64 && (trace || named < 3); i++) {
        uint32_t w = kernel::R32(widgets + i * 4);
        if (!w || !mem::IsCommitted(w + kTextId + 4)) continue;
        uint32_t at = kernel::R32(w + kString), size = kernel::R32(w + kStringSize);
        if (!at || size < 2 || size > 64 || !mem::IsCommitted(at) || !mem::IsCommitted(at + size * 2)) continue;
        std::string text;
        for (uint32_t k = 0; k + 1 < size; k++) {
            uint16_t c = kernel::R16(at + k * 2);
            text += c >= 32 && c < 127 ? char(c) : '?';
        }
        named++;
        LOGI("menu:   widget %u (state %u): \"%s\"", i, kernel::R32(w + kState), text.c_str());
    }
}

// The game's own "Invert Pitch Axis" (Options, Controller Settings) is kept between runs, and is off until the player
// turns it on (the owner, 2026-11-26: "keeps getting reset every time I launch the game again, it should be off by
// default"). The game keeps its options in an object at 0x392158 (methods 0x347C44; the switches in the word at
// +0x50, bit 0x10 = inverted) and starts every run with them at their defaults, inverted among them: they only
// come from Suffering2.cfg through the Options menu's "Save Settings" and "Load Settings". Here the port's own
// settings file holds the choice (invert_pitch). A change made on the Controller Settings screen (methods
// 0x3414D0, or within a moment of it closing) is the player's and is kept; any other change is the game putting its
// own value back, and is undone.
uint32_t g_invert_pitch = 0;
void KeepInvert() {
    constexpr uint32_t kControllerScreen = 0x3414D0;
    if (!mem::IsCommitted(kSwitches) || kernel::R32(kOptions) != 0x347C44) return;
    static Clock::time_point on_screen{};
    uint32_t manager = kernel::R32(0x391D64);
    uint32_t screens = manager ? kernel::R32(manager + kChildren) : 0, count = manager ? kernel::R32(manager + kCount) : 0;
    uint32_t top = screens && count && count <= 64 ? kernel::R32(screens + (count - 1) * 4) : 0;
    if (top && mem::IsCommitted(top) && kernel::R32(top) == kControllerScreen) on_screen = Clock::now();
    uint32_t switches = kernel::R32(kSwitches);
    bool now = (switches & kInverted) != 0, want = g_invert_pitch != 0;
    if (now == want) return;
    if (Clock::now() - on_screen < std::chrono::milliseconds(1500)) {
        g_invert_pitch = now ? 1 : 0;
        app::Save();
        LOGR("menu: Invert Pitch Axis is now %s, kept for the next runs", now ? "Yes" : "No");
    } else {
        kernel::W32(kSwitches, switches ^ kInverted);
        LOGI("menu: the game had Invert Pitch Axis at %s; it is put back to the player's %s", now ? "Yes" : "No", want ? "Yes" : "No");
    }
}

void PauseTitle(X86Context& ctx, int pauser) {
    TraceTop();
    KeepInvert();
    static uint32_t titled = 0;  // the widget that was changed
    static int shown = 0;
    if (!pauser) {
        titled = 0, shown = 0;  // (the screen is gone with its widgets; the next pause makes new ones)
        g_p1_only.clear();
        g_p1_item = false;
        return;
    }
    // Player 2's pause: Save Game, Load Game and Quit Game are Player 1's (decided 2026-11-25). They are shown
    // greyed (the widget's state S2_MENU_GREY, 3 unless named) and A does nothing on them (pad.cpp asks
    // PauseItemP1Only; the item in focus is the highlighted one).
    if (pauser == 2 && shown == pauser) {
        static const uint32_t grey = std::getenv("S2_MENU_GREY") ? uint32_t(std::atoi(std::getenv("S2_MENU_GREY"))) : 3;
        bool on_one = false;
        // The Options list (the screen with the method table 0x3417D8, its entries the odd widgets from 5 on, as in
        // the pause screen): for Player 2 only "Display and Co-op" can be chosen, which is his settings page then.
        // The others are the host's game's settings (sound, controls, subtitles): greyed, and A does nothing on one
        // that still has the focus.
        {
            uint32_t manager = kernel::R32(0x391D64);
            uint32_t list = manager ? kernel::R32(manager + kChildren) : 0, n = manager ? kernel::R32(manager + kCount) : 0;
            uint32_t top = list && n && n <= 64 ? kernel::R32(list + (n - 1) * 4) : 0;
            if (top && mem::IsCommitted(top + kCount + 4) && kernel::R32(top) == 0x3417D8) {
                uint32_t widgets = kernel::R32(top + kChildren), wn = kernel::R32(top + kCount);
                for (uint32_t i = 5; widgets && i < wn && i < 64; i += 2) {
                    uint32_t widget = kernel::R32(widgets + i * 4);
                    if (!widget || !mem::IsCommitted(widget + kTextId + 4)) continue;
                    uint32_t at = kernel::R32(widget + kString), size = kernel::R32(widget + kStringSize);
                    bool mine = at && size == 18 && mem::IsCommitted(at + 36) && kernel::R16(at) == 'D' && kernel::R16(at + 2 * 12) == 'C';  // "Display and Co-op"
                    if (mine) continue;
                    uint32_t state = kernel::R32(widget + kState);
                    on_one |= state == kHighlighted;
                    if (state == kIdle) Method(ctx, widget, kSetState, {grey});
                }
            }
        }
        for (uint32_t widget : g_p1_only) {
            if (!mem::IsCommitted(widget + kState + 4)) continue;
            uint32_t state = kernel::R32(widget + kState);
            on_one |= state == kHighlighted;
            if (state == kIdle) Method(ctx, widget, kSetState, {grey});
        }
        g_p1_item = on_one;
    }
    if (shown == pauser) return;
    uint32_t manager = kernel::R32(0x391D64);
    uint32_t screens = manager ? kernel::R32(manager + kChildren) : 0, count = manager ? kernel::R32(manager + kCount) : 0;
    if (!screens || !count || count > 64) return;
    static const bool trace = std::getenv("S2_MENU_TRACE") != nullptr;
    auto words = [](uint32_t widget) {
        std::string out;
        if (!mem::IsCommitted(widget + kStringSize + 4)) return out;
        uint32_t at = kernel::R32(widget + kString), size = kernel::R32(widget + kStringSize);
        if (!at || size < 2 || size > 64 || !mem::IsCommitted(at) || !mem::IsCommitted(at + size * 2)) return out;
        for (uint32_t i = 0; i + 1 < size; i++) {
            uint16_t c = kernel::R16(at + i * 2);
            out += c >= 32 && c < 127 ? char(c) : '?';
        }
        return out;
    };
    for (uint32_t n = count; n-- > 0 && !titled;) {  // newest first: the pause screen is on top
        uint32_t screen = kernel::R32(screens + n * 4);
        if (!screen || !mem::IsCommitted(screen + kCount + 4)) continue;
        uint32_t widgets = kernel::R32(screen + kChildren), widget_count = kernel::R32(screen + kCount);
        if (!widgets || widget_count > 64) continue;
        for (uint32_t i = 0; i < widget_count; i++) {
            uint32_t widget = kernel::R32(widgets + i * 4);
            if (!widget || !mem::IsCommitted(widget + kTextId + 4)) continue;
            std::string text = words(widget);
            if (trace && !text.empty()) LOGI("menu: screen %u widget %u says \"%s\"", n, i, text.c_str());
            if (text == "PAUSE" || text == "Pause" || text == "PAUSED") titled = widget;
            if (text == "Save Game" || text == "Load Game" || text == "Quit Game" || text == "Revert To Last Checkpoint") g_p1_only.push_back(widget);
        }
    }
    if (!titled) return;
    SetText(ctx, titled, pauser == 2 ? "P2 PAUSED" : "P1 PAUSED");
    shown = pauser;
    LOGI("menu: the pause screen's title says Player %d paused", pauser);
}
}  // namespace game_menu

X86_EXTERN_FUNC(__imp__sub_0014BF00);
X86_EXTERN_FUNC(__imp__sub_001598A0);
X86_EXTERN_FUNC(__imp__sub_0014BEE0);
X86_EXTERN_FUNC(__imp__sub_0014BDA0);
X86_EXTERN_FUNC(__imp__sub_0014C340);
X86_EXTERN_FUNC(__imp__sub_0014AE60);
X86_EXTERN_FUNC(__imp__sub_00157A60);
X86_EXTERN_FUNC(__imp__sub_00154550);

// the Brightness screen's constructor (this, template): the page as the game makes it, then the display rows.
// The game makes the screen in two places: for the Options menu (0x14B97C; the main menu's Co-op makes it the same
// way, OpenCoop) and in the main menu's code before a new game starts (0x157652). Before a new game it is the
// game's own brightness page and nothing more: until 2026-11-26 it had the display and co-op rows there too, and
// the owner asked for them to go ("i dont want to be able to change my resolution upon entering a new game ... the
// display for brightness is fine tho, it just made it a bit confusing when my friend joined"). Resolution and the
// rest are in Options, from the main menu and from the pause menu.
X86_FUNC(sub_0014BF00) {
    uint32_t screen = ctx.ecx;
    bool before_new_game = kernel::R32(ctx.esp) == 0x157657;  // (the address the call at 0x157652 returns to)
    __imp__sub_0014BF00(ctx, base);
    if (before_new_game) {
        LOGI("menu: the brightness page before a new game is the game's own (no display rows on it)");
        return;
    }
    game_menu::Build(ctx, screen);
}

// ---- the main menu's "Co-op" -----------------------------------------------------------------------------------------
// The main menu (its work each frame 0x157A60, this = the menu) has eleven entries (their text widgets at +0xC4)
// and does the chosen one's action when its way out has played: the action's number at +0x6C (0xF none), through
// the jump table at 0x1582C8. Action 6 is "Friends", the console's online friends list (0x154550, then 0x1CC250),
// done only while the online service's switch ([0x384B58] + 0xBC) is on: on this port it never is, and the entry
// did nothing. It is now "Co-op": the entry says so, the switch reads as on for that one action, and the action
// opens the Display and Co-op page at its Co-op section (Off or Split screen, each player's look). The page is made
// as the Options menu makes it (0x14B8D0): its template is property 0x22 of the Options menu's template, which is
// property 0x0D of the main menu's own resource (0x154570; the resource's handle at [0x376B98], 0x15E310 loads it).
namespace game_menu {
constexpr char kFriendsName[] = "Co-op";  // (the place holds a word as long as "Friends")
// (the pause screen's own "Friends" is renamed the same way and opens the page: PauseFriends, OpenCoopFromPause)
// The main menu writes "Sign-in failed" in its corner: the console's online service, which is not there. The
// line's widget is hidden while it says so (seen in every picture of the main menu, 2026-11-26).
void HideSignIn(X86Context& ctx, uint32_t menu) {
    uint32_t children = kernel::R32(menu + kChildren), count = kernel::R32(menu + kCount);
    for (uint32_t i = 0; children && i < count && i < 64; i++) {
        uint32_t w = kernel::R32(children + i * 4);
        if (!w || !mem::IsCommitted(w + kTextId + 4) || kernel::R32(w) != kTextMethods || kernel::R32(w + kState) == kHidden) continue;
        uint32_t at = kernel::R32(w + kString), size = kernel::R32(w + kStringSize);
        if (!at || size < 8 || size > 40 || !mem::IsCommitted(at + size * 2)) continue;
        static const char kSays[] = "sign-in";
        bool same = true;
        for (uint32_t k = 0; k < 7 && same; k++) same = std::tolower(int(kernel::R16(at + k * 2) & 0xFF)) == kSays[k];
        if (!same) continue;
        Method(ctx, w, kSetState, {kHidden});
        static bool said = false;
        if (!said) said = true, LOGI("menu: the main menu's sign-in line (the console's online service) is hidden");
    }
}
void FriendsEntry(X86Context& ctx, uint32_t menu) {
    HideSignIn(ctx, menu);
    static uint32_t named = 0;
    if (named == menu) return;
    // (the entries' words are text widgets among the menu's children; they get their strings some frames after the
    // menu is made)
    // candidates: the menu's children, and what its eleven entry widgets (+0xC4) point at
    std::vector<uint32_t> found;
    uint32_t children = kernel::R32(menu + kChildren), count = kernel::R32(menu + kCount);
    for (uint32_t i = 0; children && i < count && i < 64; i++) found.push_back(kernel::R32(children + i * 4));
    for (uint32_t i = 0; i < 11; i++) {
        uint32_t e = kernel::R32(menu + 0xC4 + i * 4);
        if (!e || !mem::IsCommitted(e + 0x100)) continue;
        found.push_back(e);
        for (uint32_t off = 0x10; off < 0x100; off += 4) found.push_back(kernel::R32(e + off));
        uint32_t inner = kernel::R32(e + kChildren), n = kernel::R32(e + kCount);
        for (uint32_t k = 0; inner && n < 16 && k < n && mem::IsCommitted(inner + k * 4 + 4); k++) found.push_back(kernel::R32(inner + k * 4));
    }
    for (uint32_t w : found) {
        if (w < 0x10000 || !mem::IsCommitted(w) || !mem::IsCommitted(w + kTextId + 4)) continue;
        uint32_t at = kernel::R32(w + kString), size = kernel::R32(w + kStringSize);
        if (!at || size < 8 || size > 16 || !mem::IsCommitted(at + size * 2)) continue;
        static const char kWas[] = "friends";
        bool same = true;
        for (uint32_t k = 0; k < 7 && same; k++) same = std::tolower(int(kernel::R16(at + k * 2) & 0xFF)) == kWas[k];
        if (!same) continue;
        LOGI("menu: the main menu's Friends entry is now %s (its box: %.0f %.0f, %.0f x %.0f)", kFriendsName, ReadF(w + kX), ReadF(w + kY), ReadF(w + kW), ReadF(w + kH));
        SetText(ctx, w, kFriendsName);
        named = menu;
        return;
    }
}
// The pause screen's "Friends" (methods 0x342290; its actions by number at +0x6C, the jump table at 0x159AF0 in its
// once-a-frame method 0x1598A0: action 6 is 0x154550, done only while the online service's switch is on, which on
// this port it never is) is "Co-op" since 2026-11-27, as on the main menu (the owner: "remove it from the game, menus
// etc."): it opens the Display and Co-op page at its Co-op section (Player 2's own settings when the pause is his).
// The page is made the way the pause screen makes its Options menu (0x158D50): the Options template is property 0x0E
// of the menus' resource (0x15E310: its +4 holds the properties), the page's is property 0x22 of that, and the pause
// screen hides itself while the page is up (it shows itself again when it is on top once more).
// its entry's words: "Friends" becomes "Co-op", once for each pause screen (they come some frames after it is made)
void PauseFriends(X86Context& ctx, uint32_t pause) {
    static uint32_t named = 0;
    if (named == pause || !mem::IsCommitted(pause + kCount + 4)) return;
    uint32_t widgets = kernel::R32(pause + kChildren), n = kernel::R32(pause + kCount);
    for (uint32_t i = 0; widgets && i < n && i < 64; i++) {
        uint32_t w = kernel::R32(widgets + i * 4);
        if (!w || !mem::IsCommitted(w + kTextId + 4)) continue;  // (its entries are buttons, not plain text widgets)
        uint32_t at = kernel::R32(w + kString), size = kernel::R32(w + kStringSize);
        if (!at || size != 8 || !mem::IsCommitted(at + size * 2)) continue;
        static const char kWas[] = "Friends";
        bool same = true;
        for (uint32_t k = 0; k < 7 && same; k++) same = kernel::R16(at + k * 2) == uint16_t(kWas[k]);
        if (!same) continue;
        SetText(ctx, w, kFriendsName);
        named = pause;
        static int said = 0;
        if (said++ < 3) LOGI("menu: the pause screen's Friends entry is now %s", kFriendsName);
        return;
    }
}
void OpenCoopFromPause(X86Context& ctx, uint32_t pause) {
    uint32_t res = kernel::CallGuest(ctx, 0x15E310, {});
    uint32_t bag = res && mem::IsCommitted(res + 8) ? kernel::R32(res + 4) : 0;
    uint32_t at = bag ? Method(ctx, bag, 8, {0x0E, 0xFFFFFFFFu}) : 0;
    uint32_t options_tmpl = at && mem::IsCommitted(at + 4) ? kernel::R32(at) : 0;
    uint32_t page_at = options_tmpl ? Property(ctx, options_tmpl, 0x22) : 0;
    uint32_t page_tmpl = page_at ? kernel::R32(page_at) : 0, manager = kernel::R32(0x391D64);
    LOGR("menu: Co-op from the pause menu: the Options template is %08X, the page's %08X", options_tmpl, page_tmpl);
    if (!page_tmpl || !manager) return;
    uint32_t page = kernel::CallGuest(ctx, kAlloc, {0, 0xD4});
    if (!page) return;
    g_open_coop = true;
    ThisCall(ctx, 0x14BF00, page, {page_tmpl});
    g_open_coop = false;
    Method(ctx, manager, 8, {page});
    Method(ctx, pause, kSetState, {2});
    kernel::W32(pause + 0x70, 0);  // (as 0x158D50 does after it)
}
void OpenCoop(X86Context& ctx, uint32_t menu) {
    kernel::CallGuest(ctx, 0x15E310, {});
    uint32_t options = Property(ctx, kernel::R32(0x376B98), 0x0D);
    uint32_t options_tmpl = options ? kernel::R32(options) : 0;
    uint32_t page_at = options_tmpl ? Property(ctx, options_tmpl, 0x22) : 0;
    uint32_t page_tmpl = page_at ? kernel::R32(page_at) : 0, manager = kernel::R32(0x391D64);
    LOGR("menu: Co-op: the Options template is %08X, the page's %08X", options_tmpl, page_tmpl);
    if (!page_tmpl || !manager) return;
    Method(ctx, menu, kSetState, {2});
    uint32_t page = kernel::CallGuest(ctx, kAlloc, {0, 0xD4});
    if (!page) return;
    g_open_coop = true;
    ThisCall(ctx, 0x14BF00, page, {page_tmpl});
    g_open_coop = false;
    Method(ctx, manager, 8, {page});
}
}  // namespace game_menu

X86_FUNC(sub_00157A60) {
    uint32_t menu = ctx.ecx;
    game_menu::FriendsEntry(ctx, menu);
    uint32_t action = kernel::R32(menu + 0x6C);
    uint32_t live = kernel::R32(0x384B58), before = 0;
    bool mine = action == 6 && live && mem::IsCommitted(live + 0xBC);
    if (mine) before = kernel::R32(live + 0xBC), kernel::W32(live + 0xBC, 1);
    __imp__sub_00157A60(ctx, base);
    if (mine && kernel::R32(0x384B58) == live) kernel::W32(live + 0xBC, before);
    // (and after its own work too: back from a page it opened, the menu showed the sign-in line again, run
    // main_coop2; only while it is still the main menu, methods 0x342198: its work may have ended it)
    if (mem::IsCommitted(menu + 4) && kernel::R32(menu) == 0x342198) {
        uint32_t ecx = ctx.ecx;
        game_menu::HideSignIn(ctx, menu);
        ctx.ecx = ecx;
    }
}
// the pause screen's work each frame (this): its action 6 is done as the main menu's is, with the switch on for it
X86_FUNC(sub_001598A0) {
    uint32_t pause = ctx.ecx;
    uint32_t live = kernel::R32(0x384B58), before = 0;
    if (pause && kernel::R32(pause) == 0x342290) game_menu::PauseFriends(ctx, pause);
    bool mine = pause && kernel::R32(pause) == 0x342290 && kernel::R32(pause + 0x6C) == 6 && live && mem::IsCommitted(live + 0xBC);
    if (mine) before = kernel::R32(live + 0xBC), kernel::W32(live + 0xBC, 1);
    __imp__sub_001598A0(ctx, base);
    if (mine && kernel::R32(0x384B58) == live) kernel::W32(live + 0xBC, before);
}
// the main menu's action 6 and the pause screen's (this = the menu or the pause screen): was the online friends list
X86_FUNC(sub_00154550) {
    if (ctx.ecx && kernel::R32(ctx.ecx) == 0x342290) game_menu::OpenCoopFromPause(ctx, ctx.ecx);
    else game_menu::OpenCoop(ctx, ctx.ecx);
    kernel::Return(ctx, 0, 0);
}

// ---- the screens before the main menu, skipped with any button ---------------------------------------------------------
// (The owner, 2026-11-26.) Two screens come up when the game starts:
//   - the publisher's and the developer's logos, films of fifteen seconds together (screen methods 0x341B70, once
//     a frame 0x14DA30). Its input handler 0x14D690 only answers event 0x420 (the A button).
//   - the title with the legal lines (methods 0x3419E8, once a frame 0x14C590), which loads the menu's resources and
//     then stays until its least time is over: state at +0x68, 4 = waiting for the time (shown since +0x6C against the
//     seconds in property 0x15), 5 = done. It takes no input at all (0x1D6540).
// Now any button of controller 1 (or the keyboard keys that stand for them) ends the film and ends the title's
// wait. The loading itself is not cut short: the main menu needs what it loads.
namespace game_menu {
bool AnyButtonPressed() {
    static bool before = true;  // (a button held since before the screen came up does not count)
    input::Pad p = input::Poll(0);
    bool now = p.buttons != 0;
    for (int i = 0; i < 8; i++) now |= p.analog[i] > 60;
    bool pressed = now && !before;
    before = now;
    return pressed;
}
}  // namespace game_menu
X86_EXTERN_FUNC(__imp__sub_0014DA30);
X86_FUNC(sub_0014DA30) {  // the logo film's screen, once a frame (this)
    if (ctx.ecx && kernel::R32(ctx.ecx) == 0x341B70 && game_menu::AnyButtonPressed()) {
        // (the screen plays a list of films, the one in hand at +0x68; +0x70 is set when a film has ended, and the
        // screen then goes on to the next one or, after the last, closes and makes the title screen: 0x14D8E0.
        // The A button's own way, [0x384B94]->method +0x3C, did not end a film on this port.)
        LOGR("menu: a button was pressed: the logo films are skipped");
        kernel::W32(ctx.ecx + 0x68, 0x7FFF);
        kernel::W32(ctx.ecx + 0x70, 1);
    }
    __imp__sub_0014DA30(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0014C590);
X86_FUNC(sub_0014C590) {  // the title and legal screen, once a frame (this)
    static uint32_t asked = 0;  // the screen a button was pressed on (while it still loads: remembered until it waits)
    if (ctx.ecx && kernel::R32(ctx.ecx) == 0x3419E8) {
        if (game_menu::AnyButtonPressed()) asked = ctx.ecx;
        if (asked == ctx.ecx && kernel::R32(ctx.ecx + 0x68) == 4) {
            kernel::W32(ctx.ecx + 0x68, 5);
            asked = 0;
            LOGR("menu: a button was pressed: the title screen's wait is skipped");
        }
    }
    __imp__sub_0014C590(ctx, base);
}

// its destructor: the rows go with the screen's other children
X86_FUNC(sub_0014BEE0) {
    using namespace game_menu;
    if (ctx.ecx == g_page.screen) {
        if (std::getenv("S2_MENU_TRACE")) LOGI("menu: the page ends, called from %s", game_coop::CallersOf(ctx).c_str());
        g_page = {};
    }
    __imp__sub_0014BEE0(ctx, base);
}

// its input (this, event)
X86_FUNC(sub_0014BDA0) {
    using namespace game_menu;
    uint32_t event = kernel::R32(ctx.esp + 4);
    if (ctx.ecx != g_page.screen || !g_page.screen) {
        __imp__sub_0014BDA0(ctx, base);
        return;
    }
    static const bool trace = std::getenv("S2_MENU_TRACE") != nullptr;  // tests: every event the page is given
    if (trace) LOGI("menu: event %X (focus %d)", event, g_page.focus);
    if (SecondsPage() && g_page.focus == 0 && (event == kLeft || event == kRight)) {  // (the brightness is Player 1's)
        kernel::Return(ctx, 1, 1);
        return;
    }
    if (event == kUp || event == kDown) {
        MoveFocus(event == kDown ? 1 : -1);
        Refresh(ctx);
        kernel::Return(ctx, 1, 1);
        return;
    }
    if (g_page.focus > 0 && (event == kLeft || event == kRight || event == kAccept)) {
        if (Visible(g_page.focus - 1)) Step(g_page.focus - 1, event == kLeft ? -1 : 1);  // (never a row that is not shown)
        Refresh(ctx);
        kernel::Return(ctx, 1, 1);
        return;
    }
    __imp__sub_0014BDA0(ctx, base);
}

// once a frame (this)
X86_FUNC(sub_0014C340) {
    if (ctx.ecx == game_menu::g_page.screen && game_menu::g_page.screen) game_menu::Tick(ctx);
    __imp__sub_0014C340(ctx, base);
}

// the Options menu's constructor (this, template): its Brightness entry opens the page that is now Display and Co-op
X86_FUNC(sub_0014AE60) {
    uint32_t menu = ctx.ecx;
    __imp__sub_0014AE60(ctx, base);
    if (uint32_t entry = kernel::R32(menu + game_menu::kBrightnessEntry)) game_menu::SetText(ctx, entry, "Display and Co-op");
}
