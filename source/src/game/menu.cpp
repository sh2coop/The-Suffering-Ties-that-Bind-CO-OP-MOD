// Display settings and co-op in the game's own Options menu.
//
// The Options menu has six entries on its paper strip and no room for a seventh, so the "Brightness Setting" entry
// becomes "Display and Co-op": the game's own page (the brightness slider with its picture and instructions) gets
// rows under the slider, made of the game's own text widgets in the page's own style:
//     Resolution    every size the monitor offers, with its shape, and "Desktop"
//     Window mode   Full screen, Borderless, Windowed
//     Picture shape Automatic (the resolution's), 4:3, 16:10, 16:9, 21:9, 32:9
//     Frame rate    60, 120, the monitor's, no limit
//     VSync         On, Off
//     Co-op         Off, Split screen, Host online, Join online
//     (online)      hosting: this PC's address, for the friend who joins; joining: the host's address, to enter
// Up and down move between the slider and the rows, left and right (or A) change the row's value, and a change
// takes effect at once and is saved. The same page is shown before a new game starts.
//
// Co-op online. Host online: the game listens (net.cpp) and the row under it shows the address a friend has to
// enter. Join online: A on the Host address row starts entering the address, digit by digit (up and down change
// the digit, left and right move; a keyboard types it, Ctrl+V pastes it), A again joins, B gives the entry up.
// While one of the two co-op rows is in focus the page's instructions (the lines over the slider) say what the
// choice does and how it is going: waiting, joined, no answer.
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
extern bool g_host_setting;       // host online: a choice for this run
extern bool g_host_failed;        // hosting could not start (the port is taken)
extern uint32_t g_p2_invert, g_p2_speed;  // Player 2's own look: the other way up, and his speed in per cent
extern uint32_t g_remote_invert, g_remote_speed;  // ... a joined player's, for this session (coop.cpp)
extern bool g_remote_edited;
extern uint32_t g_p2_volume, g_remote_volume;  // a joined player's volume for the host's sound: this PC's, and the one who is in
int Pauser();  // 0 not paused, 1 or 2: whose pause menu is up
std::string CallersOf(X86Context& ctx);  // (traces)
}
namespace game_net {  // net.cpp
bool HostHasPlayer();
bool HostNotStarted();  // joining: the host answered, and has not started the game yet
int JoinState();  // 0 not joined, 1 asking the host, 2 playing in the host's game
int JoinPhase();  // while it is 1: 1 no answer yet, 2 the host is not in a level yet, 3 coming in now
std::string LastNote();
void ClearNote();
bool JoinAddress(const std::string& typed);
void Leave();
std::string PublicAddress();
std::string LocalAddress();
void SendLook(uint32_t invert, uint32_t speed, uint32_t volume);  // the host: the joined player changed his look settings in the pause menu
std::string OutsideAddress();  // this PC's code: its game port as seen from outside, "a.b.c.d:port" ("" not known yet)
int OutsideKind();             // 0 not known, 1 a code works with this router, 2 it does not (a port per destination)
bool HostExpect(const std::string& address);  // the host: a visitor will come from this code
bool HostExpecting();
int InternetState();  // hosting: 0 not yet, 1 looking the address up, 2 port open, 3 port to forward, 4 shared address, 5 none
}
namespace game_meet {  // meet.cpp: one short code (WOLF-4271) through the meeting point
bool Available();
int HostState();  // 0 not inviting, 1 asking for a code, 2 the code is there, 3 no code (Note says why)
std::string HostCode();
std::string Note();
void ClearNote();
std::string Tidy(const std::string& typed);
bool Join(const std::string& typed);
int JoinLookup();  // 0 nothing, 1 asking the meeting point, 2 the host's address is there, 3 failed (Note says why)
bool TakeHost(std::string& address);
void JoinForget();
}
namespace game_menu {
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
// (kInvite, kOnline and kStop are the rows of "Play with a Friend": invite, join, stop inviting)
enum Row { kSection, kResolution, kWindow, kShape, kRate, kVsync, kCoop, kInvite, kOnline, kStop, kLook, kSpeed, kVolume, kRows };
int g_section = 0;  // 0 Display, 1 Co-op
enum Coop { kOff, kSplit, kHost, kJoin, kCoops };

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
    uint32_t prompt = 0;       // the button line ("A Accept, B Cancel"): not on the waiting screen, where neither does anything
    bool no_prompt = false;
    bool plain = false;        // the waiting screen is up: no slider, no rows but this player's code
    bool friends = false;      // opened from the main menu's "Play with a Friend": invite and join, nothing else
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
constexpr const char* kCoopNames[kCoops] = {"Off", "Split screen", "Online, inviting", "Online, joining"};

template <typename T, size_t N>
int IndexOf(const T (&list)[N], T value) {
    for (size_t i = 0; i < N; i++)
        if (list[i] == value) return int(i);
    return 0;
}

// ---- co-op: the choice, the host's address and its entry -----------------------------------------------------------

// The Co-op row's choice. Split screen is the saved setting. Hosting and joining are choices for this run (a game
// that opens a port each time it starts, because of a choice made weeks ago, is not what a player expects).
int g_coop = -1;                  // (-1: not looked at yet)
Clock::time_point g_host_chosen;  // hosting starts when the row has rested on Host online for a moment, so that
                                  // stepping past it on the way to Join online opens nothing
int CoopChoice() {
    if (g_coop < 0) g_coop = game_coop::g_host_setting ? kHost : game_coop::g_mode_setting == 1 ? kSplit : kOff;
    return g_coop;
}
void SetCoop(int choice) {
    if (CoopChoice() == kJoin && choice != kJoin && game_net::JoinState()) game_net::Leave();
    g_coop = choice;
    game_coop::g_mode_setting = choice == kSplit ? 1 : 0;
    if (choice != kHost) game_coop::g_host_setting = false;
    g_host_chosen = Clock::now();
    game_net::ClearNote();
    game_meet::ClearNote();
    if (choice != kJoin) game_meet::JoinForget();
}
void SettleHost(bool now) {
    if (g_coop == kHost && !game_coop::g_host_setting && (now || Clock::now() - g_host_chosen > std::chrono::milliseconds(1000)))
        game_coop::g_host_setting = true;
}

// The host's address to join: as typed last time (join_ip in the settings file; net.cpp keeps it there on a join).
std::string g_address;
bool g_address_read = false;
std::string& Address() {
    if (!g_address_read) g_address = app::Get().join_ip, g_address_read = true;
    return g_address;
}

// "a.b.c.d" at the start of a text (all of it, when whole)
bool Quad(const char* s, int out[4], bool whole) {
    unsigned f[4];
    int used = 0;
    if (!std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    if (std::sscanf(s, "%3u.%3u.%3u.%3u%n", &f[0], &f[1], &f[2], &f[3], &used) != 4) return false;
    for (unsigned v : f)
        if (v > 255) return false;
    char after = s[used];
    if (whole ? after != 0 : (std::isdigit(static_cast<unsigned char>(after)) != 0)) return false;
    for (int i = 0; i < 4; i++) out[i] = int(f[i]);
    return true;
}
// A code: "a.b.c.d:port", the address a PC's game port has from outside (net.cpp, Stun). The port may be left out
// (0): that is a plain address on the game's own port, as before codes. All of the text when whole.
bool Code(const char* s, int out[5], bool whole) {
    int f[4];
    if (!Quad(s, f, false)) return false;
    int used = 0;
    unsigned a, b, c, d, port = 0;
    std::sscanf(s, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &used);
    int end = used;
    if (s[used] == ':' && std::isdigit(static_cast<unsigned char>(s[used + 1]))) {
        int more = 0;
        if (std::sscanf(s + used + 1, "%5u%n", &port, &more) != 1 || port > 65535) return false;
        end = used + 1 + more;
    }
    if (whole ? s[end] != 0 : (std::isdigit(static_cast<unsigned char>(s[end])) != 0)) return false;
    std::copy(f, f + 4, out);
    out[4] = int(port);
    return true;
}
std::string CodeText(const int f[5]) {
    char out[32];
    if (f[4]) std::snprintf(out, sizeof out, "%d.%d.%d.%d:%d", f[0], f[1], f[2], f[3], f[4]);
    else std::snprintf(out, sizeof out, "%d.%d.%d.%d", f[0], f[1], f[2], f[3]);
    return out;
}

// Entering a code: four numbers of three digits and a port of five, as on a console's network screen. A controller
// turns one digit at a time; a keyboard types the numbers (a full stop or a colon goes on to the next one).
// The joiner enters the host's code; the host enters the joiner's (for_friend).
constexpr int kFields = 5, kDigits = 17;
constexpr int kWidth[kFields] = {3, 3, 3, 3, 5}, kMost[kFields] = {255, 255, 255, 255, 65535}, kFirst[kFields] = {0, 3, 6, 9, 12};
struct Entry {
    int field[kFields] = {};
    int caret = 0;  // the digit in hand, 0 to 16
    int typed = 0;  // digits typed into this number from a keyboard so far
    bool full = false;  // the last digit typed filled its number and went on to the next (a full stop typed now
                        // is the one between the two, not a step further)
    bool for_friend = false;  // the host is entering the joiner's code
    std::string before;
};
Entry g_entry;
bool g_by_code = false;    // the join on its way was started with a short code
std::string g_friend;  // the host: the joiner's code as entered last (this run only)
std::atomic<bool> g_typing{false};  // (the controller code asks: Backspace is not the BACK button meanwhile)
// One press of a button is several events (A is 0x412, 0x418, 0x41A, 0x420 and 0x44B). When the entry ends on one
// of them, the others of that press are not the page's: for this many frames more the page's events are dropped
// and B does not close the page.
int g_after_entry = 0;

int FieldAt(int caret) {
    for (int n = kFields - 1; n > 0; n--)
        if (caret >= kFirst[n]) return n;
    return 0;
}
int LastOf(int n) { return kFirst[n] + kWidth[n] - 1; }

void EntryStart(bool for_friend) {
    g_entry = {};
    if (!for_friend) g_by_code = false;
    g_entry.for_friend = for_friend;
    g_entry.before = for_friend ? g_friend : Address();
    Code(g_entry.before.c_str(), g_entry.field, true);  // (noughts, when it is a name or nothing yet)
    g_typing = true;
    game_net::ClearNote();  // (what the last try came to is about the address before)
    LOGI("menu: %s code is being entered", for_friend ? "the friend's" : "the host's");
}
void EntryEnd(bool keep) {
    (g_entry.for_friend ? g_friend : g_address) = keep ? CodeText(g_entry.field) : g_entry.before;
    g_typing = false;
    g_after_entry = 2;
    LOGI("menu: the %s code was %s", g_entry.for_friend ? "friend's" : "host's", keep ? "entered" : "left as it was");
}
void EntryMove(int dir) {
    g_entry.caret = std::clamp(g_entry.caret + dir, 0, kDigits - 1);
    g_entry.typed = 0;
    g_entry.full = false;
}
// the digit in hand, one up or down; values that would take its number past its most are passed over
void EntryTurn(int dir) {
    int n = FieldAt(g_entry.caret);
    int& field = g_entry.field[n];
    int place = 1;
    for (int i = g_entry.caret; i < LastOf(n); i++) place *= 10;
    int digit = field / place % 10;
    for (int step = 1; step <= 10; step++) {
        int next = ((digit + dir * step) % 10 + 10) % 10;
        int value = field + (next - digit) * place;
        if (value <= kMost[n]) {
            field = value;
            break;
        }
    }
    g_entry.typed = 0;
    g_entry.full = false;
}
void EntryDigit(int digit) {
    int n = FieldAt(g_entry.caret);
    int& field = g_entry.field[n];
    field = g_entry.typed ? std::min(field * 10 + digit, kMost[n]) : digit;
    g_entry.typed++;
    g_entry.caret = LastOf(n);
    g_entry.full = false;
    if (g_entry.typed < kWidth[n] && field * 10 <= kMost[n]) return;  // room for another digit
    g_entry.typed = 0;
    if (n < kFields - 1) g_entry.caret = kFirst[n + 1], g_entry.full = true;
}
void EntryNext() {
    int n = FieldAt(g_entry.caret);
    if (n < kFields - 1 && !g_entry.full) g_entry.caret = kFirst[n + 1];
    g_entry.typed = 0;
    g_entry.full = false;
}
void EntryDelete() {
    g_entry.full = false;
    int n = FieldAt(g_entry.caret);
    if (g_entry.field[n]) {
        g_entry.field[n] /= 10;
        g_entry.caret = LastOf(n);
        if (g_entry.typed) g_entry.typed--;
    } else if (n > 0) {
        g_entry.caret = LastOf(n - 1);
        g_entry.typed = 0;
    }
}
// "192.168.001.0[2]0:27025": the digit in hand in brackets
std::string EntryText() {
    char cells[32];
    std::snprintf(cells, sizeof cells, "%03d.%03d.%03d.%03d:%05d", g_entry.field[0], g_entry.field[1], g_entry.field[2], g_entry.field[3],
                  g_entry.field[4]);
    std::string out;
    int digit = 0;
    for (const char* c = cells; *c; c++) {
        bool is_digit = *c != '.' && *c != ':';
        bool hand = is_digit && digit == g_entry.caret;
        if (hand) out += '[';
        out += *c;
        if (hand) out += ']';
        digit += is_digit;
    }
    return out;
}

// Entering a friend's short code, WOLF-4271: four letters and four digits. A controller turns one character at a
// time (up and down), a keyboard types them; the dash is not typed.
struct Friendly {
    char c[9] = "AAAA0000";
    int caret = 0;
};
Friendly g_fr;
bool g_friendly = false;   // the entry that is open is this one
std::string g_code_typed;  // the code entered last (this run): there again when the friend has to come back in
void FriendlyStart(bool empty) {
    g_fr = {};
    std::string before = game_meet::Tidy(g_code_typed);
    if (!empty && before.size() == 9) std::memcpy(g_fr.c, (before.substr(0, 4) + before.substr(5)).c_str(), 8);
    g_friendly = true;
    g_typing = true;
    game_net::ClearNote();
    game_meet::ClearNote();
    LOGI("menu: a friend's code is being entered");
}
std::string FriendlyCode() { return std::string(g_fr.c, 4) + "-" + std::string(g_fr.c + 4, 4); }
void JoinByCode(const std::string& code) {
    g_code_typed = code;
    g_by_code = true;
    bool asked = game_meet::Join(code);
    LOGI("menu: join with the code %s %s", code.c_str(), asked ? "started" : "did not start");
}
void FriendlyEnd(bool keep) {
    g_typing = false;
    g_friendly = false;
    g_after_entry = 2;
    LOGI("menu: the friend's code was %s", keep ? "entered" : "left as it was");
    if (keep) JoinByCode(FriendlyCode());
}
void FriendlyMove(int dir) { g_fr.caret = std::clamp(g_fr.caret + dir, 0, 7); }
void FriendlyTurn(int dir) {
    char& ch = g_fr.c[g_fr.caret];
    if (g_fr.caret < 4) ch = char('A' + ((ch - 'A' + dir) % 26 + 26) % 26);
    else ch = char('0' + ((ch - '0' + dir) % 10 + 10) % 10);
}
void FriendlyKey(char key) {  // a letter or a digit typed: it goes where it fits, from the caret on
    bool letter = key >= 'A' && key <= 'Z';
    if (letter && g_fr.caret >= 4) return;
    if (!letter && g_fr.caret < 4) g_fr.caret = 4;
    g_fr.c[g_fr.caret] = key;
    if (g_fr.caret < 7) g_fr.caret++;
}
// "WO[L]F-4271": the character in hand in brackets
std::string FriendlyText() {
    std::string out;
    for (int i = 0; i < 8; i++) {
        if (i == 4) out += '-';
        if (i == g_fr.caret) out += '[';
        out += g_fr.c[i];
        if (i == g_fr.caret) out += ']';
    }
    return out;
}
// a short code somewhere in a text (a friend's message): "" none
std::string FindFriendly(const std::string& text) {
    for (size_t i = 0; i + 8 <= text.size(); i++) {
        if (i && std::isalnum(static_cast<unsigned char>(text[i - 1]))) continue;
        size_t at = i;
        int letters = 0, digits = 0;
        while (at < text.size() && letters < 4 && std::isalpha(static_cast<unsigned char>(text[at]))) at++, letters++;
        if (letters != 4) continue;
        if (at < text.size() && (text[at] == '-' || text[at] == ' ')) at++;
        while (at < text.size() && digits < 4 && std::isdigit(static_cast<unsigned char>(text[at]))) at++, digits++;
        if (digits != 4 || (at < text.size() && std::isalnum(static_cast<unsigned char>(text[at])))) continue;
        return game_meet::Tidy(text.substr(i, at - i));
    }
    return "";
}

// the host has entered the joiner's code: his router is shown packets going there
void ExpectNow() {
    int f[kFields];
    if (!Code(g_friend.c_str(), f, true) || !f[4]) {
        LOGI("menu: the friend's code has no port, nothing is sent");
        return;
    }
    bool started = game_net::HostExpect(g_friend);
    LOGI("menu: waiting for the friend %s", started ? "started" : "did not start");
}

void JoinNow() {
    if (Address().empty()) return;
    bool started = game_net::JoinAddress(g_address);
    LOGI("menu: join online %s", started ? "started" : "did not start");
}

// Tests: S2_KEYS="70:1,70.3:9,71:.,75:b,76:v" are keys of the keyboard at those seconds after the start, in place of
// the real ones (a test must not type into whatever window the player has in front): a digit, a full stop,
// b Backspace, d Delete, e Escape, v Ctrl+V (the clipboard is then the text in S2_KEYS_PASTE).
const Clock::time_point g_started = Clock::now();
struct TestKey {
    double at;
    char key;
};
std::vector<TestKey>& TestKeys() {
    static std::vector<TestKey> keys = [] {
        std::vector<TestKey> out;
        const char* v = std::getenv("S2_KEYS");
        while (v && *v) {
            double at = 0;
            char key = 0;
            if (std::sscanf(v, "%lf:%c", &at, &key) == 2) out.push_back({at, key});
            v = std::strchr(v, ',');
            if (v) v++;
        }
        return out;
    }();
    return keys;
}
char TestKeyNow() {
    double t = std::chrono::duration<double>(Clock::now() - g_started).count();
    for (TestKey& k : TestKeys())
        if (k.key && t >= k.at) {
            char key = k.key;
            k.key = 0;
            return key;
        }
    return 0;
}

#ifdef _WIN32
// a key went down since the last look
bool Pressed(int vk) {
    static bool down[256] = {};
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = now && !down[vk & 255];
    down[vk & 255] = now;
    return edge;
}
std::string Clipboard() {
    std::string out;
    if (!TestKeys().empty()) return std::getenv("S2_KEYS_PASTE") ? std::getenv("S2_KEYS_PASTE") : "";
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* text = static_cast<const char*>(GlobalLock(h))) {
            out.assign(text, strnlen(text, 256));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}
// Ctrl+C or a controller's X on the code row: this PC's code goes to the clipboard, to be pasted into a message to
// the friend (asked for after the first online session, 2026-11-25: the codes were read out and typed by hand).
std::chrono::steady_clock::time_point g_copied_at{};
bool JustCopied() { return std::chrono::steady_clock::now() - g_copied_at < std::chrono::milliseconds(2500); }
void CopyMine() {
    std::string code = game_net::OutsideAddress();
    if (CoopChoice() == kHost && !game_meet::HostCode().empty()) code = game_meet::HostCode();  // (the one a friend types)
    if (code.empty() || !OpenClipboard(static_cast<HWND>(app::Window()))) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, code.size() + 1)) {
        std::memcpy(GlobalLock(h), code.c_str(), code.size() + 1);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
        else g_copied_at = std::chrono::steady_clock::now(), LOGI("menu: this PC's code was copied");
    }
    CloseClipboard();
}
void JoinNow();
// Ctrl+V: an address anywhere in the clipboard's text (a friend's message with it in). Outside the digit entry a
// host name is taken too.
void Paste() {
    std::string text = Clipboard();
    bool join_side = CoopChoice() == kJoin || g_page.focus == kOnline + 1;
    if (std::string code = FindFriendly(text); !code.empty() && game_meet::Available() && join_side && (!g_typing || g_friendly)) {
        if (g_typing) g_typing = false, g_friendly = false, g_after_entry = 2;
        if (CoopChoice() != kJoin) SetCoop(kJoin);
        LOGI("menu: a short code was pasted");
        if (!game_net::JoinState()) JoinByCode(code);
        return;
    }
    if (g_friendly) return;
    for (size_t i = 0; i < text.size(); i++) {
        char before = i ? text[i - 1] : ' ';
        if (std::isdigit(static_cast<unsigned char>(before)) || before == '.') continue;
        int f[kFields];
        if (!Code(text.c_str() + i, f, false)) continue;
        if (g_typing) {
            std::copy(f, f + kFields, g_entry.field);
            g_entry.caret = kDigits - 1;
            g_entry.typed = 0;
        } else if (CoopChoice() == kHost && g_page.focus != kOnline + 1) {
            g_friend = CodeText(f);
            ExpectNow();
        } else {
            if (CoopChoice() != kJoin) SetCoop(kJoin);
            Address() = CodeText(f);
            g_by_code = false;
            game_net::ClearNote();
            if (!game_net::JoinState()) JoinNow();  // (a pasted code is whole: the join starts at once)
        }
        LOGI("menu: a code was pasted");
        return;
    }
    if (g_typing || (CoopChoice() == kHost && g_page.focus != kOnline + 1)) return;
    size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return;
    std::string name = text.substr(a, b - a + 1);
    bool letters = false, plain = name.size() >= 4 && name.size() <= 64 && name.find('.') != std::string::npos;
    for (char c : name) {
        letters |= std::isalpha(static_cast<unsigned char>(c)) != 0;
        plain &= std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':';
    }
    if (letters && plain) {
        g_address = name;
        game_net::ClearNote();
        LOGI("menu: a host name was pasted");
    }
}
// the keyboard on the Host address row, once a frame
void Keys(bool entry) {
    bool front = GetForegroundWindow() == static_cast<HWND>(app::Window());
    bool paste = Pressed('V') && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && front;
    int digit = -1;
    for (int d = 0; d < 10; d++)
        if (int(Pressed('0' + d)) | int(Pressed(VK_NUMPAD0 + d))) digit = d;
    bool next = (int(Pressed(VK_OEM_PERIOD)) | int(Pressed(VK_DECIMAL)) | int(Pressed(VK_OEM_1))) != 0;  // . or : (the ; key)
    bool del = Pressed(VK_BACK), clear = Pressed(VK_DELETE), leave = Pressed(VK_ESCAPE);
    if (!TestKeys().empty()) {  // (tests)
        char key = TestKeyNow();
        front = true, paste = key == 'v', digit = key >= '0' && key <= '9' ? key - '0' : -1;
        next = key == '.' || key == ':', del = key == 'b', clear = key == 'd', leave = key == 'e';
    }
    char letter = 0;
    for (int c = 'A'; c <= 'Z'; c++)
        if (Pressed(c) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) letter = char(c);
    if (!TestKeys().empty()) letter = 0;
    if (paste) Paste();
    if (!front || (GetAsyncKeyState(VK_SHIFT) & 0x8000 && !letter)) return;
    bool friendly_row = g_page.focus == kOnline + 1 && game_meet::Available();
    if (entry && g_friendly) {  // the short code: letters and digits
        if (letter) FriendlyKey(letter);
        if (digit >= 0) FriendlyKey(char('0' + digit));
        if (del) FriendlyMove(-1);
        if (leave) FriendlyEnd(false);
        return;
    }
    if (!entry && friendly_row) {  // a letter typed on the row starts a new code
        if (!letter) return;
        if (CoopChoice() != kJoin) SetCoop(kJoin);
        FriendlyStart(true);
        FriendlyKey(letter);
        return;
    }
    if (!entry) {  // a digit typed on the row starts a new address
        if (digit < 0) return;
        if (g_page.focus == kOnline + 1 && CoopChoice() != kJoin) SetCoop(kJoin);
        EntryStart(g_page.focus == kInvite + 1);
        std::fill(g_entry.field, g_entry.field + kFields, 0);
    }
    if (digit >= 0) EntryDigit(digit);
    if (next) EntryNext();
    if (del) EntryDelete();
    if (clear) g_entry.field[FieldAt(g_entry.caret)] = 0, g_entry.typed = 0;
    if (leave) EntryEnd(false);
}
#endif

