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
std::string CallersOf(X86Context& ctx);  // (traces)
}
namespace game_net {  // net.cpp
bool HostHasPlayer();
int JoinState();  // 0 not joined, 1 asking the host, 2 playing in the host's game
std::string LastNote();
void ClearNote();
bool JoinAddress(const std::string& typed);
void Leave();
std::string PublicAddress();
std::string LocalAddress();
std::string OutsideAddress();  // this PC's code: its game port as seen from outside, "a.b.c.d:port" ("" not known yet)
int OutsideKind();             // 0 not known, 1 a code works with this router, 2 it does not (a port per destination)
bool HostExpect(const std::string& address);  // the host: a visitor will come from this code
bool HostExpecting();
int InternetState();  // hosting: 0 not yet, 1 looking the address up, 2 port open, 3 port to forward, 4 shared address, 5 none
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

enum Row { kResolution, kWindow, kShape, kRate, kVsync, kCoop, kOnline, kRows };
enum Coop { kOff, kSplit, kHost, kJoin, kCoops };

struct Page {
    uint32_t screen = 0, slider = 0;
    uint32_t caption = 0;  // "Brightness" over the slider: it shows when the slider is the control in focus
    uint32_t words = 0;    // the page's instructions; help for the co-op rows while one of them is in focus
    uint32_t words_id = 0;            // the game's own text in them (a string id), to put back,
    std::vector<uint16_t> words_own;  // or its characters when it has no id
    uint32_t rows[kRows] = {};
    std::string shown[kRows];  // what each row says now
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
constexpr const char* kCoopNames[kCoops] = {"Off", "Split screen", "Host online", "Join online"};

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
// Ctrl+V: an address anywhere in the clipboard's text (a friend's message with it in). Outside the digit entry a
// host name is taken too.
void Paste() {
    std::string text = Clipboard();
    for (size_t i = 0; i < text.size(); i++) {
        char before = i ? text[i - 1] : ' ';
        if (std::isdigit(static_cast<unsigned char>(before)) || before == '.') continue;
        int f[kFields];
        if (!Code(text.c_str() + i, f, false)) continue;
        if (g_typing) {
            std::copy(f, f + kFields, g_entry.field);
            g_entry.caret = kDigits - 1;
            g_entry.typed = 0;
        } else if (CoopChoice() == kHost) {
            g_friend = CodeText(f);
            ExpectNow();
        } else {
            g_address = CodeText(f);
            game_net::ClearNote();
        }
        LOGI("menu: a code was pasted");
        return;
    }
    if (g_typing || CoopChoice() == kHost) return;
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
    if (paste) Paste();
    if (!front || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) return;
    if (!entry) {  // a digit typed on the row starts a new address
        if (digit < 0) return;
        EntryStart(CoopChoice() == kHost);
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
    }
    return "";
}

constexpr const char* kLabels[kRows] = {"Resolution", "Window mode", "Picture shape", "Frame rate", "VSync", "Co-op", "Online"};

bool Visible(int row) { return row != kOnline || CoopChoice() == kHost || CoopChoice() == kJoin; }
bool Focusable(int row) { return row != kOnline || CoopChoice() == kJoin || CoopChoice() == kHost; }

// The row under Co-op. Hosting: this PC's code (its game port as seen from outside), or its address when no code
// is known; while the host enters the joiner's code, that. Joining: the host's code; once the join is on its way,
// this PC's own code, which the host needs.
std::string OnlineText(bool focused) {
    if (CoopChoice() == kHost) {
        if (g_typing) return "Friend's code:  " + EntryText();
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
    if (g_typing) return "Host's code:  " + EntryText();
    if (game_net::JoinState() == 1) {
        std::string mine = game_net::OutsideAddress();
        return "Your code:  " + (mine.empty() ? std::string("looking it up") : mine);
    }
    std::string address = Address().empty() ? "not set" : Address();
    if (game_net::JoinState()) return "Host's code:  " + address + "  (joined)";
    return focused ? "Host's code:  [ " + address + " ]" : "Host's code:  " + address;
}

std::string RowText(int row, bool focused) {
    if (row == kOnline) return OnlineText(focused);
    std::string v = Value(row);
    return std::string(kLabels[row]) + (focused ? ":  < " : ":  ") + v + (focused ? " >" : "");
}

// What the page's instructions say while a co-op row is in focus (empty: the game's own words about the slider).
// The box holds five lines of about thirty characters.
std::string Help() {
    int row = g_page.focus - 1;
    if (row != kCoop && row != kOnline) return "";
    switch (CoopChoice()) {
        case kSplit:
            return "Two players on this PC, side by side. Player 2 uses the second controller and comes in once a level is in play.";
        case kHost: {
            if (game_coop::g_host_failed) return "Cannot host: UDP port 27025 is in use on this PC. Is another copy of the game hosting?";
            if (game_net::HostHasPlayer()) return "Player 2 has joined your game and comes in once a level is in play.";
            if (g_typing)
                return "Enter the code your friend's screen shows. Up and down change the digit, A is done, B cancels. Ctrl+V pastes it.";
            if (game_net::HostExpecting()) return "Waiting for your friend to come in. This can take half a minute.";
            if (game_net::OutsideKind() == 2)
                return "Your router gives every connection a new port, so a code cannot work. Forward UDP port 27025 to this PC instead.";
            if (!game_net::OutsideAddress().empty())
                return "1. Send your code to your friend. 2. He enters it under Join online and sends you his. 3. Press A here and enter his code.";
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
            if (g_typing)
                return "Up and down change the digit, left and right move. A joins, B cancels. A keyboard can type the code, or paste it with Ctrl+V.";
            if (game_net::JoinState() == 1) {
                if (game_net::OutsideKind() == 2)
                    return "Your router gives every connection a new port, so a code cannot work. The host has to forward UDP port 27025. Hold BACK to stop.";
                return "WAITING FOR THE HOST. Send him YOUR code below. He enters it under Host online and you are in. Hold BACK to stop.";
            }
            if (game_net::JoinState()) return "Joined. To leave the game, hold BACK (keyboard: Backspace) for two seconds.";
            std::string note = game_net::LastNote();
            if (!note.empty()) {
                if (note.rfind("No answer", 0) == 0) note += " Check the code, and that the host entered yours.";
                return note;
            }
            if (row == kCoop) return "Join a friend's game: go down to Host's code. To leave a game later, hold BACK (Backspace) for two seconds.";
            return "Press A, enter the code your friend sees under Host online, then press A to join.";
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
        case kCoop: SetCoop(wrap(CoopChoice(), kCoops)); break;
    }
    app::Save();
    LOGI("menu: %s is now %s", kLabels[row], Value(row).c_str());
}

// ---- the page ---------------------------------------------------------------------------------------------------

// The widgets follow the state: focus, what each row says, and what the instructions say. Called on every input
// and once a frame (the online rows change by themselves); nothing is touched that already says the right thing.
void Refresh(X86Context& ctx) {
    if (!g_page.screen) return;
    auto state = [&](uint32_t widget, uint32_t want) {
        if (widget && kernel::R32(widget + kState) != want) Method(ctx, widget, kSetState, {want});
    };
    state(g_page.caption, g_page.focus == 0 ? kHighlighted : kIdle);
    for (int r = 0; r < kRows; r++) {
        if (!g_page.rows[r]) continue;
        bool focused = g_page.focus == r + 1, visible = Visible(r);
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
    do f = (f + dir + kRows + 1) % (kRows + 1);
    while (f != 0 && !(Visible(f - 1) && Focusable(f - 1)));
    g_page.focus = f;
}

// once a frame while the page is up
void Tick(X86Context& ctx) {
    if (g_after_entry > 0) g_after_entry--;
    SettleHost(false);
#ifdef _WIN32
    if (g_page.focus == kOnline + 1 && ((CoopChoice() == kJoin && !game_net::JoinState()) || CoopChoice() == kHost)) Keys(g_typing);
#endif
    Refresh(ctx);
}

void Build(X86Context& ctx, uint32_t screen) {
    g_page = {};
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
    constexpr float kCaptionGap = 7.0f, kRowsGap = 18.0f, kStep = 20.0f;
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
    float last_bottom = top + step * (kRows - 1) + kGlyphTop + kGlyphHeight;
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
        g_page.rows[r] = line(top + step * float(r));
        if (!g_page.rows[r]) return;
    }
    g_page.screen = screen;
    g_page.slider = slider;
    g_page.words = words;
    // the title is one line: its box is made wide enough for the longer name (the same middle)
    constexpr float kTitleWider = 80.0f;
    WriteF(title + kX, ReadF(title + kX) - kTitleWider / 2);
    WriteF(title + kW, ReadF(title + kW) + kTitleWider);
    SetText(ctx, title, "DISPLAY AND CO-OP");  // (the title font's P is drawn with a straight right side)
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
        if (manager && ThisCall(ctx, 0x1A09C0, kernel::R32(0x384BC8), {manager + 0x68, kBack, 1})) EntryEnd(false);
        return false;
    }
    return g_after_entry == 0;
}
}  // namespace

bool Typing() { return g_typing; }

// The pause screen's title says who paused, in co-op: "PLAYER 2 PAUSED" where the game writes "PAUSE". The title is
// one of the screen's own text widgets (the newest screen of the menu manager [0x391D64]; screens at +0x50, count
// +0x58; a screen's widgets the same way; a text widget's words at +0x68, their count with the end at +0x6C), so
// the game draws it, in its font, on its strip of paper, once for both players. pauser 0: not paused.
void PauseTitle(X86Context& ctx, int pauser) {
    static uint32_t titled = 0;  // the widget that was changed
    static int shown = 0;
    if (!pauser) {
        titled = 0, shown = 0;  // (the screen is gone with its widgets; the next pause makes new ones)
        return;
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

// the Brightness screen's constructor (this, template): the page as the game makes it, then the display rows
X86_FUNC(sub_0014BF00) {
    uint32_t screen = ctx.ecx;
    __imp__sub_0014BF00(ctx, base);
    game_menu::Build(ctx, screen);
}

// its destructor: the rows go with the screen's other children
X86_FUNC(sub_0014BEE0) {
    using namespace game_menu;
    if (ctx.ecx == g_page.screen) {
        if (std::getenv("S2_MENU_TRACE")) LOGI("menu: the page ends, called from %s", game_coop::CallersOf(ctx).c_str());
        if (g_typing) EntryEnd(false);
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
        if (row != kOnline) Step(row, event == kLeft ? -1 : 1);
        else if (CoopChoice() == kHost) EntryStart(true);  // the joiner's code
        else if (game_net::JoinState()) {}                 // (joining or joined: nothing to enter)
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