// ---- what the rows say -----------------------------------------------------------------------------------------

// Player 2's look settings the page shows and changes: a joined player's (kept by the host for the session) while
// one is in, else this PC's own (split screen's Player 2, or this player when he joins someone)
uint32_t& LookInvert() { return game_net::HostHasPlayer() ? game_coop::g_remote_invert : game_coop::g_p2_invert; }
uint32_t& LookSpeed() { return game_net::HostHasPlayer() ? game_coop::g_remote_speed : game_coop::g_p2_speed; }
uint32_t& JoinVolume() { return game_net::HostHasPlayer() ? game_coop::g_remote_volume : game_coop::g_p2_volume; }
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
        case kCoop: return kCoopNames[CoopChoice()];
        case kSection: return g_section ? "Co-op" : "Display";
        case kLook: return LookInvert() ? "Up and down swapped" : "As the game has it";
        case kVolume:
            if (!JoinVolume()) return "Off";
            std::snprintf(out, sizeof out, "%u%%", std::min<uint32_t>(JoinVolume(), 100));
            return out;
        case kSpeed:
            std::snprintf(out, sizeof out, "%u%%", std::clamp<uint32_t>(LookSpeed(), 40, 100));
            return out;
    }
    return "";
}

constexpr const char* kLabels[kRows] = {"Settings", "Resolution", "Window mode", "Picture shape", "Frame rate", "VSync", "Co-op", "Invite", "Online", "Stop inviting", "Player 2 look", "Player 2 look speed", "Player 2 volume"};

// What is on the page now. Player 2's own page: his two rows. Otherwise the section's rows under the row that
// chooses the section. Player 2's look rows are his alone: on Player 1's page they only show for a player who is
// about to join someone (he is Player 2 there, and these are his own PC's settings).
void MoveFocusTo(int focus);
// The waiting screen: this player has asked to join and is not in the host's game yet. The page then says only what
// he is waiting for (title and instructions) and, while the host still needs it, his code.
bool Waiting() { return !SecondsPage() && CoopChoice() == kJoin && game_net::JoinState() == 1; }

bool Visible(int row) {
    if (Waiting()) return row == kOnline && game_net::JoinPhase() == 1 && !g_typing && !g_by_code;
    // (his volume is for a player on another PC: in split screen both players have one sound)
    if (SecondsPage()) return row == kLook || row == kSpeed || (row == kVolume && game_net::HostHasPlayer());
    // "Play with a Friend" (from the main menu): invite, join, and stop inviting while this game invites
    if (g_page.friends) return row == kInvite || row == kOnline || (row == kStop && CoopChoice() == kHost);
    switch (row) {
        case kSection: return true;
        case kResolution: case kWindow: case kShape: case kRate: case kVsync: return g_section == 0;
        case kCoop: case kInvite: return g_section == 1;  // (Invite a friend: here too, so that the pause menu has it)
        case kStop: return g_section == 1 && CoopChoice() == kHost;
        case kOnline: return false;                       // (joining is on the main menu's Play with a Friend)
        case kLook: case kSpeed: case kVolume: return g_section == 1 && CoopChoice() == kJoin;
    }
    return false;
}
bool Focusable(int) { return true; }

// The row under Co-op. Hosting: this PC's code (its game port as seen from outside), or its address when no code
// is known; while the host enters the joiner's code, that. Joining: the host's code; once the join is on its way,
// this PC's own code, which the host needs.
std::string InviteText(bool focused) {
    if (CoopChoice() != kHost) return "Invite a friend";
    {
        if (g_typing) return "Friend's code:  " + EntryText();
        if (!game_coop::g_host_failed && game_meet::Available()) {  // the short code a friend types
            std::string invite = game_meet::HostCode();
            if (!invite.empty()) return focused ? "Invite code:  [ " + invite + " ]" : "Invite code:  " + invite;
            if (game_meet::HostState() != 3) return "Invite code:  one moment";
        }
        std::string code = game_coop::g_host_failed ? std::string() : game_net::OutsideAddress();
        if (!code.empty()) return focused ? "Your code:  [ " + code + " ]" : "Your code:  " + code;
        std::string address;
        if (game_coop::g_host_failed) {
            address = "not hosting";
        } else {
            switch (game_net::InternetState()) {
                case 0: address = "starting"; break;
                case 1: address = "looking it up"; break;
                case 2:
                case 3: address = game_net::PublicAddress(); break;
                default: address = game_net::LocalAddress(); break;  // no way in from outside: the home network's
            }
            if (address.empty()) address = "not known";
        }
        return "Your address:  " + address;
    }
}
// The Join row: the friend's code while it is entered and looked up; once the join is on its way with an address
// code, this PC's own code, which the host needs.
std::string OnlineText(bool focused) {
    if (g_typing && g_friendly) return "Friend's code:  " + FriendlyText();
    if (g_typing && !g_entry.for_friend) return "Host's code:  " + EntryText();
    if (game_meet::Available() && !game_net::JoinState()) {
        if (game_meet::JoinLookup() == 1) return "Friend's code:  " + g_code_typed + "  (looking it up)";
        if (g_code_typed.empty()) return "Join a friend";
        return focused ? "Join a friend:  [ " + g_code_typed + " ]" : "Join a friend:  " + g_code_typed;
    }
    if (game_net::JoinState() && g_by_code) return "Friend's code:  " + g_code_typed + (game_net::JoinState() == 2 ? "  (joined)" : "");
    if (game_net::JoinState() == 1) {
        std::string mine = game_net::OutsideAddress();
        return "Your code:  " + (mine.empty() ? std::string("looking it up") : mine);
    }
    if (game_net::JoinState()) return "Host's code:  " + Address() + "  (joined)";
    if (Address().empty()) return "Join a friend";
    return focused ? "Join a friend:  [ " + Address() + " ]" : "Join a friend:  " + Address();
}

// A row that shows an internet address (this PC's code, or the one being typed): the session's pictures are not
// taken while one is on the screen (record.cpp), so that a pictures folder sent to someone holds nobody's address.
std::atomic<std::chrono::steady_clock::time_point> g_address_shown{};
bool HasAddress(const std::string& text) {
    for (size_t i = 0; i < text.size(); i++) {  // four numbers of up to three digits with dots between them
        size_t k = i;
        for (int groups = 0;;) {
            size_t from = k;
            while (k < text.size() && text[k] >= '0' && text[k] <= '9') k++;
            if (k == from || k - from > 3) break;
            if (++groups == 4) return true;
            if (k >= text.size() || text[k] != '.') break;
            k++;
        }
    }
    return false;
}
std::string RowText(int row, bool focused) {
    if (row == kOnline) return OnlineText(focused);
    if (row == kInvite) return InviteText(focused);
    if (row == kStop) return "Stop inviting";
    std::string v = Value(row);
    std::string label = kLabels[row];
    if (!SecondsPage() && row == kLook) label = "My look";  // (the one who joins is Player 2: his own PC's settings)
    if (!SecondsPage() && row == kSpeed) label = "My look speed";
    if (!SecondsPage() && row == kVolume) label = "My volume";
    if (row == kSection) return focused ? "<  " + v + " settings  >" : v + " settings";
    return label + (focused ? ":  < " : ":  ") + v + (focused ? " >" : "");
}

// What the page's instructions say while a co-op row is in focus (empty: the game's own words about the slider).
// The box holds five lines of about thirty characters.
std::string Help() {
    int row = g_page.focus - 1;
    if (row == kVolume)
        return SecondsPage() ? "How loud the game is on Player 2's own PC. It only changes Player 2."
                             : "How loud the host's game is on this PC while you are in it. Your own setting.";
    if (SecondsPage() && row != kLook && row != kSpeed)
        return "Player 2's own settings. Go down to change how you look around. Nothing here changes Player 1.";
    if (row == kSection)
        return g_section ? "Co-op: play with a friend on this PC or online. Left or right goes to the Display settings."
                         : "Display: resolution, window, picture shape and frame rate. Left or right goes to the Co-op settings.";
    if (row == kLook)
        return "Player 2's own up and down. It only changes Player 2. Try both: one of them will feel right.";
    if (row == kSpeed) return "How fast Player 2 turns, as a share of the game's own look speed. It only changes Player 2.";
    if (row == kStop) return "Stop inviting: the code stops working, and a friend who is in your game leaves it.";
    if (row == kCoop && CoopChoice() >= kHost)
        return "Online play is on. Left or right ends it and goes back to Off or Split screen.";
    if (row == kInvite && CoopChoice() != kHost)
        return "You host the game and your friend joins it. Press A: a short code appears here for you to send to your friend.";
    if (row != kCoop && row != kOnline && row != kInvite) return "";
    switch (row == kInvite ? int(kHost) : row == kOnline ? int(kJoin) : CoopChoice()) {
        case kSplit:
            return "Two players on this PC, side by side. Player 2 uses the second controller and comes in once a level is in play.";
        case kHost: {
            if (game_coop::g_host_failed) return "Cannot host: UDP port 27025 is in use on this PC. Is another copy of the game hosting?";
            if (game_net::HostHasPlayer()) return "Player 2 has joined your game and comes in once a level is in play.";
            if (g_typing)
                return "Enter the code your friend's screen shows. Up and down change the digit, A is done, B cancels. Ctrl+V pastes it.";
            if (JustCopied()) return "Copied. Paste it into a message to your friend.";
            if (game_net::HostExpecting()) return "Your friend is connecting. This can take half a minute.";
            if (!game_coop::g_host_failed && game_meet::Available() && !game_meet::HostCode().empty())
                return "Send this code to your friend: A or X copies it. Your friend picks Co-op on the main menu, then Join a friend. Just play meanwhile.";
            if (game_net::OutsideKind() == 2)
                return "Your router gives every connection a new port, so a code cannot work. Forward UDP port 27025 to this PC instead.";
            if (!game_net::OutsideAddress().empty())
                return "X (Ctrl+C) copies your code for your friend. He joins with it and sends his back: Y (Ctrl+V) pastes it here, A types it.";
            switch (game_net::InternetState()) {
                case 1: return "Hosting. Looking up your internet address...";
                case 2: return "Hosting. Your friend chooses Join online and enters your address. Waiting for Player 2.";
                case 3:
                    return "Hosting, but your router did not open the port. Forward UDP port 27025 to this PC, then a friend can join.";
                case 4:
                    return "Your internet provider shares your address with others, so only players on your home network can join this PC.";
                case 5: return "No internet address was found. Players on your home network can join with the address below.";
            }
            return "Starting to host...";
        }
        case kJoin: {
            if (g_typing && g_friendly)
                return "Up and down change the letter or digit, left and right move. A joins, B cancels. A keyboard can type the code, or paste it with Ctrl+V.";
            if (g_typing)
                return "Up and down change the digit, left and right move. A joins, B cancels. A keyboard can type the code, or paste it with Ctrl+V.";
            if (game_meet::JoinLookup() == 1) return "Looking the code up...";
            if (!game_net::JoinState() && !game_meet::Note().empty()) return game_meet::Note();
            if (game_net::JoinPhase() == 3 && game_net::HostNotStarted()) return "You are connected. Your friend's screen comes up now.";
            if (game_net::JoinPhase() == 3) return "The host is in the game. You are coming in as Player 2 now.";
            if (game_net::JoinPhase() == 1 && g_by_code)
                return "The code is right. Connecting to your friend's game now. This can take half a minute. Hold BACK to stop.";
            if (game_net::JoinPhase() == 2)
                return "You are connected. The host is still in his menu. You come in by yourself as soon as he starts or loads a game. Hold BACK to stop.";
            if (game_net::JoinState() == 1) {
                if (game_net::OutsideKind() == 2)
                    return "Your router gives every connection a new port, so a code cannot work. The host has to forward UDP port 27025. Hold BACK to stop.";
                if (JustCopied()) return "Copied. Paste it into a message to the host.";
                return "The host has to enter YOUR code. X (Ctrl+C) copies it: send it to him. Then you are connected. Hold BACK to stop.";
            }
            if (game_net::JoinState()) return "Joined. To leave the game, hold BACK (keyboard: Backspace) for two seconds.";
            std::string note = game_net::LastNote();
            if (!note.empty()) {
                if (note.rfind("No answer", 0) == 0 && g_by_code)
                    return "No connection to your friend's game. One of your routers may block direct connections: ask your friend to forward UDP port 27025.";
                if (note.rfind("No answer", 0) == 0) note += " Check the code, and that the host entered yours.";
                return note;
            }
            if (game_meet::Available()) return "Press A and enter the code your friend sent you, like WOLF-4271. Y (Ctrl+V) pastes it. To leave a game later, hold BACK.";
            return "Y (Ctrl+V) pastes the host's code and joins. Or press A, enter the code your friend's screen shows, and press A.";
        }
    }
    return "Play with a friend: Split screen on this PC, or Host online and Join online over the internet.";
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
        // Off; two players on this PC (the second Torque on controller 2, the two views side by side); or online,
        // as the host or as the one who joins. Split screen takes effect at once (coop.cpp: Player 2 comes in when
        // a level is in play, and is taken out when it is switched off).
        case kCoop: SetCoop(CoopChoice() == kOff ? kSplit : kOff); break;  // (online: Play with a Friend, Invite a friend)
        case kSection: g_section = g_section ? 0 : 1; break;
        case kLook:
            LookInvert() = LookInvert() ? 0 : 1;
            if (game_net::HostHasPlayer()) game_coop::g_remote_edited = true, game_net::SendLook(LookInvert(), LookSpeed(), JoinVolume());
            break;
        case kVolume: {
            int percent = int(std::min<uint32_t>(JoinVolume(), 100)) + dir * 10;
            JoinVolume() = uint32_t(std::clamp(percent, 0, 100));
            if (game_net::HostHasPlayer()) game_coop::g_remote_edited = true, game_net::SendLook(LookInvert(), LookSpeed(), JoinVolume());
            break;
        }
        case kSpeed: {
            int percent = int(std::clamp<uint32_t>(LookSpeed(), 40, 100)) + dir * 10;
            LookSpeed() = uint32_t(percent > 100 ? 40 : percent < 40 ? 100 : percent);
            if (game_net::HostHasPlayer()) game_coop::g_remote_edited = true, game_net::SendLook(LookInvert(), LookSpeed(), JoinVolume());
            break;
        }
    }
    app::Save();
    LOGI("menu: %s is now %s", kLabels[row], Value(row).c_str());
}

// ---- the page ---------------------------------------------------------------------------------------------------

// The widgets follow the state: focus, what each row says, and what the instructions say. Called on every input
// and once a frame (the online rows change by themselves); nothing is touched that already says the right thing.
void MoveFocusTo(int focus) { g_page.focus = focus; }
void Refresh(X86Context& ctx) {
    if (!g_page.screen) return;
    auto state = [&](uint32_t widget, uint32_t want) {
        if (widget && kernel::R32(widget + kState) != want) Method(ctx, widget, kSetState, {want});
    };
    bool seconds = SecondsPage(), waiting = Waiting();
    if (waiting) MoveFocusTo(kOnline + 1);  // (his code's row: X copies it)
    if (seconds != g_page.seconds) {  // (the page is built once and can be either: who opened it decides)
        g_page.seconds = seconds;
        if (seconds && g_page.focus == 0) MoveFocusTo(kLook + 1);
    }
    // the title: the page's name, or what a joining player is waiting for
    static const char* const kWaitTitles[] = {"", "WAITING FOR HOST", "CONNECTED", "JOINING GAME"};
    int phase = waiting ? std::clamp(game_net::JoinPhase(), 1, 3) : 0;
    std::string title = phase ? kWaitTitles[phase] : seconds ? "PLAYER 2 SETTINGS" : g_page.friends ? "PLAY WITH A FRIEND" : "DISPLAY AND CO-OP";
    if (phase == 1 && g_by_code) title = "CONNECTING";
    if (phase == 3 && game_net::HostNotStarted()) title = "CONNECTED";
    if (g_page.title && title != g_page.title_text) {
        SetText(ctx, g_page.title, title);
        if (phase) LOGR("menu: the joining player's screen says %s", title.c_str());
        g_page.title_text = title;
    }
    bool plain = seconds || waiting || g_page.friends;  // (no brightness on Player 2's page, the waiting screen, Play with a Friend)
    if (g_page.friends && !waiting && (g_page.focus == 0 || !Visible(g_page.focus - 1))) MoveFocusTo(kInvite + 1);
    if (plain != g_page.plain) {
        g_page.plain = plain;
        if (g_page.slider) Method(ctx, g_page.slider, kSetState, {plain ? kHidden : kIdle});
    }
    state(g_page.caption, plain ? kHidden : g_page.focus == 0 ? kHighlighted : kIdle);
    if (waiting != g_page.no_prompt) {
        g_page.no_prompt = waiting;
        if (g_page.prompt) Method(ctx, g_page.prompt, kSetState, {waiting ? kHidden : kIdle});
    }
    int slot = 0;
    for (int r = 0; r < kRows; r++) {
        if (!g_page.rows[r]) continue;
        bool focused = g_page.focus == r + 1, visible = Visible(r);
        if (visible) {  // the next free line (Player 2's page starts higher: there is no slider on it)
            float y = (seconds || g_page.friends ? g_page.slot_top - 36.0f : g_page.slot_top) + g_page.slot_step * float(slot++) - g_page.margin;
            if (std::fabs(y - g_page.row_y[r]) > 0.01f) WriteF(g_page.rows[r] + kY, y), g_page.row_y[r] = y;
        }
        state(g_page.rows[r], !visible ? kHidden : focused ? kHighlighted : kIdle);
        std::string text = visible ? RowText(r, focused) : std::string();
        if (visible && HasAddress(text)) g_address_shown = std::chrono::steady_clock::now();
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
        if (f == 0 ? !SecondsPage() && !Waiting() && !g_page.friends : (Visible(f - 1) && Focusable(f - 1))) break;  // (0 is the brightness: not on Player 2's page)
    }
    g_page.focus = f;
}

// once a frame while the page is up
void Tick(X86Context& ctx) {
    if (g_after_entry > 0) g_after_entry--;
    SettleHost(false);
#ifdef _WIN32
    bool invite_row = g_page.focus == kInvite + 1 && CoopChoice() == kHost, join_row = g_page.focus == kOnline + 1;
    bool code_row = invite_row || (join_row && CoopChoice() == kJoin);  // (a code of this PC's own to copy)
    bool takes_code = (join_row && !game_net::JoinState()) || (invite_row && (g_typing || game_meet::HostCode().empty()));
    {   // copy and paste on the code row: Ctrl+C and Ctrl+V, or a controller's X and Y
        static uint16_t held = 0;
        uint16_t now = input::HeldOnAnyController(), pressed = uint16_t(now & ~held);
        held = now;
        bool front = GetForegroundWindow() == static_cast<HWND>(app::Window());
        bool copy_key = Pressed('C') && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        constexpr uint16_t kPadX = 0x4000, kPadY = 0x8000;
        // (not once the host has answered: the page stays up under the host's picture while the friend plays, and
        // every press of his X, the game's "use", copied his code over whatever he had on the clipboard: 213 times in
        // the friend's log of 2026-11-26)
        if (code_row && front && game_net::JoinPhase() <= 1 && (copy_key || (pressed & kPadX))) CopyMine();
        if (takes_code && front && !g_typing && (pressed & kPadY)) Paste();
    }
    if (takes_code) Keys(g_typing);
#endif
    if (std::string host; game_meet::TakeHost(host)) {  // the meeting point knows the code: join that game
        Address() = host;  // (through Address(): its first use reads the settings file over it)
        g_by_code = true;
        JoinNow();
    }
    Refresh(ctx);
}

bool g_open_friends = false;  // the page being made is the main menu's "Play with a Friend"
void Build(X86Context& ctx, uint32_t screen) {
    g_page = {};
    g_page.friends = g_open_friends;
    g_after_entry = 0;
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
    // Now: title, instructions from 116, "Brightness" at 206, slider from 222, seven rows 20 apart from 261 (the
    // seventh is there for the online choices only; the others do not move when it comes and goes), button line;
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
    constexpr int kMostLines = 6;  // the section row and the five rows of the longer section
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
    g_section = 0;
    g_page.screen = screen;
    g_page.slider = slider;
    g_page.words = words;
    // the title is one line: its box is made wide enough for the longer name (the same middle)
    constexpr float kTitleWider = 80.0f;
    WriteF(title + kX, ReadF(title + kX) - kTitleWider / 2);
    WriteF(title + kW, ReadF(title + kW) + kTitleWider);
    SetText(ctx, title, "DISPLAY AND CO-OP");  // (the title font's P is drawn with a straight right side)
    g_page.title_text = "DISPLAY AND CO-OP";
    Refresh(ctx);
    LOGI("menu: Display and Co-op page built, rows from %.0f, %.1f apart (text margins %.1f and %.1f), %zu resolutions", top, step,
         margin_top, margin_bottom, Sizes().size());
}

// B on a screen. The menu manager ([0x391D64], once a frame 0x13E080) does not send B to the screen: it asks the
// screen in front whether B closes it (the screen's method +0x30; 0x14E980, "yes", for this page and most others),
// then asks its controller whether 0x421 was pressed (0x1A09C0 through its user object at +0x68) and closes the
// screen itself. While the host's address is entered, B gives the entry up and the page stays.
bool BackCloses(X86Context& ctx) {
    if (g_typing) {
        uint32_t manager = kernel::R32(0x391D64);
        if (manager && ThisCall(ctx, 0x1A09C0, kernel::R32(0x384BC8), {manager + 0x68, kBack, 1})) g_friendly ? FriendlyEnd(false) : EntryEnd(false);
        return false;
    }
    if (Waiting()) return false;  // (the waiting screen stays up: holding BACK stops the join)
    return g_after_entry == 0;
}
}  // namespace

bool Typing() { return g_typing; }
bool AddressOnScreen() { return std::chrono::steady_clock::now() - g_address_shown.load() < std::chrono::milliseconds(700); }

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
    constexpr uint32_t kOptions = 0x392158, kSwitches = kOptions + 0x50, kInverted = 0x10, kControllerScreen = 0x3414D0;
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
X86_EXTERN_FUNC(__imp__sub_0014BEE0);
X86_EXTERN_FUNC(__imp__sub_0014BDA0);
X86_EXTERN_FUNC(__imp__sub_0014C340);
X86_EXTERN_FUNC(__imp__sub_0014E980);
X86_EXTERN_FUNC(__imp__sub_0014AE60);
X86_EXTERN_FUNC(__imp__sub_00157A60);
X86_EXTERN_FUNC(__imp__sub_00154550);

// the Brightness screen's constructor (this, template): the page as the game makes it, then the display rows
X86_FUNC(sub_0014BF00) {
    uint32_t screen = ctx.ecx;
    __imp__sub_0014BF00(ctx, base);
    game_menu::Build(ctx, screen);
}

// ---- the main menu's "Play with a Friend" ----------------------------------------------------------------------------
// The main menu (its work each frame 0x157A60, this = the menu) has eleven entries (their text widgets at +0xC4)
// and does the chosen one's action when its way out has played: the action's number at +0x6C (0xF none), through
// the jump table at 0x1582C8. Action 6 is "Friends", the console's online friends list (0x154550, then 0x1CC250),
// done only while the online service's switch ([0x384B58] + 0xBC) is on: on this port it never is, and the entry
// did nothing. It is now "Play with a Friend": the entry says so, the switch reads as on for that one action, and
// the action opens the Display and Co-op page laid out for inviting and joining (g_page.friends). The page is made
// as the Options menu makes it (0x14B8D0): its template is property 0x22 of the Options menu's template, which is
// property 0x0D of the main menu's own resource (0x154570; the resource's handle at [0x376B98], 0x15E310 loads it).
namespace game_menu {
constexpr char kFriendsName[] = "Co-op";  // (the place holds a word as long as "Friends": "Play with a Friend" ran into "Archives")
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
void OpenFriends(X86Context& ctx, uint32_t menu) {
    kernel::CallGuest(ctx, 0x15E310, {});
    uint32_t options = Property(ctx, kernel::R32(0x376B98), 0x0D);
    uint32_t options_tmpl = options ? kernel::R32(options) : 0;
    uint32_t page_at = options_tmpl ? Property(ctx, options_tmpl, 0x22) : 0;
    uint32_t page_tmpl = page_at ? kernel::R32(page_at) : 0, manager = kernel::R32(0x391D64);
    LOGR("menu: Play with a Friend: the Options template is %08X, the page's %08X", options_tmpl, page_tmpl);
    if (!page_tmpl || !manager) return;
    Method(ctx, menu, kSetState, {2});
    uint32_t page = kernel::CallGuest(ctx, kAlloc, {0, 0xD4});
    if (!page) return;
    g_open_friends = true;
    ThisCall(ctx, 0x14BF00, page, {page_tmpl});
    g_open_friends = false;
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
}
// the main menu's action 6 (this = the menu): was the online friends list
X86_FUNC(sub_00154550) {
    game_menu::OpenFriends(ctx, ctx.ecx);
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
        if (g_typing) g_friendly ? FriendlyEnd(false) : EntryEnd(false);
        SettleHost(true);  // (the choice was made; the page was left before the moment was up)
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
    if (trace) LOGI("menu: event %X (focus %d%s%s)", event, g_page.focus, g_typing ? ", address entry" : "", g_after_entry ? ", entry ended" : "");
    if (g_after_entry) {  // the rest of the press that ended the address entry
        kernel::Return(ctx, 1, 1);
        return;
    }
    if (SecondsPage() && g_page.focus == 0 && (event == kLeft || event == kRight)) {  // (the brightness is Player 1's)
        kernel::Return(ctx, 1, 1);
        return;
    }
    if (g_typing && g_friendly) {  // a short code is being entered
        switch (event) {
            case kLeft: FriendlyMove(-1); break;
            case kRight: FriendlyMove(1); break;
            case kUp: FriendlyTurn(1); break;
            case kDown: FriendlyTurn(-1); break;
            case kAccept: FriendlyEnd(true); break;
            case kBack: FriendlyEnd(false); break;
        }
        Refresh(ctx);
        kernel::Return(ctx, 1, 1);
        return;
    }
    if (g_typing) {  // the address is being entered: every button is the entry's
        switch (event) {
            case kLeft: EntryMove(-1); break;
            case kRight: EntryMove(1); break;
            case kUp: EntryTurn(1); break;
            case kDown: EntryTurn(-1); break;
            case kAccept:
                EntryEnd(true);
                if (g_entry.for_friend) ExpectNow();
                else JoinNow();
                break;
            case kBack: EntryEnd(false); break;  // (B is asked for in BackCloses too: the manager takes it first)
        }
        Refresh(ctx);
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
        int row = g_page.focus - 1;
        if (row == kStop) {
            if (event == kAccept) SetCoop(kOff), MoveFocusTo(kInvite + 1), LOGI("menu: inviting was stopped");
        } else if (row == kInvite) {
            if (CoopChoice() != kHost) {  // start inviting: this game hosts from now on
                if (event == kAccept) SetCoop(kHost), SettleHost(true), LOGI("menu: inviting a friend");
            } else if (!game_meet::HostCode().empty()) {
                CopyMine();  // (the short code: nothing to enter, A copies it)
            } else if (event == kAccept && (!game_meet::Available() || game_meet::HostState() == 3)) {
                EntryStart(true);  // without a meeting point: the friend's address code
            }
        } else if (row != kOnline) Step(row, event == kLeft ? -1 : 1);
        else if (game_net::JoinState()) {}                 // (joining or joined: nothing to enter)
        else if (event != kAccept) {}
        else if (CoopChoice() != kJoin && (SetCoop(kJoin), false)) {}
        else if (game_meet::Available()) FriendlyStart(false);
        else if (int f[kFields]; event == kAccept && !Address().empty() && !Code(g_address.c_str(), f, true)) JoinNow();  // a host name
        else EntryStart(false);
        Refresh(ctx);
        kernel::Return(ctx, 1, 1);
        return;
    }
    __imp__sub_0014BDA0(ctx, base);
}

// "does B close this screen?" (this): the yes of most screens, and other yeses of theirs (one function for several
// methods). The manager's question about B comes from 0x13E23C.
X86_FUNC(sub_0014E980) {
    if (kernel::R32(ctx.esp) == 0x13E23F && ctx.ecx == game_menu::g_page.screen && ctx.ecx && !game_menu::BackCloses(ctx)) {
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_0014E980(ctx, base);
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
