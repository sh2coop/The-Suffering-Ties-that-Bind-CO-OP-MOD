// Online co-op: one game is played by two people over the network. The host's game runs both players (split
// screen, coop.cpp); the other player's game becomes his screen and controller.
//
//   HOST   this game takes a remote player: when one joins, Player 2 comes in and the right half of the picture,
//          Player 2's view, is sent to him with the game's sound. His controller arrives as controller 2.
//   JOIN   this game shows the host's picture of Player 2 and sends this controller (or keyboard) to the host.
//          The joiner's own game idles underneath. Hold BACK for two seconds to leave.
//
// The transport is the Batman Begins port's (the same runtime), with its own packet mark and ports.
// Everything is UDP on two ports (little endian, 'S2C1' then a type byte):
//   27026 broadcast, host -> all   type 3: game port u16, host name (32 bytes)                      every second
//   27025 client -> host           type 1: seq u32, buttons u16, analog[8], lx ly rx ry i16          60 a second
//   host -> client                 type 2: frame u32, w u16, h u16, parts u16, part u16, bytes u16, JPEG chunk
//   host -> client                 type 4: seq u32, frames u16, stereo i16 at 24 kHz (the host's sound)  100 a second
//   client -> host                 type 5: the player is leaving                                    three times
// A client counts as connected while its controller packets keep coming (kClientTimeout); the host as reachable
// while its frames do (kHostTimeout). The joiner plays the host's sound in place of its own game's.
//
// Direct over the internet, no virtual LAN software: hosting asks the router to forward UDP 27025 here (UPnP,
// Windows' NAT traversal API) and looks up this connection's public address (the router's answer, else
// api.ipify.org). The other player joins that address: only the host needs the port open, the joiner's router lets
// the host's answers back in. An address the router reports that is itself private (CGNAT, a second router)
// cannot be reached from outside: the status says so. The address joined last is kept as join_ip in settings.toml;
// lines of coop_hosts.txt beside the exe list more hosts.
//
// In the game the two are choices of the Co-op row on the Display and Co-op page (menu.cpp): Host online, and Join
// online with the host's address entered there.
// Tests: S2_COOP_HOST=1 hosts from the start; S2_COOP_JOIN=<address> joins that address a few seconds after start.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <natupnp.h>
#include <winhttp.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

namespace game_coop {
extern std::atomic<int> g_ammo2_rounds, g_ammo2_clip, g_ammo2_more;  // Player 2's ammunition (coop.cpp, DetailFrame)
void RemotePlayer(bool connected);  // the host's remote player came or went
bool SplitActive();                 // player two is in and the screen is split
bool StreamView();                  // player two is in and the frames handed over are his view, whole
bool StreamHasLayer();              // ... with the game's own 2D for him in them (its crosshair, messages, pictures over the screen)
extern uint32_t g_p2_invert, g_p2_speed;  // this PC's Player 2 look settings
void RemoteLookFrom(uint32_t invert, uint32_t speed);  // a joined player's, as his PC sends them
void RemoteLookForget();
bool InLevel();  // a level is in play on this PC
bool SceneNow();  // the picture is one view, Player 1's: a scene, or the story holds him
extern uint32_t g_p2_volume;  // this PC's volume for a host's sound, per cent
void RemoteVolumeFrom(uint32_t volume);  // a joined player's, as his PC sends it
}

namespace game_detail {  // detail.cpp: the record of every frame, and the session's pictures folder
bool On();
std::string PicturesDir();
void Line(const char* fmt, ...);
}
namespace game_record {  // record.cpp
uint32_t Marks(std::string* why);  // how many marks (full-size pictures of the window) there have been
}

namespace game_net {
uint32_t g_picture_rows = 1080;  // the setting friend_picture_rows: the most rows of the joined player's picture
namespace {
#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
void Close(Sock s) { closesocket(s); }
#else
using Sock = int;
constexpr Sock kBad = -1;
void Close(Sock s) { close(s); }
#endif
using Clock = std::chrono::steady_clock;
constexpr uint16_t kGamePort = 27025, kBeaconPort = 27026;
// ms between frames to the client. 28, not 33: the host draws a frame every 16.7 ms, and "at least 33 ms since the
// last one sent" let every second frame through only when the timer agreed, else the third: pictures 33 or 50 ms
// apart, 25 a second, which the joined player of the first online session saw as stutter (his copy counted waits
// of up to 74 ms on one PC). With 28 it is every second frame, evenly, 30 a second (every fourth at 120).
constexpr int kFrameInterval = 28;
constexpr int kJpegQuality = 70;
// The joined player's picture at the size of his window ("he should be playing at the resolution he has the window
// at", 2026-11-26; until then it was the console's 640 x 480, made larger on his PC). Player 2's view is taken from
// the host's render target at up to as many rows as the joined player's window has, the host's target has, and the
// setting allows (friend_picture_rows in the settings file, 1080 unless set; 480 is the old picture). A picture of
// 1920 x 1440 is about 100 kB, 25 to 50 Mbit a second: the joined game tells the host how many pictures it lost,
// and the host goes down a size when pictures get lost and up again when none are.
constexpr uint32_t kRowSteps[] = {480, 600, 720, 900, 1080, 1200, 1440, 1600, 2160};
constexpr int kRepairGroup = 12;  // parts of a large picture that share one repair packet (packet type 11)
constexpr size_t kChunk = 1200;
constexpr size_t kFrameHead = 23;         // a frame packet's bytes before the JPEG chunk
constexpr int kClientTimeout = 3000;   // ms without pad packets: the remote player left
constexpr int kHostTimeout = 60000;    // ms without frames (a host loading a level stalls a while; 15 s until
                                       // 2026-11-25, which a slow disk can pass): the joiner leaves
constexpr int kConnectTimeout = 90000;  // ms without a first frame: the host is not answering (long: over the
                                       // internet the host first has to enter this side's code)
constexpr int kBeaconTimeout = 4000;   // ms without a beacon: the host drops off the list
constexpr int kLeaveHold = 2000;       // ms of BACK held to leave a game
constexpr uint32_t kAudioFrames = 240;   // stereo frames per sound packet: 10 ms at 24 kHz (half the mixer's rate)
constexpr size_t kAudioStart = 4 * 2 * kAudioFrames;  // samples buffered before the joiner plays: 40 ms
constexpr size_t kAudioLow = 3 * 2 * kAudioFrames, kAudioHigh = 8 * 2 * kAudioFrames;  // the band it is kept in
constexpr size_t kAudioMost = 30 * 2 * kAudioFrames;  // more than this (300 ms: a stall) and the oldest go

struct Pad {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Startup() {
#ifdef _WIN32
    static bool done = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)done;
#endif
}

void Timeout(Sock s, int ms) {
#ifdef _WIN32
    DWORD t = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

Sock Bind(uint16_t port, bool reuse) {
    Startup();
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBad) return kBad;
    int one = 1;
    if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        Close(s);
        return kBad;
    }
    Timeout(s, 100);
    return s;
}

std::string HostName() {
    char b[64] = {};
    if (gethostname(b, sizeof b - 1) != 0) std::strcpy(b, "BATMAN");
    std::string s(b);
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s.substr(0, 20);
}

// the broadcast address of every IPv4 network this machine is on (LAN, Wi-Fi, Hamachi ...), and loopback
std::vector<uint32_t> Broadcasts() {
    std::vector<uint32_t> out{htonl(INADDR_LOOPBACK), htonl(INADDR_BROADCAST)};
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             list, &size) != NO_ERROR)
        return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            uint32_t bits = u->OnLinkPrefixLength;
            if (bits == 0 || bits >= 32 || (ip >> 24) == 127) continue;
            uint32_t mask = bits ? ~0u << (32 - bits) : 0;
            out.push_back(htonl(ip | ~mask));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_BROADCAST) || !a->ifa_broadaddr) continue;
        out.push_back(reinterpret_cast<sockaddr_in*>(a->ifa_broadaddr)->sin_addr.s_addr);
    }
    freeifaddrs(list);
#endif
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string IpText(uint32_t ip_be) {
    in_addr a;
    a.s_addr = ip_be;
    char b[32] = "?";
    inet_ntop(AF_INET, &a, b, sizeof b);
    return b;
}

// the last thing that happened, for the menu's status line ("CONNECTION TO X LOST" ...)
// The record of the connection (asked for 2026-11-26), both sides, every five seconds while two are connected.
// The joiner asks for an answer to every thirtieth controller packet (the host sends its number back, packet type
// 9): the time there and back. A copy from before this neither asks nor answers, and nothing is lost by that.
struct NetRecord {
    std::atomic<uint32_t> pads{0}, pads_missing{0}, pads_late{0}, pad_gap_ms{0};         // host: controller packets in
    std::atomic<uint32_t> pictures{0}, picture_bytes{0}, encode_us{0}, encode_worst_us{0};  // host: pictures out
    std::atomic<uint32_t> sound_dry{0};                                                   // joiner: the sound ran out of data
    std::atomic<uint32_t> sounds{0};                                                      // sound packets out (host) or in (joiner)
    std::atomic<uint32_t> pads_out{0}, bytes_in{0};                                       // joiner
    std::atomic<uint32_t> trip_us{0}, trip_worst_us{0}, trips{0};                         // joiner: there and back
    std::atomic<uint32_t> asked_seq{0};
    std::atomic<int64_t> asked_at{0};  // (microseconds on the steady clock)
} g_rec;
int64_t MicrosNow() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Most(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t was = a.load();
    while (v > was && !a.compare_exchange_weak(was, v)) {}
}
// what a controller packet holds, when it changes: the joined player's buttons and sticks as the host got them
// (the sticks since 2026-11-26: the log of that day's session could not say whether the joined player had pushed
// the stick while he held a box for 18 s; named as record.cpp names a controller's)
const char* StickWay(int x, int y, bool was_out) {
    static const char* const kNames[9] = {"the middle", "up", "up and right", "right", "down and right", "down", "down and left", "left", "up and left"};
    double r = std::sqrt(double(x) * x + double(y) * y) / 32767.0;
    if (r < (was_out ? 0.25 : 0.40)) return kNames[0];
    double a = std::atan2(double(x), double(y)) * 57.29578;  // 0 up, 90 right
    if (a < 0) a += 360;
    return kNames[1 + int((a + 22.5) / 45.0) % 8];
}
void RemoteInput(uint16_t buttons, const uint8_t analog[8], int lx, int ly, int rx, int ry) {
    static uint16_t before = 0;
    static bool held[8] = {};
    {
        static const char* left = nullptr;
        static const char* right = nullptr;
        const char* middle = StickWay(0, 0, true);
        const char* l = StickWay(lx, ly, left && left != middle);
        const char* r = StickWay(rx, ry, right && right != middle);
        if (left && l != left) LOGI("input: the joined player: left stick %s", l);
        if (right && r != right) LOGI("input: the joined player: right stick %s", r);
        left = l, right = r;
    }
    static const struct {
        uint16_t bit;
        const char* name;
    } kButtons[] = {{0x01, "D-pad up"}, {0x02, "D-pad down"}, {0x04, "D-pad left"}, {0x08, "D-pad right"},
                    {0x10, "START"},    {0x20, "BACK"},       {0x40, "left stick click"}, {0x80, "right stick click"}};
    static const char* const kAnalog[8] = {"A", "B", "X", "Y", "black", "white", "left trigger", "right trigger"};
    for (const auto& b : kButtons)
        if ((buttons ^ before) & b.bit) LOGI("input: the joined player: %s %s", b.name, (buttons & b.bit) ? "down" : "up");
    before = buttons;
    for (int i = 0; i < 8; i++) {
        bool now = analog[i] > (held[i] ? 30 : 60);
        if (now != held[i]) LOGI("input: the joined player: %s %s", kAnalog[i], now ? "down" : "up");
        held[i] = now;
    }
}
constexpr int kJoiningMs = 2000;  // the joiner's "joining" screen before the host's picture is shown
std::atomic<Clock::time_point> g_host_waits{};  // when the host last said "not in a level yet"
// ... and when it last said "a scene is on: what you see is Player 1's view" (packet type 10), and since when it has
// been saying so. While a scene plays or the story holds Player 1, the joined player's own view is not sent and his
// controller does nothing; nothing told him, and it read as a camera that had stopped ("there was a bug where his
// camera became fixed", 2026-11-26). His screen now says so for the first seconds of each.
std::atomic<Clock::time_point> g_host_scene{}, g_host_scene_since{};
std::mutex g_note_lock;
std::string g_note;
void Note(const std::string& n) {
    std::lock_guard l(g_note_lock);
    g_note = n;
}

// ---- internet: the router's port and the public address ----------------------------------------------------------
struct Internet {
    std::mutex lock;
    std::string address;            // public IPv4 as text, empty until known
    std::string status;             // for the log
    std::string local;              // this PC's address on its own network
    int state = 0;                  // for the menu: 0 not hosting, 1 looking the address up, 2 the router opened
                                    // the port, 3 it did not (the port has to be forwarded), 4 the provider shares
                                    // the address (no way in from outside), 5 no internet address found
    bool mapped = false;            // our UPnP forward is on the router
    std::atomic<uint32_t> gen{0};   // bumped on every Host on/off: an older lookup's result is dropped
} g_net;

bool PrivateIp(uint32_t ip) {  // host order
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100u << 2 | 1) ||
           (ip >> 24) == 127 || (ip >> 16) == 0xA9FE;
}

// this machine's address on the network that leads to the internet (no packet is sent)
uint32_t LocalIp() {
    Startup();
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBad) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = me.sin_addr.s_addr;
    }
    Close(s);
    return ip;
}

#ifdef _WIN32
const CLSID kUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
const IID kIUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
std::string Narrow(BSTR b) {
    std::string out;
    for (UINT i = 0, n = b ? SysStringLen(b) : 0; i < n; i++) out += char(b[i] < 128 ? b[i] : '?');
    return out;
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// the router's port mappings, or null (no UPnP router, or UPnP off on it); COM is set up on the calling thread
IStaticPortMappingCollection* Mappings() {
    IUPnPNAT* nat = nullptr;
    if (FAILED(CoCreateInstance(kUPnPNAT, nullptr, CLSCTX_ALL, kIUPnPNAT, reinterpret_cast<void**>(&nat))) || !nat)
        return nullptr;
    IStaticPortMappingCollection* maps = nullptr;
    nat->get_StaticPortMappingCollection(&maps);  // null when the router does not answer
    nat->Release();
    return maps;
}

// forward UDP port to this machine; the router's public address when it says
bool MapPort(uint16_t port, uint32_t local, std::string& external) {
    IStaticPortMappingCollection* maps = Mappings();
    if (!maps) return false;
    BSTR proto = SysAllocString(L"UDP");
    BSTR client = SysAllocString(Wide(IpText(local)).c_str());
    BSTR desc = SysAllocString(L"Batman Begins co-op");
    IStaticPortMapping* m = nullptr;
    maps->Remove(port, proto);  // a stale one (another PC, an old address) would make Add fail
    HRESULT hr = maps->Add(port, proto, port, client, VARIANT_TRUE, desc, &m);
    if (SUCCEEDED(hr) && m) {
        BSTR ext = nullptr;
        if (SUCCEEDED(m->get_ExternalIPAddress(&ext)) && ext) external = Narrow(ext);
        if (ext) SysFreeString(ext);
        m->Release();
    }
    SysFreeString(proto);
    SysFreeString(client);
    SysFreeString(desc);
    maps->Release();
    return SUCCEEDED(hr);
}

void UnmapPort(uint16_t port) {
    IStaticPortMappingCollection* maps = Mappings();
    if (!maps) return;
    BSTR proto = SysAllocString(L"UDP");
    maps->Remove(port, proto);
    SysFreeString(proto);
    maps->Release();
}

// the public address as a web service sees it
std::string AskPublicIp() {
    const wchar_t* hosts[] = {L"api.ipify.org", L"checkip.amazonaws.com"};
    for (const wchar_t* host : hosts) {
        std::string body;
        HINTERNET h = WinHttpOpen(L"BatmanBegins", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) continue;
        WinHttpSetTimeouts(h, 3000, 3000, 3000, 3000);
        HINTERNET c = WinHttpConnect(h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)
                        : nullptr;
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            char buf[128];
            DWORD got = 0;
            while (body.size() < 64 && WinHttpReadData(r, buf, sizeof buf, &got) && got) body.append(buf, got);
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(h);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
        in_addr a{};
        if (inet_pton(AF_INET, body.c_str(), &a) == 1) return body;
    }
    return "";
}
#else
bool MapPort(uint16_t, uint32_t, std::string&) { return false; }
void UnmapPort(uint16_t) {}
std::string AskPublicIp() { return ""; }
#endif

void SetInternet(uint32_t gen, const std::string& address, const std::string& status, bool mapped, int state) {
    std::lock_guard l(g_net.lock);
    if (g_net.gen != gen) return;
    g_net.address = address;
    g_net.status = status;
    g_net.mapped = mapped;
    g_net.state = state;
}

// hosting: open the port on the router, find the public address (a thread: the router can take seconds to answer)
void OpenToInternet() {
    uint32_t gen = ++g_net.gen;
    SetInternet(gen, "", "LOOKING UP YOUR ADDRESS", false, 1);
    std::thread([gen] {
#ifdef _WIN32
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
#endif
        uint32_t local = LocalIp();
        {
            std::lock_guard l(g_net.lock);
            if (g_net.gen == gen) g_net.local = local ? IpText(local) : "";
            if (const char* fake = std::getenv("COOP_FAKE_LOCAL")) g_net.local = fake;  // harness: pictures of the menu
        }
        std::string external;
        bool mapped = local && MapPort(kGamePort, local, external);
        std::string web = AskPublicIp();
        if (const char* fake = std::getenv("COOP_FAKE_ADDRESS")) web = fake;  // harness: the menu with an address
        in_addr ea{};
        bool ext_ok = !external.empty() && inet_pton(AF_INET, external.c_str(), &ea) == 1;
        bool behind = ext_ok && PrivateIp(ntohl(ea.s_addr));  // the router itself sits behind another (CGNAT)
        std::string address = web.empty() ? (ext_ok && !behind ? external : "") : web;
        std::string status;
        int state;
        if (address.empty()) status = "NO INTERNET ADDRESS FOUND", state = 5;
        else if (behind) status = "SHARED ADDRESS: NO WAY IN FROM OUTSIDE", state = 4;
        else if (mapped) status = "PORT OPENED ON YOUR ROUTER", state = 2;
        else status = "OPEN UDP 27025 ON YOUR ROUTER", state = 3;
        if (const char* fake = std::getenv("COOP_FAKE_STATE")) state = std::atoi(fake);  // harness: the menu's texts
        if (mapped && g_net.gen != gen) UnmapPort(kGamePort);  // hosting stopped meanwhile
        LOGR("coop: internet: local %s, router %s%s, public %s: %s", local ? IpText(local).c_str() : "?",
             mapped ? "forwarded UDP 27025" : "no UPnP forward", external.empty() ? "" : (" (says " + external + ")").c_str(),
             web.empty() ? "?" : web.c_str(), status.c_str());
        SetInternet(gen, address, status, mapped && g_net.gen == gen, state);
#ifdef _WIN32
        if (com) CoUninitialize();
#endif
    }).detach();
}

void CloseToInternet() {
    bool mapped;
    {
        std::lock_guard l(g_net.lock);
        ++g_net.gen;
        mapped = g_net.mapped;
        g_net.address.clear();
        g_net.status.clear();
        g_net.local.clear();
        g_net.state = 0;
        g_net.mapped = false;
    }
    if (!mapped) return;
    std::thread([] {
#ifdef _WIN32
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
#endif
        UnmapPort(kGamePort);
        LOGR("coop: internet: router forward of UDP 27025 removed");
#ifdef _WIN32
        if (com) CoUninitialize();
#endif
    }).detach();
}

// ---- host -------------------------------------------------------------------------------------------------------
// The game closed while hosting (the window's X, Alt+F4): the router's forward must not stay behind. StopHost takes
// it away when hosting is switched off in the menu; this does the same when the program ends.
struct UnmapAtExit {
    ~UnmapAtExit() {
        bool mapped;
        {
            std::lock_guard l(g_net.lock);
            mapped = g_net.mapped;
            g_net.mapped = false;
        }
        if (!mapped) return;
#ifdef _WIN32
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
#endif
        UnmapPort(kGamePort);
        LOGR("coop: internet: router forward of UDP 27025 removed (the game is closing)");
#ifdef _WIN32
        if (com) CoUninitialize();
#endif
    }
} g_unmap_at_exit;

struct HostState {
    Sock sock = kBad;
    std::atomic<bool> run{false};
    std::thread listen, encode, beacon;
    std::mutex lock;
    sockaddr_in client{};
    bool have_client = false, connected = false;
    Clock::time_point last_input;
    Pad pad;
    uint32_t seq = 0;
    // the newest frame from the renderer, for the encoder
    std::mutex frame_lock;
    std::condition_variable frame_cv;
    std::vector<uint8_t> raw;    // BGRA as the renderer handed it over, rows raw_w * 4 apart
    uint32_t raw_w = 0, raw_h = 0;
    std::atomic<float> frame_aspect{0};  // the shape that picture is shown in on the host (width / height)
    bool frame_new = false;
    Clock::time_point last_sent;
    sockaddr_in punch{};  // a visitor's outside address, sent to until punch_until (HostBeacon)
    Clock::time_point punch_until{};
    // the joined game: its window's picture area, the share of pictures it lost in its last two seconds (per cent),
    // whether it knows large pictures at all (a copy from before 2026-11-26 sends none of this)
    std::atomic<uint32_t> client_w{0}, client_h{0}, client_lost{0};
    std::atomic<bool> client_large{false};
    std::atomic<uint32_t> rows{480};  // the rows of the picture being sent now
} g_host;

// The rows of Player 2's picture: up when no picture was lost for a while, down when they are. Called with every
// controller packet of the joined game (about 120 a second).
void PictureRows() {
    static Clock::time_point changed{}, said{};
    static uint32_t before = 0;
    uint32_t most = std::min<uint32_t>(std::max<uint32_t>(g_picture_rows, 480), std::max<uint32_t>(g_host.client_h.load(), 480));
    if (!g_host.client_large.load()) most = 480;
    uint32_t rows = std::clamp<uint32_t>(g_host.rows.load(), 480, most), lost = g_host.client_lost.load();
    Clock::time_point now = Clock::now();
    static bool large_before = false;
    bool large = g_host.client_large.load();
    if (large && !large_before) rows = std::min<uint32_t>(most, 720), changed = now;  // (he has just come in: not from 480 up)
    large_before = large;
    auto step = [&](int dir) {
        int at = 0;
        for (int i = 0; i < int(std::size(kRowSteps)); i++)
            if (kRowSteps[i] <= rows) at = i;
        at = std::clamp(at + dir, 0, int(std::size(kRowSteps)) - 1);
        return std::min(kRowSteps[at], most);
    };
    if (lost >= 4 && now - changed > std::chrono::seconds(3)) rows = step(-1), changed = now;
    else if (lost == 0 && rows < most && now - changed > std::chrono::seconds(4)) rows = step(+1) > rows ? step(+1) : most, changed = now;
    g_host.rows = rows;
    gpu::g_stream_snapshot_rows = rows > 480 ? rows : 0;
    if (rows != before && now - said > std::chrono::milliseconds(500)) {
        LOGR("net: the joined player's picture is now %u rows (his window %u x %u, the setting allows %u; he lost %u%% of the last pictures)", rows,
             g_host.client_w.load(), g_host.client_h.load(), g_picture_rows, lost);
        before = rows, said = now;
    }
}

bool ClientConnected() {
    std::lock_guard l(g_host.lock);
    return g_host.have_client && Clock::now() - g_host.last_input < std::chrono::milliseconds(kClientTimeout);
}

// ---- how this PC's game port looks from outside (STUN) -------------------------------------------------------------
// For playing over the internet without forwarding a port, both routers have to let the two PCs' packets through
// ("hole punching"): each PC sends to the other's public address and port at the same time. That needs the public
// address AND port of the game's own socket, which only a server outside can tell: a STUN server answers a 20-byte
// question (RFC 5389 binding request) with the address it saw the question come from. Two different servers are
// asked from the game's socket: when both see the same port, the router gives this socket one port for everyone
// and hole punching can work; when the ports differ, it gives a new port for every destination ("symmetric") and
// only a forwarded port (or UPnP, or a virtual LAN) gets a visitor in. Asked again every 20 s, which also keeps
// the router's entry alive. This is the groundwork (2026-11-25); the join code that uses it is not built yet.
struct Stun {
    std::mutex lock;
    sockaddr_in server[2] = {};
    bool resolved[2] = {false, false};
    uint32_t ip[2] = {0, 0};    // network order
    uint16_t port[2] = {0, 0};  // host order
    std::string said;           // the last line logged (a change is logged again)
} g_stun;
constexpr struct {
    const char* host;
    const char* port;
} kStunServers[2] = {{"stun.l.google.com", "19302"}, {"stun.cloudflare.com", "3478"}};

void StunForget() {
    std::lock_guard l(g_stun.lock);
    g_stun.ip[0] = g_stun.ip[1] = 0;
    g_stun.port[0] = g_stun.port[1] = 0;
    g_stun.said.clear();
}
void StunAsk(Sock s) {
    for (int i = 0; i < 2; i++) {
        sockaddr_in to{};
        {
            std::lock_guard l(g_stun.lock);
            if (!g_stun.resolved[i]) {
                addrinfo hints{}, *found = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                if (getaddrinfo(kStunServers[i].host, kStunServers[i].port, &hints, &found) == 0 && found) {
                    std::memcpy(&g_stun.server[i], found->ai_addr, sizeof(sockaddr_in));
                    g_stun.resolved[i] = true;
                }
                if (found) freeaddrinfo(found);
            }
            if (!g_stun.resolved[i]) continue;
            to = g_stun.server[i];
        }
        uint8_t ask[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42, 'S', '2', uint8_t(i)};
        for (int k = 11; k < 20; k++) ask[k] = uint8_t(std::rand());
        sendto(s, reinterpret_cast<const char*>(ask), sizeof ask, 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    }
}

// a STUN server's answer on the game's socket? (binding success, our own marks in the transaction id)
bool StunAnswer(const uint8_t* b, int n) {
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01 || b[4] != 0x21 || b[5] != 0x12 || b[6] != 0xA4 || b[7] != 0x42 || b[8] != 'S' || b[9] != '2' || b[10] > 1)
        return false;
    int which = b[10];
    for (int at = 20; at + 4 <= n;) {
        int type = b[at] << 8 | b[at + 1], size = b[at + 2] << 8 | b[at + 3];
        if (type == 0x0020 && size >= 8 && at + 12 <= n && b[at + 5] == 0x01) {  // XOR-MAPPED-ADDRESS, IPv4
            uint16_t port = uint16_t((b[at + 6] << 8 | b[at + 7]) ^ 0x2112);
            uint8_t ip[4] = {uint8_t(b[at + 8] ^ 0x21), uint8_t(b[at + 9] ^ 0x12), uint8_t(b[at + 10] ^ 0xA4), uint8_t(b[at + 11] ^ 0x42)};
            std::lock_guard l(g_stun.lock);
            std::memcpy(&g_stun.ip[which], ip, 4);
            g_stun.port[which] = port;
            if (g_stun.port[0] && g_stun.port[1]) {
                bool same = g_stun.port[0] == g_stun.port[1] && g_stun.ip[0] == g_stun.ip[1];
                // (the address itself is not written to the log: logs are sent around)
                std::string line = same ? "the game's port is seen from outside as port " + std::to_string(g_stun.port[0]) +
                                              " by both servers: one port for everyone (hole punching can work)"
                                        : "the game's port is seen from outside as port " + std::to_string(g_stun.port[0]) + " by one server and " +
                                              std::to_string(g_stun.port[1]) + " by the other: a new port for every destination (only a forwarded port lets a visitor in)";
                if (line != g_stun.said) LOGR("coop: internet: %s", line.c_str()), g_stun.said = line;
            }
            return true;
        }
        at += 4 + ((size + 3) & ~3);
    }
    return true;
}

void HostListen() {
    uint8_t buf[512];
    while (g_host.run) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n = int(recvfrom(g_host.sock, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
        if (StunAnswer(buf, n)) continue;
        bool now = ClientConnected();
        {
            std::lock_guard l(g_host.lock);
            if (g_host.connected && !now) {
                g_host.connected = false;
                g_host.have_client = false;
                LOGR("coop: the remote player left");
                game_coop::RemotePlayer(false);
            }
        }
        // the remote player says he is leaving (type 5): out at once, not after kClientTimeout without packets
        if (n >= 5 && std::memcmp(buf, "S2C1", 4) == 0 && buf[4] == 5) {
            std::lock_guard l(g_host.lock);
            if (g_host.connected && from.sin_addr.s_addr == g_host.client.sin_addr.s_addr && from.sin_port == g_host.client.sin_port) {
                g_host.connected = false;
                g_host.have_client = false;
                g_host.client_large = false;
                LOGR("coop: the remote player left (he said so)");
                game_coop::RemotePlayer(false);
            }
            continue;
        }
        if (n < 27 || std::memcmp(buf, "S2C1", 4) != 0 || buf[4] != 1) continue;
        uint32_t seq;
        Pad p;
        std::memcpy(&seq, buf + 5, 4);
        std::memcpy(&p.buttons, buf + 9, 2);
        std::memcpy(p.analog, buf + 11, 8);
        std::memcpy(&p.lx, buf + 19, 2);
        std::memcpy(&p.ly, buf + 21, 2);
        std::memcpy(&p.rx, buf + 23, 2);
        std::memcpy(&p.ry, buf + 25, 2);
        if (n >= 29) game_coop::RemoteLookFrom(buf[27], buf[28]);  // (an older copy sends 27 bytes: as the game has it)
        if (n >= 30) game_coop::RemoteVolumeFrom(buf[29]);
        if (n >= 36) {  // (since 2026-11-26: his window, and how many pictures he lost)
            uint16_t cw, ch;
            std::memcpy(&cw, buf + 30, 2), std::memcpy(&ch, buf + 32, 2);
            g_host.client_w = cw, g_host.client_h = ch, g_host.client_lost = buf[34], g_host.client_large = buf[35] >= 1;
        } else {
            g_host.client_large = false;
        }
        PictureRows();
        std::lock_guard l(g_host.lock);
        bool same = g_host.have_client && from.sin_addr.s_addr == g_host.client.sin_addr.s_addr &&
                    from.sin_port == g_host.client.sin_port;
        // A friend who comes while the host is still in its menus is let in and watches them, as if on the same
        // couch (the study "Co-op joining: the easy way", 2026-11-26; from 2026-11-25 until then he was kept
        // waiting outside). His PC is told once a second that no level is in play here (packet type 8): it writes
        // that over the picture, and his controller does nothing here until a level is (RemotePad).
        if (!game_coop::InLevel()) {
            static Clock::time_point told{};
            if (Clock::now() - told > std::chrono::milliseconds(1000)) {
                told = Clock::now();
                const uint8_t wait[5] = {'S', '2', 'C', '1', 8};
                sendto(g_host.sock, reinterpret_cast<const char*>(wait), sizeof wait, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
                static bool said = false;
                if (!g_host.connected || !said) said = true, LOGI("coop: a friend is watching this game's menus (no level is in play here yet)");
            }
        }
        if (game_coop::InLevel() && game_coop::SceneNow()) {
            static Clock::time_point told{};
            if (Clock::now() - told > std::chrono::milliseconds(1000)) {
                told = Clock::now();
                const uint8_t scene[6] = {'S', '2', 'C', '1', 10, 1};
                sendto(g_host.sock, reinterpret_cast<const char*>(scene), sizeof scene, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
            }
        }
        if (!same) {
            if (g_host.have_client && Clock::now() - g_host.last_input < std::chrono::milliseconds(kClientTimeout))
                continue;  // one remote player at a time
            g_host.client = from;
            g_host.have_client = true;
            g_host.seq = 0;
        }
        {   // the record: how the controller packets come
            static Clock::time_point last{};
            Clock::time_point t = Clock::now();
            if (g_host.connected && same && last != Clock::time_point{})
                Most(g_rec.pad_gap_ms, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(t - last).count()));
            last = t;
            g_rec.pads++;
            if (g_host.seq && int32_t(seq - g_host.seq) > 1) g_rec.pads_missing += seq - g_host.seq - 1;
            if (g_host.seq && int32_t(seq - g_host.seq) <= 0) g_rec.pads_late++;
            if (seq % 30 == 0) {  // the joiner times the way there and back with this
                uint8_t echo[9] = {'S', '2', 'C', '1', 9};
                std::memcpy(echo + 5, &seq, 4);
                sendto(g_host.sock, reinterpret_cast<const char*>(echo), sizeof echo, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
            }
        }
        if (int32_t(seq - g_host.seq) > 0 || !g_host.seq) {
            g_host.seq = seq;
            g_host.pad = p;
            if (g_host.connected) RemoteInput(p.buttons, p.analog, p.lx, p.ly, p.rx, p.ry);
        }
        g_host.last_input = Clock::now();
        if (!g_host.connected) {
            g_host.connected = true;
            LOGR("coop: remote player joined from %s:%u", IpText(from.sin_addr.s_addr).c_str(), unsigned(ntohs(from.sin_port)));
            game_coop::RemoteLookForget();
            if (n >= 29) game_coop::RemoteLookFrom(buf[27], buf[28]);
            if (n >= 30) game_coop::RemoteVolumeFrom(buf[29]);
            game_coop::RemotePlayer(true);
        }
    }
}

void HostSend(const void* data, size_t n) {
    sockaddr_in to;
    {
        std::lock_guard l(g_host.lock);
        if (!g_host.have_client) return;
        to = g_host.client;
    }
    sendto(g_host.sock, static_cast<const char*>(data), int(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

struct Outgoing {
    std::mutex lock;
    std::condition_variable cv;
    std::vector<uint8_t> jpeg;
    uint32_t frame = 0, w = 0, h = 0, dropped = 0;
    float shape = 0;
    bool fresh = false;
} g_out;

// One picture to the joined player, in packets. On a thread of its own since 2026-11-26 (HostSend below): a large
// picture is a hundred packets with breaths between them, and the packing thread was held up by it: 24 pictures a
// second went out where 30 were made.
void SendPicture(const std::vector<uint8_t>& jpeg, uint32_t frame, uint32_t w, uint32_t h, float shape) {
    uint16_t parts = uint16_t((jpeg.size() + kChunk - 1) / kChunk);
    std::vector<uint8_t> pkt(kFrameHead + kChunk);

    std::vector<uint8_t> repair(kChunk, 0);
    for (uint16_t i = 0; i < parts; i++) {
        size_t at = size_t(i) * kChunk, n = std::min(kChunk, jpeg.size() - at);
        uint16_t w16 = uint16_t(w), h16 = uint16_t(h), n16 = uint16_t(n);
        std::memcpy(pkt.data(), "S2C1", 4);
        pkt[4] = 2;
        std::memcpy(pkt.data() + 5, &frame, 4);
        std::memcpy(pkt.data() + 9, &w16, 2);
        std::memcpy(pkt.data() + 11, &h16, 2);
        std::memcpy(pkt.data() + 13, &parts, 2);
        std::memcpy(pkt.data() + 15, &i, 2);
        std::memcpy(pkt.data() + 17, &n16, 2);
        std::memcpy(pkt.data() + 19, &shape, 4);  // the shape the picture is shown in (width / height)
        std::memcpy(pkt.data() + kFrameHead, jpeg.data() + at, n);
        HostSend(pkt.data(), kFrameHead + n);
        for (size_t k = 0; k < n; k++) repair[k] ^= jpeg[at + k];
        // (a picture is twenty to a hundred packets: sent in one burst they overran a home router's queue now and
        // then, and one lost packet was a lost picture. A short breath every eight.)
        if (i % 8 == 7) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    // A large picture is many parts (a hundred for 1920 x 1440), and one repair packet for all of them mends
    // one loss only: every twelve parts get a repair packet of their own (packet type 11: the picture, how many
    // parts it has, the group's first part, how many are in the group, the last part's length). A copy of the
    // game from before 2026-11-26 drops these and keeps the one below.
    if (parts > kRepairGroup) {
        uint16_t last_len = uint16_t(jpeg.size() - size_t(parts - 1) * kChunk);
        std::vector<uint8_t> grp(17 + kChunk);
        for (uint16_t first = 0; first < parts; first = uint16_t(first + kRepairGroup)) {
            uint16_t count = uint16_t(std::min<int>(kRepairGroup, parts - first));
            std::fill(grp.begin(), grp.end(), uint8_t(0));
            std::memcpy(grp.data(), "S2C1", 4);
            grp[4] = 11;
            std::memcpy(grp.data() + 5, &frame, 4);
            std::memcpy(grp.data() + 9, &parts, 2);
            std::memcpy(grp.data() + 11, &first, 2);
            std::memcpy(grp.data() + 13, &count, 2);
            std::memcpy(grp.data() + 15, &last_len, 2);
            for (uint16_t k = first; k < first + count; k++) {
                size_t at = size_t(k) * kChunk, len = std::min(kChunk, jpeg.size() - at);
                for (size_t b = 0; b < len; b++) grp[17 + b] ^= jpeg[at + b];
            }
            HostSend(grp.data(), grp.size());
            if ((first / kRepairGroup) % 8 == 7) std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    // The repair packet: part number = the number of parts (a copy of the game from before 2026-11-25 drops such
    // a packet), the width field holds the last part's length, the height field 0xFFFF. With it the joiner
    // rebuilds any one part of the picture that did not arrive.
    if (parts > 1) {
        uint16_t last = uint16_t(jpeg.size() - size_t(parts - 1) * kChunk), mark = 0xFFFF, n16 = uint16_t(kChunk);
        std::memcpy(pkt.data(), "S2C1", 4);
        pkt[4] = 2;
        std::memcpy(pkt.data() + 5, &frame, 4);
        std::memcpy(pkt.data() + 9, &last, 2);
        std::memcpy(pkt.data() + 11, &mark, 2);
        std::memcpy(pkt.data() + 13, &parts, 2);
        std::memcpy(pkt.data() + 15, &parts, 2);
        std::memcpy(pkt.data() + 17, &n16, 2);
        std::memcpy(pkt.data() + 19, &shape, 4);
        std::memcpy(pkt.data() + kFrameHead, repair.data(), kChunk);
        HostSend(pkt.data(), kFrameHead + kChunk);
    }
}

void Compose(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch, std::vector<uint8_t>& out, uint32_t& out_w, uint32_t& out_h);
void HostEncode() {
    std::vector<uint8_t> rgb, jpeg, raw;
    uint32_t frame = 0, w = 0, h = 0, rw = 0, rh = 0;
    std::thread sender([] {
        std::vector<uint8_t> mine;
        while (g_host.run) {
            uint32_t frame_ = 0, w_ = 0, h_ = 0;
            float shape_ = 0;
            {
                std::unique_lock o(g_out.lock);
                g_out.cv.wait_for(o, std::chrono::milliseconds(100), [] { return g_out.fresh || !g_host.run; });
                if (!g_out.fresh) continue;
                g_out.fresh = false;
                mine.swap(g_out.jpeg);
                frame_ = g_out.frame, w_ = g_out.w, h_ = g_out.h, shape_ = g_out.shape;
            }
            SendPicture(mine, frame_, w_, h_, shape_);
        }
    });
    while (g_host.run) {
        {
            std::unique_lock l(g_host.frame_lock);
            g_host.frame_cv.wait_for(l, std::chrono::milliseconds(100), [] { return g_host.frame_new || !g_host.run; });
            if (!g_host.frame_new) continue;
            g_host.frame_new = false;
            raw.swap(g_host.raw);
            rw = g_host.raw_w, rh = g_host.raw_h;
        }
        if (!ClientConnected() || raw.size() < size_t(rw) * rh * 4 || !rw || !rh) continue;
        Compose(raw.data(), rw, rh, rw * 4, rgb, w, h);
        jpeg.clear();
        auto put = [](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<uint8_t>*>(ctx);
            v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        };
        int64_t encode_from = MicrosNow();
        if (!stbi_write_jpg_to_func(put, &jpeg, int(w), int(h), 3, rgb.data(), kJpegQuality)) continue;
        {   // the record
            uint32_t us = uint32_t(MicrosNow() - encode_from);
            g_rec.encode_us += us;
            Most(g_rec.encode_worst_us, us);
            g_rec.pictures++;
            g_rec.picture_bytes += uint32_t(jpeg.size());
            static Clock::time_point since = Clock::now();
            Clock::time_point t = Clock::now();
            if (t - since >= std::chrono::seconds(5)) {
                double span = std::chrono::duration<double>(t - since).count();
                uint32_t pictures = g_rec.pictures.exchange(0), pads = g_rec.pads.exchange(0);
                LOGI("net: host, last %.0f s: %u pictures sent (%.0f kB a second, %.1f ms to pack one, %.1f at worst), %u sound packets; "
                     "%u controller packets came (%u missing, %u out of order, longest gap %u ms)",
                     span, pictures, g_rec.picture_bytes.exchange(0) / 1024.0 / span, pictures ? g_rec.encode_us.exchange(0) / 1000.0 / pictures : 0.0,
                     g_rec.encode_worst_us.exchange(0) / 1000.0, g_rec.sounds.exchange(0), pads, g_rec.pads_missing.exchange(0),
                     g_rec.pads_late.exchange(0), g_rec.pad_gap_ms.exchange(0));
                since = t;
            }
        }
        frame++;
        if (frame == 1 || frame % 300 == 0) LOGI("coop: picture %u to the remote player: %u x %u, %zu bytes", frame, w, h, jpeg.size());
        {   // the detail record: the picture the joined player is sent, kept every three seconds exactly as it goes
            // out (what HE saw: his view is not on this PC's screen), and a line for each one
            // ... and at every mark (F9, or a check that found something odd), beside the full picture of the window
            static Clock::time_point kept{};
            static uint32_t marks_seen = game_record::Marks(nullptr);
            if (game_detail::On()) {
                game_detail::Line("sent %u %ux%u %zu bytes", frame, w, h, jpeg.size());
                uint32_t marks = game_record::Marks(nullptr);
                bool marked = marks != marks_seen;
                marks_seen = marks;
                if (marked || Clock::now() - kept >= std::chrono::seconds(3)) {
                    if (!marked) kept = Clock::now();
                    char name[64];
                    std::snprintf(name, sizeof name, marked ? "/mark_%07.1f_p2.jpg" : "/%07.1f_p2.jpg", logx::Seconds());
                    if (FILE* f = std::fopen((game_detail::PicturesDir() + name).c_str(), "wb")) {
                        std::fwrite(jpeg.data(), 1, jpeg.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
        {   // to the sending thread: the newest picture waits there, an older one that has not gone yet is dropped
            std::lock_guard o(g_out.lock);
            if (g_out.fresh) g_out.dropped++;
            g_out.jpeg = jpeg, g_out.frame = frame, g_out.w = w, g_out.h = h, g_out.shape = g_host.frame_aspect.load(), g_out.fresh = true;
        }
        g_out.cv.notify_one();
    }
    g_out.cv.notify_all();
    if (sender.joinable()) sender.join();
}

void HostBeacon() {
    std::string name = HostName();
    uint8_t pkt[39] = {};
    std::memcpy(pkt, "S2C1", 4);
    pkt[4] = 3;
    uint16_t port = kGamePort;
    std::memcpy(pkt + 5, &port, 2);
    std::memcpy(pkt + 7, name.c_str(), std::min<size_t>(name.size(), 31));
    for (int tick = 0; g_host.run; tick++) {
        // (see Stun, above. Every five seconds while nobody is in: on 2026-11-26 the host's outside port changed as
        // the friend's packets began to arrive and it took up to twenty seconds to notice; every twenty with a player)
        if (tick % (ClientConnected() ? 20 : 5) == 0) StunAsk(g_host.sock);
        for (uint32_t b : Broadcasts()) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kBeaconPort);
            to.sin_addr.s_addr = b;
            sendto(g_host.sock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        }
        for (int i = 0; i < 10 && g_host.run; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // hole punching (see Stun): while a visitor is expected, a small packet goes to his outside address
            // five times a second, so that this side's router lets his packets in
            sockaddr_in peer{};
            bool punch;
            {
                std::lock_guard l(g_host.lock);
                // five times a second for the first 90 s, then once a second for as long as he hosts and nobody
                // is in: a friend who left can come back with the same code
                bool known = g_host.punch.sin_port != 0 && !g_host.connected;
                bool keen = g_host.punch_until > Clock::now();
                punch = known && (keen ? i % 2 == 0 : i == 0);
                peer = g_host.punch;
            }
            if (punch) {
                const uint8_t hello[5] = {'S', '2', 'C', '1', 6};
                sendto(g_host.sock, reinterpret_cast<const char*>(hello), sizeof hello, 0, reinterpret_cast<sockaddr*>(&peer), sizeof peer);
                static Clock::time_point said{};
                if (Clock::now() - said > std::chrono::seconds(10)) {
                    said = Clock::now();
                    unsigned mine;
                    {
                        std::lock_guard s(g_stun.lock);
                        mine = g_stun.port[0] ? g_stun.port[0] : g_stun.port[1];
                    }
                    LOGI("net: host, waiting for the expected visitor: sending to his outside port %u, nothing has come from him yet; this game's outside port is %u",
                         unsigned(ntohs(peer.sin_port)), mine);
                }
            }
        }
    }
}

// the renderer's frame (render thread): the half or whole screen as RGB for the encoder, at most every kFrameInterval
void Sink(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch) {
    static std::atomic<uint32_t> calls{0};
    if (++calls == 1) LOGI("coop: the renderer hands over frames (%u x %u)", w, h);
    auto now = Clock::now();
    // (Player 2's own view is drawn only when a picture is due, every 30 ms or more, see coop.cpp 0x1B83E0, and reaches
    // this place a frame or two later, not evenly: each one is taken. The limit is for the frames of a scene, where
    // the host's own picture comes every frame.)
    int least = game_coop::StreamView() ? kFrameInterval / 2 : kFrameInterval;
    if (now - g_host.last_sent < std::chrono::milliseconds(least) || !ClientConnected()) return;
    g_host.last_sent = now;
    // (Only the copy is made here, on the renderer's thread: the picture for the joined player is put together on
    // the packing thread, see Compose. At 640 x 480 the difference was nothing; a picture of 1920 x 1440 is nine
    // times the pixels.)
    std::lock_guard l(g_host.frame_lock);
    g_host.raw.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(g_host.raw.data() + size_t(y) * w * 4, bgra + size_t(y) * pitch, size_t(w) * 4);
    g_host.raw_w = w, g_host.raw_h = h;
    g_host.frame_new = true;
    g_host.frame_cv.notify_one();
}

// the renderer's pixels as the picture the joined player gets: RGB, with what the host's runtime draws over its own
// screen and is not in these pixels (his health and ammunition, his partner's health, the arrow, a crosshair)
void Compose(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch, std::vector<uint8_t>& out, uint32_t& out_w, uint32_t& out_h) {
    bool half = game_coop::SplitActive();
    // the right half (and not the divider's own columns), halved again in size when it is large: the picture is
    // JPEG in UDP packets, 30 a second
    uint32_t x0 = half ? w / 2 + std::max<uint32_t>(2, w / 320) : 0, ww = w - x0;
    // (it was halved when larger than 1100 either way, from the days the whole render target came here; now the
    // renderer hands over the size asked for, see PictureRows)
    uint32_t step = 1, ow = ww / step, oh = h / step;
    out.resize(size_t(ow) * oh * 3);
    for (uint32_t y = 0; y < oh; y++) {
        const uint8_t* s = bgra + size_t(y * step) * pitch + size_t(x0) * 4;
        uint8_t* d = out.data() + size_t(y) * ow * 3;
        for (uint32_t x = 0; x < ow; x++) {
            const uint8_t* p = s + size_t(x * step) * 4;
            d[3 * x] = p[2];
            d[3 * x + 1] = p[1];
            d[3 * x + 2] = p[0];
        }
    }
    // Player 2's health bar: on the host the runtime draws it over the finished frame (gpu::g_second_health), so
    // it is not in these pixels. The same bar here, at the same place in his view (the title's bar: layout 37 to
    // 150 across, 40 to 53 down in a 640 x 480 frame put into the half).
    float health = gpu::g_second_health.load();
    auto rect = [&](int l, int t, int r, int b, uint8_t red, uint8_t green, uint8_t blue) {
        for (int y = std::max(t, 0); y < std::min(b, int(oh)); y++)
            for (int x = std::max(l, 0); x < std::min(r, int(ow)); x++) {
                uint8_t* d = out.data() + (size_t(y) * ow + size_t(x)) * 3;
                d[0] = red, d[1] = green, d[2] = blue;
            }
    };
    // (Player 2's own bar is blue; under it, thin, his partner's in the title's red: see gpu::g_first_health)
    // In the title's own bar art when the runtime has taken it from the HUD texture (gpu::g_hud_art: the empty bar's
    // strip rows 32 to 47, the filled strip rows 3 to 15, columns 0 to 97; Player 2's 48 rows further down), each
    // pixel sampled between the art's texels and blended in by its alpha. Plain rectangles until then.
    std::vector<uint8_t> art;
    if (gpu::g_hud_art.ready.load()) {
        std::lock_guard a(gpu::g_hud_art.lock);
        art = gpu::g_hud_art.bgra;
    }
    auto strip = [&](float l, float t, float r, float b, float u1, float v0, float v1) {  // texels 0..u1 across, v0..v1 down
        if (r <= l || b <= t) return;
        for (int y = std::max(int(t), 0); y < std::min(int(std::ceil(b)), int(oh)); y++)
            for (int x = std::max(int(l), 0); x < std::min(int(std::ceil(r)), int(ow)); x++) {
                float u = (float(x) + 0.5f - l) / (r - l) * u1 - 0.5f, v = v0 + (float(y) + 0.5f - t) / (b - t) * (v1 - v0) - 0.5f;
                if (u < -0.5f || u > u1 - 0.5f || v < v0 - 0.5f || v > v1 - 0.5f) continue;
                int ui = int(std::floor(u)), vi = int(std::floor(v));
                float fu = u - float(ui), fv = v - float(vi), c[4] = {};
                for (int k = 0; k < 4; k++) {
                    int tx = std::clamp(ui + (k & 1), 0, int(gpu::kHudArtW) - 1), ty = std::clamp(vi + (k >> 1), int(v0), int(std::ceil(v1)) - 1);
                    float wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                    const uint8_t* texel = art.data() + (size_t(ty) * gpu::kHudArtW + size_t(tx)) * 4;
                    for (int i = 0; i < 4; i++) c[i] += float(texel[i]) * wgt;
                }
                float alpha = c[3] / 255.0f;
                uint8_t* d = out.data() + (size_t(y) * ow + size_t(x)) * 3;
                d[0] = uint8_t(float(d[0]) * (1 - alpha) + c[2] * alpha);  // (the art is B G R A, the frame R G B)
                d[1] = uint8_t(float(d[1]) * (1 - alpha) + c[1] * alpha);
                d[2] = uint8_t(float(d[2]) * (1 - alpha) + c[0] * alpha);
            }
    };
    auto bar_of = [&](int left, int top, int right, int bottom, float value, bool blue) {
        if (value < 0) return;
        if (!art.empty()) {
            float part = std::clamp(value, 0.0f, 1.0f), high = float(bottom - top), v = blue ? float(gpu::kHudArtH) : 0.0f;
            strip(float(left), float(top) - high * 1.5f / 13.0f, float(right), float(bottom) + high * 1.5f / 13.0f, 98.0f, v + 32.0f, v + 48.0f);
            strip(float(left), float(top), float(left) + float(right - left) * part, float(bottom), 98.0f * part, v + 3.0f, v + 16.0f);
            return;
        }
        int filled = left + int(float(right - left) * std::clamp(value, 0.0f, 1.0f));
        rect(left - 1, top - 1, right + 1, bottom + 1, 5, 3, 3);
        if (blue) rect(left, top, right, bottom, 15, 36, 8), rect(left, top, filled, bottom, 140, 219, 41);  // (Player 2: lime green)
        else rect(left, top, right, bottom, 41, 8, 8), rect(left, top, filled, bottom, 204, 56, 61);
    };
    auto bar = [&](int left, int top, int right, int bottom) { bar_of(left, top, right, bottom, health, true); };
    float partner = gpu::g_first_health.load();
    if (half && health >= 0) {
        float fx = float(w) / 640.0f / float(step), fy = float(h) / 480.0f / float(step) * gpu::g_flat2d.half_sy;
        bar(int((320.0f + 37.0f * 0.5f) * fx) - int(x0 / step), int(40.0f * fy), int((320.0f + 150.0f * 0.5f) * fx) - int(x0 / step),
            int(53.0f * fy));
    }
    // Player 2's view, whole (taken inside the frame, before Player 1's view and the 2D layer are drawn): his
    // health bar where Player 1 has his own (the 2D layer is kept in its 4:3 shape in the middle of a wide
    // picture: g_flat2d), and a crosshair in the middle, light with a dark edge, the size of the game's own.
    if (game_coop::StreamView()) {
        float kx = float(ow) / 640.0f, ky = float(oh) / 480.0f;
        float sx = gpu::g_flat2d.on ? gpu::g_flat2d.sx : 1.0f, sy = gpu::g_flat2d.on ? gpu::g_flat2d.sy : 1.0f;
        auto px = [&](float x) { return int((320.0f + (x - 320.0f) * sx) * kx); };
        auto py = [&](float y) { return int((240.0f + (y - 240.0f) * sy) * ky); };
        if (health >= 0) bar(px(37), py(40), px(150), py(53));
        bar_of(px(37), py(86), px(37.0f + 113.0f * 0.65f), py(86.0f + 13.0f * 0.65f), partner, false);  // (the same art, smaller)
        {   // the arrow above his partner's head (gpu::g_partner_arrow[1]: where, as a share of his view), as the
            // runtime draws it on the host's own screen
            float ax = gpu::g_partner_arrow[1].x.load(), ay = gpu::g_partner_arrow[1].y.load();
            if (ax >= 0) {
                // a triangle pointing down, each pixel coloured by how much of it the triangle covers (16 samples),
                // first a little larger in a dark colour, then the orange
                float tx = std::clamp(ax, 0.03f, 0.97f) * float(ow), tip = std::clamp(ay, 0.06f, 0.97f) * float(oh);
                float th = float(oh) / 48.0f, tw = th * 0.95f;
                for (int pass = 0; pass < 2; pass++) {
                    float grow = pass ? 0.0f : std::max(1.0f, float(oh) / 540.0f);
                    float hw = tw + grow * 1.6f, y0 = tip - th - grow, y1 = tip + grow * 1.8f;
                    const uint8_t colour[3] = {uint8_t(pass ? 237 : 12), uint8_t(pass ? 133 : 8), uint8_t(pass ? 26 : 3)};
                    for (int y = std::max(0, int(y0) - 1); y <= std::min(int(oh) - 1, int(y1) + 1); y++)
                        for (int x = std::max(0, int(tx - hw) - 1); x <= std::min(int(ow) - 1, int(tx + hw) + 1); x++) {
                            int inside = 0, inner = 0;  // (inner: of the orange triangle, which the dark edge stays out of)
                            for (int k = 0; k < 16; k++) {
                                float sx = float(x) + (float(k % 4) + 0.5f) / 4, sy = float(y) + (float(k / 4) + 0.5f) / 4;
                                if (sy < y0 || sy > y1) continue;
                                float half = hw * (y1 - sy) / (y1 - y0);  // the triangle's half width at that height
                                inside += std::fabs(sx - tx) <= half;
                                if (!pass && sy >= tip - th && sy <= tip) inner += std::fabs(sx - tx) <= tw * (tip - sy) / th;
                            }
                            inside -= std::min(inside, inner);
                            if (!inside) continue;
                            uint8_t* d = out.data() + (size_t(y) * ow + size_t(x)) * 3;
                            for (int c = 0; c < 3; c++) d[c] = uint8_t((d[c] * (80 - inside * 3) + colour[c] * inside * 3) / 80);  // (see-through at 60%)
                        }
                }
            }
        }
        {   // His ammunition, where the game's own HUD has Player 1's (asked for 2026-11-26: "he also doesnt have an
            // ammo hud"): a mark for each round in the weapon, the clip's other places dark, and in a box the number
            // the game shows there, what is in the weapon plus what he carries for it. (The game's HUD keeps one
            // set of numbers, Player 1's: these are read from Player 2 himself, coop.cpp.)
            int rounds = game_coop::g_ammo2_rounds.load(), clip = std::max(game_coop::g_ammo2_clip.load(), 0), more = game_coop::g_ammo2_more.load();
            if (rounds >= 0 && clip > 0) {
                int total = rounds + std::max(more, 0);
                // (a big clip: at most twenty marks, each for several rounds)
                int each = (clip + 19) / 20;
                clip = (clip + each - 1) / each, rounds = (rounds + each - 1) / each;
                static const uint8_t kDigits[10][7] = {{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
                                                       {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
                                                       {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
                                                       {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
                int top = py(40), bottom = py(53), high = std::max(7, bottom - top);
                int dot = std::max(1, (high - 4) / 7);                       // a digit is 5 x 7 dots
                char text[12];
                std::snprintf(text, sizeof text, "%d", std::clamp(total, 0, 9999));
                int digits = int(std::strlen(text)), box_w = digits * 6 * dot + 3 * dot, right = px(603), left = right - box_w;
                rect(left - 1, top - 1, right + 1, bottom + 1, 150, 150, 140);  // the box: a pale edge, dark inside
                rect(left, top, right, bottom, 12, 12, 12);
                int ty = top + (high - 7 * dot) / 2, tx = left + 2 * dot;
                for (int i = 0; i < digits; i++, tx += 6 * dot)
                    for (int r = 0; r < 7; r++)
                        for (int c = 0; c < 5; c++)
                            if (kDigits[text[i] - '0'][r] & (0x10 >> c)) rect(tx + c * dot, ty + r * dot, tx + (c + 1) * dot, ty + (r + 1) * dot, 232, 228, 210);
                // the marks, right to left from the box: one for each place in the clip
                int mark_w = std::max(2, int(2.0f * kx * sx + 0.5f)), step = mark_w + std::max(1, mark_w / 2), x = left - 2 * step;
                for (int i = 0; i < clip; i++, x -= step) {
                    bool full = i < rounds;
                    rect(x - 1, top + 1, x + mark_w + 1, bottom - 1, 10, 10, 10);
                    rect(x, top + 2, x + mark_w, bottom - 2, full ? 222 : 52, full ? 214 : 50, full ? 180 : 46);
                }
            }
        }
        // (the crosshair: the game's own is in the picture when its 2D layer is drawn for him, coop.cpp StreamLayer)
        int cx = int(ow) / 2, cy = int(oh) / 2;
        int arm = std::max(3, int(5.0f * ky)), gap = std::max(1, int(2.0f * ky)), thick = std::max(1, int(1.0f * ky + 0.5f));
        for (int pass = game_coop::StreamHasLayer() ? 2 : 0; pass < 2; pass++) {  // the dark edge, then the light cross
            int e = pass ? 0 : 1;
            uint8_t c = pass ? 225 : 20;
            rect(cx - gap - arm - e, cy - e, cx - gap + e, cy + thick + e, c, c, c);
            rect(cx + gap + thick - e, cy - e, cx + gap + thick + arm + e, cy + thick + e, c, c, c);
            rect(cx - e, cy - gap - arm - e, cx + thick + e, cy - gap + e, c, c, c);
            rect(cx - e, cy + gap + thick - e, cx + thick + e, cy + gap + thick + arm + e, c, c, c);
        }
    }
    g_host.frame_aspect = half ? app::g_frame_aspect.load() * float(ww) / float(w) : app::g_frame_aspect.load();
    out_w = ow;
    out_h = oh;
}

// ---- discovery --------------------------------------------------------------------------------------------------
struct Found {
    std::string name;
    uint32_t ip = 0;  // network order
    uint16_t port = kGamePort;
    Clock::time_point seen;
    bool manual = false;
};
std::mutex g_found_lock;
std::vector<Found> g_found;
std::atomic<bool> g_discover{false};
std::thread g_discover_thread;

void ManualHosts() {
    if (!app::Get().join_ip.empty()) {  // settings.toml join_ip: a host past the broadcast networks
        in_addr a{};
        std::string ip = app::Get().join_ip;
        std::lock_guard l(g_found_lock);
        if (inet_pton(AF_INET, ip.c_str(), &a) == 1) g_found.push_back({ip, a.s_addr, kGamePort, Clock::now(), true});
    }
    std::string path = (app::ExeDir() / "coop_hosts.txt").string();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        char ip[64] = {};
        int port = kGamePort;
        if (line[0] == '#' || std::sscanf(line, "%63s %d", ip, &port) < 1) continue;
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) != 1) continue;
        std::lock_guard l(g_found_lock);
        bool have = false;
        for (Found& h : g_found) have |= h.ip == a.s_addr && h.port == port;
        if (!have) g_found.push_back({ip, a.s_addr, uint16_t(port), Clock::now(), true});
    }
    std::fclose(f);
}

void Discover() {
    Sock s = Bind(kBeaconPort, true);
    if (s == kBad) LOGE("coop: cannot listen for games on UDP port %u", unsigned(kBeaconPort));
    ManualHosts();
    std::string own = HostName();
    uint8_t buf[64];
    while (g_discover) {
        if (s == kBad) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n = int(recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
        std::lock_guard l(g_found_lock);
        auto now = Clock::now();
        g_found.erase(std::remove_if(g_found.begin(), g_found.end(),
                                     [&](const Found& h) {
                                         return !h.manual && now - h.seen > std::chrono::milliseconds(kBeaconTimeout);
                                     }),
                      g_found.end());
        if (n < 39 || std::memcmp(buf, "S2C1", 4) != 0 || buf[4] != 3) continue;
        if (g_host.run) continue;  // a hosting game does not list games to join, its own included
        uint16_t port;
        std::memcpy(&port, buf + 5, 2);
        char name[33] = {};
        std::memcpy(name, buf + 7, 32);
        bool have = false;
        for (Found& h : g_found)
            if (h.ip == from.sin_addr.s_addr && h.port == port) {
                h.seen = now;
                h.name = name;
                have = true;
            }
        // the same game over two networks (LAN and loopback, LAN and Hamachi): listed once, by the first address
        for (Found& h : g_found) have |= !h.manual && h.name == name && h.port == port;
        if (!have) {
            g_found.push_back({name, from.sin_addr.s_addr, port, now, false});
            LOGR("coop: found %s at %s", name, IpText(from.sin_addr.s_addr).c_str());
        }
    }
    if (s != kBad) Close(s);
}

// ---- client -----------------------------------------------------------------------------------------------------
struct ClientState {
    Sock sock = kBad;
    std::atomic<bool> run{false};
    std::thread recv;
    sockaddr_in host{};
    std::string name;
    uint32_t seq = 0;
    Clock::time_point joined, last_frame, back_since;
    Clock::time_point first_frame;  // when the host's first picture came: it is shown a moment later (kJoiningMs)
    bool back = false;
    // frames: assembling, and the newest decoded one (BGRA, at its own size, with bars where this window's shape
    // differs from the picture's), handed to the renderer
    std::mutex lock;
    std::vector<uint8_t> shown;
    int shown_w = 0, shown_h = 0;
    uint32_t shown_serial = 0;      // goes up with every new picture
    bool have_frame = false;
    std::vector<uint8_t> present;  // the renderer's copy
    int present_w = 0, present_h = 0;
    std::atomic<uint32_t> lost_percent{0};  // of the pictures of the last two seconds (told to the host)
} g_client;

// a decoded frame into the 640 x 480 BGRA frame this game shows: the picture keeps the shape it has on the host
// (shape, width / height), centred, with bars where this game's own picture (app::g_frame_aspect) is wider or taller
// (Since 2026-11-26 the frame is as large as the picture: W x H is the picture's own size, made wider or taller only
// by the bars. It was 640 x 480 whatever came.)
void Letterbox(const uint8_t* rgba, int w, int h, float shape, std::vector<uint8_t>& out, int& W, int& H) {
    float mine = app::g_frame_aspect.load();
    if (mine <= 0) mine = 4.0f / 3.0f;
    if (shape <= 0) shape = float(w) / float(h);
    // this game's frame is drawn 4:3 and shown in the window's shape, as the host's is: a picture of the same shape
    // fills it pixel for pixel
    W = w, H = h;
    int dw = W, dh = H;  // the part of the frame the picture fills
    if (shape < mine * 0.995f) W = std::max(w, int(float(w) * mine / shape + 0.5f)) & ~1;
    else if (shape > mine * 1.005f) H = std::max(h, int(float(h) * shape / mine + 0.5f)) & ~1;
    out.assign(size_t(W) * H * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    int ox = (W - dw) / 2, oy = (H - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy2 = std::min(h - 1, y * h / dh);
        uint8_t* d = &out[(size_t(oy + y) * W + ox) * 4];
        for (int x = 0; x < dw; x++) {
            const uint8_t* p = rgba + (size_t(sy2) * w + std::min(w - 1, x * w / dw)) * 4;
            d[4 * x] = p[2];
            d[4 * x + 1] = p[1];
            d[4 * x + 2] = p[0];
            d[4 * x + 3] = 255;
        }
    }
}

void SoundIn(const uint8_t* b, int n);

// The host's game port, as seen from outside, can change while a friend is on his way in. In the session of
// 2026-11-26 (suffering2_012.log and the friend's suffering2_002.log) the host's router gave the game a new outside
// port (27025 became 22611) within ten seconds of the friend's first packets arriving: the meeting point was told the
// new port at once, but the friend's game went on sending to the old one for its whole 90 s and gave up, and only
// typing the code again got him in. The joiner now follows the host: the meeting point's answer is read every four
// seconds (meet.cpp) and a packet of the game's own that comes from the host's address on another port is believed.
// Only until the first picture: after that the two games are talking and the port stays.
void Retarget(const sockaddr_in& to, const char* how) {
    std::lock_guard l(g_client.lock);
    if (!g_client.run || g_client.have_frame) return;
    if (to.sin_addr.s_addr == g_client.host.sin_addr.s_addr && to.sin_port == g_client.host.sin_port) return;
    bool same_pc = to.sin_addr.s_addr == g_client.host.sin_addr.s_addr;
    LOGR("coop: internet: the host's game is now at port %u%s (it was port %u): this game sends there (%s)", unsigned(ntohs(to.sin_port)),
         same_pc ? "" : " of another address", unsigned(ntohs(g_client.host.sin_port)), how);
    g_client.host.sin_addr = to.sin_addr;
    g_client.host.sin_port = to.sin_port;
    g_client.last_frame = Clock::now();  // (the wait for an answer starts again)
}

void ClientRecv() {
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> parts;
    std::map<uint32_t, uint16_t> have, last_len, size_w, size_h;
    std::map<uint32_t, std::vector<uint8_t>> repairs;  // a picture's repair packet
    struct Group {
        uint16_t first = 0, count = 0, last_len = 0;
        std::vector<uint8_t> x;
    };
    std::map<uint32_t, std::vector<Group>> groups;     // a large picture's repair packets, one for every twelve parts
    std::map<uint32_t, float> shapes;                  // each picture's shape, from its parts (a group packet has none)
    std::vector<std::pair<Clock::time_point, uint32_t>> window;  // pictures shown, and lost before each, in the last 2 s
    Clock::time_point stats_at = Clock::now(), last_shown = Clock::now();
    uint32_t stat_shown = 0, stat_lost = 0, stat_repaired = 0;
    int stat_gap = 0;
    uint32_t shown = 0;
    std::vector<uint8_t> buf(2048), frame;
    while (g_client.run) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n = int(recvfrom(g_client.sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &len));
        {   // this side's outside address is asked again every five seconds until the first picture (it goes to the
            // meeting point for the host, and a router can change it when the host's first packets arrive)
            static Clock::time_point asked{};
            bool waiting;
            {
                std::lock_guard l(g_client.lock);
                waiting = !g_client.have_frame;
            }
            if (waiting && Clock::now() - asked > std::chrono::seconds(5)) asked = Clock::now(), StunAsk(g_client.sock);
        }
        if (StunAnswer(buf.data(), n)) continue;                                        // (how the joiner looks from outside)
        bool ours = n >= 5 && std::memcmp(buf.data(), "S2C1", 4) == 0;
        // (until 2026-11-26 this line counted the STUN servers' answers too, and said "from the host" of them)
        static std::atomic<uint32_t> packets{0};
        if (ours && ++packets == 1) LOGI("coop: first packet from the host (%d bytes, type %d, from port %u)", n, buf[4], unsigned(ntohs(from.sin_port)));
        // the host's game answers from another port than the one this game sends to: that is where it is now
        if (ours && from.sin_addr.s_addr == g_client.host.sin_addr.s_addr && from.sin_port != g_client.host.sin_port) Retarget(from, "its packets come from there");
        if (ours && buf[4] == 6) {                                                       // (the host's hole-punching hello)
            static Clock::time_point said{};
            if (Clock::now() - said > std::chrono::seconds(20)) said = Clock::now(), LOGI("coop: internet: the host's game is sending to this one (its hello came from port %u)", unsigned(ntohs(from.sin_port)));
            continue;
        }
        if (n > 0) g_rec.bytes_in += uint32_t(n);
        if (n >= 9 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 9) {          // the answer to a timed controller packet
            uint32_t seq;
            std::memcpy(&seq, &buf[5], 4);
            if (seq == g_rec.asked_seq.load() && g_rec.asked_at.load()) {
                uint32_t us = uint32_t(std::min<int64_t>(MicrosNow() - g_rec.asked_at.load(), 4000000));
                g_rec.trip_us += us;
                g_rec.trips++;
                Most(g_rec.trip_worst_us, us);
            }
            continue;
        }
        if (n >= 6 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 10) {         // a scene is on in the host's game
            Clock::time_point now = Clock::now();
            if (now - g_host_scene.load() > std::chrono::milliseconds(2500)) {
                g_host_scene_since = now;
                LOGI("coop: the host's game is in a scene: the picture is Player 1's view and this controller waits");
            }
            g_host_scene = now;
            continue;
        }
        if (n >= 5 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 8) {          // the host is there, not in a level yet
            std::lock_guard l(g_client.lock);
            g_client.last_frame = Clock::now();  // (he answers: the wait for a first picture starts again)
            g_host_waits = Clock::now();
            continue;
        }
        if (n >= 7 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 7) {          // my look settings, changed in the host's menu
            uint32_t invert = buf[5] ? 1 : 0, speed = std::clamp<uint32_t>(buf[6], 40, 100);
            uint32_t volume = n >= 8 ? std::min<uint32_t>(buf[7], 100) : game_coop::g_p2_volume;
            if (invert != game_coop::g_p2_invert || speed != game_coop::g_p2_speed || volume != game_coop::g_p2_volume) {
                game_coop::g_p2_invert = invert;
                game_coop::g_p2_speed = speed;
                game_coop::g_p2_volume = volume;
                app::Save();
                LOGR("coop: my settings were changed in the host's menu and are saved here (look %s, %u%%; volume %u%%)", invert ? "swapped" : "as the game has it", speed, volume);
            }
            continue;
        }
        if (n >= 11 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 4) {
            g_rec.sounds++;
            SoundIn(buf.data(), n);
            continue;
        }
        // (tests: S2_NET_DROP=<per cent> loses that share of the picture packets on the way in)
        static const int drop = std::getenv("S2_NET_DROP") ? std::atoi(std::getenv("S2_NET_DROP")) : 0;
        bool group_packet = n >= 17 + int(kChunk) && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 11;  // a group's repair packet
        uint32_t id;
        float shape;
        uint16_t w = 0, h = 0, count = 0, part = 0, bytes = 0;
        if (group_packet) {
            if (drop && std::rand() % 100 < drop) continue;
            Group g;
            std::memcpy(&id, &buf[5], 4);
            std::memcpy(&count, &buf[9], 2);
            std::memcpy(&g.first, &buf[11], 2), std::memcpy(&g.count, &buf[13], 2), std::memcpy(&g.last_len, &buf[15], 2);
            if ((id <= shown && shown - id < 1000) || !count || !shapes.count(id)) continue;  // (a picture none of whose parts came)
            g.x.assign(&buf[17], &buf[17] + kChunk);
            auto& list = groups[id];
            if (list.size() < 64) list.push_back(std::move(g));
            shape = shapes[id];
        } else {
            if (n < int(kFrameHead) || std::memcmp(buf.data(), "S2C1", 4) != 0 || buf[4] != 2) continue;
            if (drop && std::rand() % 100 < drop) continue;
            std::memcpy(&shape, &buf[19], 4);
            std::memcpy(&id, &buf[5], 4);
            std::memcpy(&w, &buf[9], 2);
            std::memcpy(&h, &buf[11], 2);
            std::memcpy(&count, &buf[13], 2);
            std::memcpy(&part, &buf[15], 2);
            std::memcpy(&bytes, &buf[17], 2);
            if ((id <= shown && shown - id < 1000) || part > count || int(kFrameHead) + bytes > n) continue;
            shapes[id] = shape;
        }
        // how the stream is doing, into the log every five seconds (the first online session, 2026-11-25, stuttered
        // for the joined player and his log could not say why)
        {
            auto now = Clock::now();
            if (now - stats_at > std::chrono::seconds(5)) {
                if (stat_shown || stat_lost)
                    LOGI("coop: stream: %u pictures shown in 5 s, %u lost, %u repaired, longest wait between two %d ms", stat_shown, stat_lost, stat_repaired, stat_gap);
                {   // the record of the connection, the joiner's side
                    uint32_t trips = g_rec.trips.exchange(0), trip = g_rec.trip_us.exchange(0), worst = g_rec.trip_worst_us.exchange(0);
                    char there[96] = "the host does not answer timing packets (an older copy)";
                    if (trips) std::snprintf(there, sizeof there, "to the host and back %.1f ms (%.1f at worst)", trip / 1000.0 / trips, worst / 1000.0);
                    LOGI("net: joined, last 5 s: %.0f kB a second came in, %u sound packets (the sound ran dry %u times), %u controller packets sent; %s",
                         g_rec.bytes_in.exchange(0) / 1024.0 / 5.0, g_rec.sounds.exchange(0), g_rec.sound_dry.exchange(0), g_rec.pads_out.exchange(0), there);
                }
                stats_at = now, stat_shown = stat_lost = stat_repaired = 0, stat_gap = 0;
            }
        }
        auto& p = parts[id];
        if (p.empty()) p.resize(count);
        if (p.size() != count) continue;
        if (group_packet) {
            // (stored above: what follows may complete the picture with it)
        } else if (part == count) {  // the repair packet (see HostEncode)
            if (h != 0xFFFF || bytes != kChunk) continue;
            auto& r = repairs[id];
            r.assign(&buf[kFrameHead], &buf[kFrameHead] + bytes);
            last_len[id] = w;
        } else {
            if (!p[part].empty()) continue;
            p[part].assign(&buf[kFrameHead], &buf[kFrameHead] + bytes);
            ++have[id];
            if (w != 0 && h != 0xFFFF) size_w[id] = w, size_h[id] = h;
        }
        // a large picture: every group that misses one part and has its repair packet gets that part back
        if (have[id] < count && groups.count(id)) {
            for (const Group& g : groups[id]) {
                if (g.first + g.count > count || g.x.size() != kChunk) continue;
                int missing = -1, n_missing = 0;
                for (uint16_t k = g.first; k < g.first + g.count; k++)
                    if (p[k].empty()) missing = k, n_missing++;
                if (n_missing != 1) continue;
                std::vector<uint8_t> fix = g.x;
                for (uint16_t k = g.first; k < g.first + g.count; k++) {
                    if (k == missing) continue;
                    for (size_t b = 0; b < p[k].size() && b < fix.size(); b++) fix[b] ^= p[k][b];
                }
                fix.resize(missing + 1 == count ? std::min<size_t>(g.last_len, kChunk) : kChunk);
                p[size_t(missing)] = std::move(fix);
                ++have[id];
                ++stat_repaired;
            }
        }
        if (have[id] + 1 == count && repairs.count(id)) {  // one part is missing and the repair packet is here
            std::vector<uint8_t> fix = repairs[id];
            uint16_t missing = 0;
            for (uint16_t k = 0; k < count; k++) {
                if (p[k].empty()) {
                    missing = k;
                    continue;
                }
                for (size_t b = 0; b < p[k].size() && b < fix.size(); b++) fix[b] ^= p[k][b];
            }
            fix.resize(missing + 1 == count ? std::min<size_t>(last_len[id], kChunk) : kChunk);
            p[missing] = std::move(fix);
            ++have[id];
            ++stat_repaired;
        }
        if (have[id] < count) continue;
        uint32_t lost_now = id > shown + 1 && shown ? std::min<uint32_t>(id - shown - 1, 1000) : 0;
        stat_lost += lost_now;
        {   // the share of pictures lost in the last two seconds, for the host (it makes the picture smaller)
            Clock::time_point t = Clock::now();
            window.push_back({t, lost_now});
            while (!window.empty() && t - window.front().first > std::chrono::seconds(2)) window.erase(window.begin());
            uint32_t lost_sum = 0;
            for (const auto& w2 : window) lost_sum += w2.second;
            g_client.lost_percent = uint32_t(std::min<size_t>(100, lost_sum * 100 / (lost_sum + window.size())));
        }
        {
            auto now = Clock::now();
            int gap = int(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_shown).count());
            if (stat_shown && gap > stat_gap) stat_gap = gap;
            last_shown = now;
            ++stat_shown;
        }
        frame.clear();
        for (auto& c : p) frame.insert(frame.end(), c.begin(), c.end());
        if (game_detail::On()) game_detail::Line("got %u %ux%u %zu bytes in %u parts", id, unsigned(size_w.count(id) ? size_w[id] : 0), unsigned(size_h.count(id) ? size_h[id] : 0), frame.size(), unsigned(count));
        int iw, ih, comp;
        int64_t unpack_from = MicrosNow();
        uint8_t* px = stbi_load_from_memory(frame.data(), int(frame.size()), &iw, &ih, &comp, 4);
        {   // the record: how long a picture takes to unpack (a large one is ten times the old one's work)
            static uint32_t count_ = 0, worst = 0;
            static uint64_t sum = 0;
            static Clock::time_point said = Clock::now();
            uint32_t us = uint32_t(MicrosNow() - unpack_from);
            count_++, sum += us, worst = std::max(worst, us);
            if (Clock::now() - said >= std::chrono::seconds(5)) {
                LOGI("net: joined: the pictures are %d x %d, %zu kB each, %.1f ms to unpack (%.1f at worst); lost %u%% of the last two seconds' pictures", iw, ih,
                     frame.size() / 1024, sum / 1000.0 / count_, worst / 1000.0, g_client.lost_percent.load());
                said = Clock::now(), count_ = 0, sum = 0, worst = 0;
            }
        }
        for (auto it = parts.begin(); it != parts.end();) it = it->first <= id ? parts.erase(it) : std::next(it);
        for (auto it = have.begin(); it != have.end();) it = it->first <= id ? have.erase(it) : std::next(it);
        for (auto it = repairs.begin(); it != repairs.end();) it = it->first <= id ? repairs.erase(it) : std::next(it);
        for (auto it = groups.begin(); it != groups.end();) it = it->first <= id ? groups.erase(it) : std::next(it);
        for (auto it = shapes.begin(); it != shapes.end();) it = it->first <= id ? shapes.erase(it) : std::next(it);
        for (auto it = last_len.begin(); it != last_len.end();) it = it->first <= id ? last_len.erase(it) : std::next(it);
        size_w.clear(), size_h.clear();
        shown = id;
        if (!px) continue;
        std::vector<uint8_t> boxed;
        int bw = 0, bh = 0;
        Letterbox(px, iw, ih, shape, boxed, bw, bh);
        stbi_image_free(px);
        std::lock_guard l(g_client.lock);
        if (!g_client.have_frame) LOGR("coop: joined %s, first picture in", g_client.name.c_str());
        if (!g_client.have_frame) g_client.first_frame = Clock::now();
        g_client.shown.swap(boxed);
        g_client.shown_w = bw, g_client.shown_h = bh;
        g_client.shown_serial++;
        g_client.have_frame = true;
        g_client.last_frame = Clock::now();
    }
}

// Words over the host's picture on the joiner's PC, in a strip at the top: capital letters of 5 x 7 dots, drawn twice
// their size. The game's own font cannot be used here: the picture goes over the joiner's whole frame after his
// game has drawn. (640 x 480 BGRA)
void Strip(std::vector<uint8_t>& px, int W, int H, const char* line1, const char* line2) {
    static const struct {
        char c;
        uint8_t rows[7];
    } kGlyphs[] = {
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}}, {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}}, {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
        {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
        {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}, {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}, {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}}, {':', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
        {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {',', {0x00, 0x00, 0x00, 0x00, 0x06, 0x06, 0x0C}},
    };
    // (laid out for 480 rows and made larger with the picture)
    if (W < 320 || H < 240 || px.size() < size_t(W) * H * 4) return;
    const int unit = std::max(1, (H + 240) / 480), kBand = 46 * unit, kScale = 2 * unit, kStep = 6 * kScale;
    for (int y = 0; y < kBand; y++)  // a dark band, fading out at its lower edge
        for (int x = 0; x < W; x++) {
            uint8_t* p = &px[(size_t(y) * W + x) * 4];
            int keep = y < kBand - 10 * unit ? 70 : 70 + (y - (kBand - 10 * unit)) * 18 / unit;  // of 256
            p[0] = uint8_t(p[0] * keep / 256), p[1] = uint8_t(p[1] * keep / 256), p[2] = uint8_t(p[2] * keep / 256);
        }
    auto dot = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* p = &px[(size_t(y) * W + x) * 4];
        p[0] = b, p[1] = g, p[2] = r;
    };
    auto line = [&](const char* text, int top, uint8_t b, uint8_t g, uint8_t r) {
        int x0 = (W - int(std::strlen(text)) * kStep) / 2;
        for (const char* c = text; *c; c++, x0 += kStep)
            for (const auto& gl : kGlyphs) {
                if (gl.c != *c) continue;
                for (int pass = 0; pass < 2; pass++)  // a shadow first, then the letter
                    for (int ry = 0; ry < 7; ry++)
                        for (int rx = 0; rx < 5; rx++)
                            if (gl.rows[ry] & (0x10 >> rx))
                                for (int sy = 0; sy < kScale; sy++)
                                    for (int sx = 0; sx < kScale; sx++) {
                                        int x = x0 + rx * kScale + sx, y = top + ry * kScale + sy;
                                        if (pass == 0) dot(x + unit, y + unit, 0, 0, 0);
                                        else dot(x, y, b, g, r);
                                    }
                break;
            }
    };
    line(line1, 6 * unit, 0xC8, 0xE6, 0xF0);   // (the menus' pale yellow)
    line(line2, 24 * unit, 0xB0, 0xB0, 0xB0);
}

// the renderer, each present while joined: the host's picture over the joiner's own (BGRA, at the picture's size).
// The renderer keeps its texture of it while gpu::g_present_override_serial stays: the copy is made, and the number
// moved, only when a new picture came or the words over it change.
const uint8_t* Present(uint32_t& w, uint32_t& h) {
    if (!g_client.run) return nullptr;
    std::lock_guard l(g_client.lock);
    if (!g_client.have_frame) return nullptr;
    // (the joiner's own screen says "joining" for a moment first: cut straight from his menu to the middle of the
    // host's game, he could not tell what had happened; the owner's first online session, 2026-11-25)
    Clock::time_point now = Clock::now();
    bool menus = now - g_host_waits.load() < std::chrono::milliseconds(3000);
    if (now - g_client.first_frame < std::chrono::milliseconds(menus ? kJoiningMs / 3 : kJoiningMs)) return nullptr;
    // which words go over it: 1 the host is still in his menus, 2 a scene shows Player 1's view (its first seconds)
    int words = menus ? 1 : now - g_host_scene.load() < std::chrono::milliseconds(2500) && now - g_host_scene_since.load() < std::chrono::seconds(6) ? 2 : 0;
    static uint32_t made_serial = 0, serial = 0;
    static int made_words = -1;
    if (g_client.shown_serial != made_serial || words != made_words || g_client.present.empty()) {
        made_serial = g_client.shown_serial, made_words = words;
        g_client.present = g_client.shown;
        g_client.present_w = g_client.shown_w, g_client.present_h = g_client.shown_h;
        if (words == 1) Strip(g_client.present, g_client.present_w, g_client.present_h, "CONNECTED TO YOUR FRIEND'S GAME", "YOU PLAY WHEN THE GAME STARTS. HOLD BACK TO LEAVE.");
        if (words == 2) Strip(g_client.present, g_client.present_w, g_client.present_h, "A SCENE IS PLAYING. THIS IS PLAYER 1'S VIEW.", "YOU PLAY AGAIN WHEN IT ENDS.");
        gpu::g_present_override_serial = ++serial ? serial : ++serial;
    }
    int pw = g_client.present_w, ph = g_client.present_h;
    if (pw <= 0 || ph <= 0 || g_client.present.size() < size_t(pw) * ph * 4) return nullptr;
    static uint32_t calls = 0;
    if (++calls % 300 == 1) {  // (a check now and then: is there a picture in it)
        uint64_t sum = 0;
        for (size_t i = 0; i + 2 < g_client.present.size(); i += 4 * 37) sum += g_client.present[i] + g_client.present[i + 1] + g_client.present[i + 2];
        LOGI("coop: showing the host's picture (present %u, %d x %d, average level %.1f of 255)", calls, pw, ph,
             double(sum) / double(g_client.present.size() / (4 * 37) * 3 + 1));
    }
    auto dump = [&](const std::string& path) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            uint32_t bytes = uint32_t(pw) * uint32_t(ph) * 4u;
            uint32_t head[13] = {bytes + 54u, 0, 54, 40, uint32_t(pw), uint32_t(-ph), 1u | (32u << 16), 0, bytes, 2835, 2835, 0, 0};
            std::fwrite("BM", 1, 2, f);
            std::fwrite(head, 4, 13, f);
            std::fwrite(g_client.present.data(), 1, bytes, f);
            std::fclose(f);
            return true;
        }
        return false;
    };
    // S2_JOIN_DUMP=<file>: the picture handed to the renderer 20 seconds after joining, as a .bmp (tests: the
    // joiner's own frame capture is taken before this picture goes over it)
    if (calls == 1200) {
        if (const char* path = std::getenv("S2_JOIN_DUMP"))
            if (dump(path)) LOGI("coop: the picture shown was written to %s", path);
    }
    // S2_JOIN_DUMPS=<seconds,seconds,...> (with S2_JOIN_DUMP): the picture shown at those seconds after the first
    // one, as <file>_<seconds>.bmp (tests: what the joined player sees through a scene, a pause, a level change)
    {
        static const char* list = std::getenv("S2_JOIN_DUMPS");
        static const char* path = std::getenv("S2_JOIN_DUMP");
        static const auto first = Clock::now();
        static std::vector<int> times = [] {
            std::vector<int> out;
            for (const char* c = list; c && *c;) {
                out.push_back(std::atoi(c));
                c = std::strchr(c, ',');
                if (c) c++;
            }
            return out;
        }();
        static size_t next = 0;
        if (path && next < times.size() && Clock::now() - first >= std::chrono::seconds(times[next])) {
            std::string name = std::string(path);
            if (name.size() > 4) name = name.substr(0, name.size() - 4);
            name += "_" + std::to_string(times[next]) + ".bmp";
            dump(name);
            next++;
        }
    }
    w = uint32_t(pw);
    h = uint32_t(ph);
    return g_client.present.data();
}

// ---- sound ------------------------------------------------------------------------------------------------------
struct Sound {
    std::mutex lock;
    std::deque<int16_t> ring;  // the host's sound, 24 kHz stereo
    bool playing = false;
    uint32_t seq = 0;
    std::vector<int16_t> pending;  // the host's: samples not yet sent
    uint32_t sent = 0;
    float prev[2] = {};
} g_sound;

bool ClientConnected();
void HostSend(const void* data, size_t n);

// the output thread, each 10 ms block (48 kHz stereo float)
void (*g_tap_before)(float* out, uint32_t frames) = nullptr;
void SoundTap(float* out, uint32_t frames) {
    if (g_tap_before) g_tap_before(out, frames);  // (the test tooling's sound tap, when it is on)
    if (g_host.run && ClientConnected()) {
        // to 24 kHz (pairs averaged) and 16 bits, sent in kAudioFrames packets
        auto& p = g_sound.pending;
        for (uint32_t i = 0; i + 1 < frames; i += 2)
            for (int c = 0; c < 2; c++) {
                float v = (out[2 * i + c] + out[2 * i + 2 + c]) * 0.5f;
                p.push_back(int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            }
        uint8_t pkt[11 + kAudioFrames * 4];
        while (p.size() >= 2 * kAudioFrames) {
            std::memcpy(pkt, "S2C1", 4);
            pkt[4] = 4;
            uint32_t seq = ++g_sound.sent;
            uint16_t n = uint16_t(kAudioFrames);
            std::memcpy(pkt + 5, &seq, 4);
            std::memcpy(pkt + 9, &n, 2);
            std::memcpy(pkt + 11, p.data(), kAudioFrames * 4);
            g_rec.sounds++;
            HostSend(pkt, sizeof pkt);
            p.erase(p.begin(), p.begin() + 2 * kAudioFrames);
        }
    } else {
        g_sound.pending.clear();
    }
    if (!g_client.run) return;
    // joined: the host's sound instead of this game's, back to 48 kHz. The two machines' clocks drift apart, so the
    // buffer is kept between kAudioLow and kAudioHigh by reading a frame more or less per block (0.4%, unheard).
    std::lock_guard l(g_sound.lock);
    auto& r = g_sound.ring;
    if (!g_sound.playing && r.size() >= kAudioStart) g_sound.playing = true;
    if (g_sound.playing && r.size() < size_t(frames)) g_sound.playing = false, g_rec.sound_dry++;  // ran dry: silence until it fills again
    if (!g_sound.playing) {
        std::fill(out, out + frames * 2, 0.0f);
        return;
    }
    uint32_t m = frames / 2;  // source frames this block
    if (r.size() > kAudioHigh) m++;
    else if (r.size() < kAudioLow) m--;
    for (uint32_t i = 0; i < frames; i++) {
        float pos = float(i + 1) * float(m) / float(frames) - 1.0f;  // ends on the block's last source frame
        int j = int(std::floor(pos));
        float t = pos - float(j);
        for (int c = 0; c < 2; c++) {
            float a = j < 0 ? g_sound.prev[c] : r[2 * j + c] / 32768.0f;
            float b = r[2 * (j + 1) + c] / 32768.0f;
            out[2 * i + c] = a + (b - a) * t;
        }
    }
    for (int c = 0; c < 2; c++) g_sound.prev[c] = r[2 * (m - 1) + c] / 32768.0f;
    r.erase(r.begin(), r.begin() + 2 * m);
}

// the receive thread: one sound packet
void SoundIn(const uint8_t* b, int n) {
    uint32_t seq;
    uint16_t frames;
    std::memcpy(&seq, b + 5, 4);
    std::memcpy(&frames, b + 9, 2);
    if (11 + int(frames) * 4 > n) return;
    std::lock_guard l(g_sound.lock);
    if (g_sound.seq && int32_t(seq - g_sound.seq) <= 0) return;  // late or repeated
    g_sound.seq = seq;
    const auto* s = reinterpret_cast<const int16_t*>(b + 11);
    uint32_t volume = std::min<uint32_t>(game_coop::g_p2_volume, 100);  // this player's own volume for the host's sound
    if (volume == 100) {
        g_sound.ring.insert(g_sound.ring.end(), s, s + frames * 2);
    } else {
        // (per cent to loudness: the square, so that 50% sounds about half as loud)
        int32_t gain = int32_t(volume * volume * 65536 / 10000);
        for (int i = 0; i < int(frames) * 2; i++) g_sound.ring.push_back(int16_t((int32_t(s[i]) * gain) >> 16));
    }
    if (g_sound.ring.size() > kAudioMost) g_sound.ring.erase(g_sound.ring.begin(), g_sound.ring.end() - kAudioStart);
}

void StopClient() {
    if (!g_client.run) return;
    g_client.run = false;
    gpu::g_present_override = nullptr;
    if (g_client.recv.joinable()) g_client.recv.join();
    // tell the host (three times: a packet can get lost), so that Player 2 is out of its game at once
    const uint8_t bye[5] = {'S', '2', 'C', '1', 5};
    for (int i = 0; i < 3; i++)
        sendto(g_client.sock, reinterpret_cast<const char*>(bye), sizeof bye, 0, reinterpret_cast<sockaddr*>(&g_client.host), sizeof g_client.host);
    Close(g_client.sock);
    g_client.sock = kBad;
    {
        std::lock_guard s(g_sound.lock);
        g_sound.ring.clear();
        g_sound.playing = false;
        g_sound.seq = 0;
    }
    std::lock_guard l(g_client.lock);
    g_client.have_frame = false;
    LOGR("coop: left %s", g_client.name.c_str());
}

void StopHost() {
    if (!g_host.run) return;
    g_host.run = false;
    gpu::g_stream = nullptr;
    g_host.frame_cv.notify_all();
    for (std::thread* t : {&g_host.listen, &g_host.encode, &g_host.beacon})
        if (t->joinable()) t->join();
    Close(g_host.sock);
    g_host.sock = kBad;
    bool was;
    {
        std::lock_guard l(g_host.lock);
        was = g_host.connected;
        g_host.connected = g_host.have_client = false;
    }
    if (was) game_coop::RemotePlayer(false);
    g_host.punch = {};
    CloseToInternet();
    LOGR("coop: hosting off");
}
}  // namespace

// ---- the menu's side ----------------------------------------------------------------------------------------------
bool Hosting() { return g_host.run; }
bool HostHasPlayer() { return g_host.run && ClientConnected(); }
bool Joined() { return g_client.run; }

bool Host(bool on) {
    if (!on) {
        StopHost();
        return true;
    }
    if (g_host.run) return true;
    StopClient();
    g_host.sock = Bind(kGamePort, false);
    if (g_host.sock == kBad) {
        LOGE("coop: cannot host, UDP port %u is taken", unsigned(kGamePort));
        Note("UDP port " + std::to_string(kGamePort) + " is in use on this PC.");
        return false;
    }
    Note("");
    StunForget();
    g_host.run = true;
    g_host.listen = std::thread(HostListen);
    g_host.encode = std::thread(HostEncode);
    g_host.beacon = std::thread(HostBeacon);
    gpu::g_stream = Sink;
    LOGR("coop: hosting as %s on UDP port %u", HostName().c_str(), unsigned(kGamePort));
    OpenToInternet();
    return true;
}

// games to join, for the menu: names as shown, in a stable order
std::vector<std::string> Games() {
    if (!g_discover) {
        g_discover = true;
        g_discover_thread = std::thread(Discover);
        g_discover_thread.detach();
    }
    std::lock_guard l(g_found_lock);
    std::vector<std::string> out;
    for (const Found& h : g_found) out.push_back(h.name);
    return out;
}

// the host answered a moment ago that he has not started the game yet (packet type 8)
bool HostNotStarted() { return g_client.run && Clock::now() - g_host_waits.load() < std::chrono::milliseconds(3000); }

// the joiner's state for the menu: 0 not joined, 1 connecting, 2 playing in the host's game
int JoinState() {
    if (!g_client.run) return 0;
    std::lock_guard l(g_client.lock);
    return g_client.have_frame && Clock::now() - g_client.first_frame >= std::chrono::milliseconds(kJoiningMs) ? 2 : 1;
}
// ... and what he is waiting for while it is 1: 1 the host has not answered (he still has to enter this player's
// code), 2 the host answered and is not in a level yet, 3 the host is in the game and this player comes in now
int JoinPhase() {
    if (!g_client.run) return 0;
    {
        std::lock_guard l(g_client.lock);
        if (g_client.have_frame) return Clock::now() - g_client.first_frame >= std::chrono::milliseconds(kJoiningMs) ? 4 : 3;
    }
    return HostNotStarted() ? 2 : 1;
}
std::string JoinName() {
    std::lock_guard l(g_client.lock);
    return g_client.name;
}
std::string MyName() { return HostName(); }
std::string LastNote() {
    std::lock_guard l(g_note_lock);
    return g_note;
}
void ClearNote() { Note(""); }

bool Join(int index) {
    Found target;
    {
        std::lock_guard l(g_found_lock);
        if (index < 0 || index >= int(g_found.size())) return false;
        target = g_found[size_t(index)];
    }
    StopHost();
    StopClient();
    // (tests: S2_COOP_PORT=<n> gives the joiner's socket a port known beforehand, so that a host started with
    // S2_COOP_PEER can be told where he will come from)
    // The joiner's own port is the same every time (27027, or what the test names), so that his code stays the same
    // and the host, who keeps sending to it, lets him back in after a leave without a new swap of codes. Taken
    // (two copies on one PC): any free port.
    g_client.sock = Bind(std::getenv("S2_COOP_PORT") ? uint16_t(std::atoi(std::getenv("S2_COOP_PORT"))) : uint16_t(27027), false);
    if (g_client.sock == kBad) g_client.sock = Bind(0, false);
    if (g_client.sock == kBad) return false;
    g_client.host = {};
    g_client.host.sin_family = AF_INET;
    g_client.host.sin_port = htons(target.port);
    g_client.host.sin_addr.s_addr = target.ip;
    // tests: S2_TEST_WRONG_PORT=<n>: this game first sends to that port of the host's address, as if the host's
    // outside port had changed after the meeting point was asked (what happened on 2026-11-26)
    if (const char* wrong = std::getenv("S2_TEST_WRONG_PORT")) {
        g_client.host.sin_port = htons(uint16_t(std::atoi(wrong)));
        LOGR("coop: test: the first packets go to port %s, not the host's", wrong);
    }
    g_client.name = target.name;
    g_client.joined = g_client.last_frame = Clock::now();
    g_client.back = false;
    g_client.have_frame = false;
    g_client.run = true;
    Note("");
    g_client.recv = std::thread(ClientRecv);
    StunForget();
    StunAsk(g_client.sock);  // (this side's code, and logged: can it be reached directly)
    gpu::g_present_override = Present;
    LOGR("coop: joining %s at %s", target.name.c_str(), IpText(target.ip).c_str());
    return true;
}


// the host's public address and how reachable it is (empty while not hosting / not known yet)
std::string PublicAddress() {
    std::lock_guard l(g_net.lock);
    return g_net.address;
}
std::string InternetStatus() {
    std::lock_guard l(g_net.lock);
    return g_net.status;
}
// ... for the menu: this PC's address on its own network, and how far the way in from the internet got (the
// numbers are at Internet::state)
std::string LocalAddress() {
    std::lock_guard l(g_net.lock);
    return g_net.local;
}
int InternetState() {
    std::lock_guard l(g_net.lock);
    return g_net.state;
}

// join by a typed address: "73.12.4.9", "73.12.4.9:27025" or a host name. Kept as join_ip for next time (it then shows
// in the menu's JOIN list too).
bool JoinAddress(const std::string& typed) {
    std::string text = typed;
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
               text.end());
    if (text.empty()) return false;
    uint16_t port = kGamePort;
    std::string host = text;
    if (size_t colon = text.rfind(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        int p = std::atoi(text.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return Note("The port in " + text + " is not a number from 1 to 65535."), false;
        port = uint16_t(p);
    }
    // a name, or a dotted address: "31" alone would be taken as 0.0.0.31
    bool letters = std::any_of(host.begin(), host.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
    if (!letters && std::count(host.begin(), host.end(), '.') != 3) return Note("That is not an address. An address looks like 203.0.113.7."), false;
    Startup();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        Note("Cannot find " + text + ".");
        LOGR("coop: cannot resolve %s", text.c_str());
        return false;
    }
    uint32_t ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    int index = -1;
    {
        std::lock_guard l(g_found_lock);
        for (size_t i = 0; i < g_found.size(); i++)
            if (g_found[i].ip == ip && g_found[i].port == port) index = int(i);
        if (index < 0) {
            g_found.push_back({text, ip, port, Clock::now(), true});
            index = int(g_found.size()) - 1;
        }
    }
    if (app::Get().join_ip != text) {
        app::Get().join_ip = text;
        app::Save();
    }
    return Join(index);
}

// The joined player changed his look settings in the host's pause menu: they are his, so they go back to his PC
// (packet type 7: up and down swapped, speed in per cent), which keeps them in its own settings file.
void SendLook(uint32_t invert, uint32_t speed, uint32_t volume) {
    if (!g_host.run) return;
    uint8_t pkt[8] = {'S', '2', 'C', '1', 7, uint8_t(invert ? 1 : 0), uint8_t(std::clamp<uint32_t>(speed, 40, 100)), uint8_t(std::min<uint32_t>(volume, 100))};
    for (int i = 0; i < 3; i++) HostSend(pkt, sizeof pkt);  // (UDP: three times, one will arrive)
    LOGR("coop: the joined player's settings go back to his PC (look %s, %u%%; volume %u%%)", invert ? "swapped" : "as the game has it", speed, volume);
}

// The host expects a visitor from this outside address ("203.0.113.7:51234"): for half a minute his router is
// shown packets going there, so that the visitor's packets are let in. Tests: S2_COOP_PEER=<address:port>.
bool HostExpect(const std::string& address) {
    size_t colon = address.rfind(':');
    in_addr ip{};
    if (colon == std::string::npos || inet_pton(AF_INET, address.substr(0, colon).c_str(), &ip) != 1) return false;
    int port = std::atoi(address.c_str() + colon + 1);
    if (port <= 0 || port > 65535 || !g_host.run) return false;
    std::lock_guard l(g_host.lock);
    g_host.punch = {};
    g_host.punch.sin_family = AF_INET;
    g_host.punch.sin_addr = ip;
    g_host.punch.sin_port = htons(uint16_t(port));
    g_host.punch_until = Clock::now() + std::chrono::seconds(90);
    LOGR("coop: internet: expecting a visitor, sending to his outside port %d for 90 s", port);
    return true;
}

// this PC's game socket as the STUN servers see it, "203.0.113.7:51234": the code to give to the other player
// ("" until the first server has answered)
std::string OutsideAddress() {
    if (const char* fake = std::getenv("COOP_FAKE_CODE")) return fake;  // harness: pictures of the menu
    std::lock_guard l(g_stun.lock);
    int which = g_stun.port[0] ? 0 : g_stun.port[1] ? 1 : -1;
    if (which < 0) return "";
    return IpText(g_stun.ip[which]) + ":" + std::to_string(g_stun.port[which]);
}
// 0 not known yet, 1 one port for everyone (a code works), 2 a new port for every destination (it does not)
int OutsideKind() {
    std::lock_guard l(g_stun.lock);
    if (!g_stun.port[0] || !g_stun.port[1]) return 0;
    return g_stun.port[0] == g_stun.port[1] && g_stun.ip[0] == g_stun.ip[1] ? 1 : 2;
}
// the joiner: the meeting point says the host's game is at this address now ("203.0.113.7:51234")
bool JoinRetarget(const std::string& address) {
    size_t colon = address.rfind(':');
    sockaddr_in to{};
    to.sin_family = AF_INET;
    if (colon == std::string::npos || inet_pton(AF_INET, address.substr(0, colon).c_str(), &to.sin_addr) != 1) return false;
    int port = std::atoi(address.c_str() + colon + 1);
    if (port <= 0 || port > 65535 || !g_client.run) return false;
    to.sin_port = htons(uint16_t(port));
    Retarget(to, "the meeting point says so");
    return true;
}
bool HostExpecting() {
    std::lock_guard l(g_host.lock);
    return g_host.run && g_host.punch_until > Clock::now() && !g_host.connected;
}

void Leave() { StopClient(); }

// the joiner's pad goes to the host while joined, and the BACK that left a game stays away from this one's menu
// until it is let go
std::atomic<bool> g_swallow{false};
bool PadTaken(const input::Pad& p) {
    if (g_client.run) return true;
    if (g_swallow && !(p.buttons & 32)) g_swallow = false;
    return g_swallow;
}

// the joiner's pad (pad 0, keyboard included), each poll: to the host; BACK held kLeaveHold leaves; a host whose
// frames stopped is left too
void ClientPad(const input::Pad& p) {
    if (!g_client.run) return;
    auto now = Clock::now();
    bool back = p.buttons & 32;
    if (back && !g_client.back) g_client.back_since = now;
    g_client.back = back;
    if (back && now - g_client.back_since > std::chrono::milliseconds(kLeaveHold)) {
        g_swallow = true;
        Note("You left the game.");
        StopClient();
        return;
    }
    Clock::time_point last;
    bool have;
    {
        std::lock_guard l(g_client.lock);
        last = g_client.last_frame;
        have = g_client.have_frame;
    }
    if (!have && now - last > std::chrono::milliseconds(kConnectTimeout)) {
        LOGR("coop: no answer from %s, giving up", g_client.name.c_str());
        Note("No answer from " + g_client.name.substr(0, 40) + ".");
        StopClient();
        return;
    }
    // (only once a picture has come: before the first one the wait is kConnectTimeout. Until 2026-11-25 this fired
    // after 15 s without any picture, so a joiner gave up before the host could enter his code: run hairpin_join)
    if (have && now - last > std::chrono::milliseconds(kHostTimeout)) {
        LOGR("coop: no picture from %s for %d s, leaving", g_client.name.c_str(), kHostTimeout / 1000);
        Note("The connection to the host was lost.");
        StopClient();
        return;
    }
    // (byte 29: this player's volume for the host's sound, for the host's menu to show. Since 2026-11-26, bytes 30
    // to 35: this window's picture area, the share of pictures lost in the last two seconds, and 1 for "this copy
    // takes large pictures": the host makes Player 2's picture as large as this window, see PictureRows)
    uint8_t pkt[36];
    std::memcpy(pkt, "S2C1", 4);
    pkt[4] = 1;
    uint32_t seq = ++g_client.seq;
    uint8_t analog[8];
    std::memcpy(analog, p.analog, 8);
#ifdef _WIN32
    // the keyboard's teleport: T held is A+B+X+Y held (coop.cpp counts the five seconds)
    if (GetForegroundWindow() == static_cast<HWND>(app::Window()) && (GetAsyncKeyState('T') & 0x8000)) analog[0] = analog[1] = analog[2] = analog[3] = 255;
#endif
    g_rec.pads_out++;
    if (seq % 30 == 0) g_rec.asked_seq = seq, g_rec.asked_at = MicrosNow();
    std::memcpy(pkt + 5, &seq, 4);
    std::memcpy(pkt + 9, &p.buttons, 2);
    std::memcpy(pkt + 11, analog, 8);
    // this player's own look settings go with his controller (bytes 27 and 28: up and down swapped, speed in per
    // cent) and the host applies them: he is Player 2 in the host's game, and can change them in its pause menu
    std::memcpy(pkt + 19, &p.lx, 2);
    std::memcpy(pkt + 21, &p.ly, 2);
    std::memcpy(pkt + 23, &p.rx, 2);
    std::memcpy(pkt + 25, &p.ry, 2);
    pkt[27] = uint8_t(game_coop::g_p2_invert ? 1 : 0);
    pkt[28] = uint8_t(std::clamp<uint32_t>(game_coop::g_p2_speed, 40, 100));
    pkt[29] = uint8_t(std::min<uint32_t>(game_coop::g_p2_volume, 100));
    {
        static uint16_t cw = 0, ch = 0;
        static Clock::time_point read{};
#ifdef _WIN32
        if (now - read > std::chrono::milliseconds(500)) {
            read = now;
            RECT rc{};
            if (GetClientRect(static_cast<HWND>(app::Window()), &rc) && rc.right > 0 && rc.bottom > 0) cw = uint16_t(std::min<LONG>(rc.right, 65535)), ch = uint16_t(std::min<LONG>(rc.bottom, 65535));
        }
#endif
        // (tests: S2_TEST_WINDOW_ROWS=<n> says the window has that many rows, whatever it has)
        static const int rows = std::getenv("S2_TEST_WINDOW_ROWS") ? std::atoi(std::getenv("S2_TEST_WINDOW_ROWS")) : 0;
        uint16_t sw = cw, sh = rows > 0 ? uint16_t(rows) : ch;
        std::memcpy(pkt + 30, &sw, 2);
        std::memcpy(pkt + 32, &sh, 2);
        pkt[34] = uint8_t(std::min<uint32_t>(g_client.lost_percent.load(), 100));
        pkt[35] = 1;
    }
    sockaddr_in to;
    {
        std::lock_guard l(g_client.lock);  // (Retarget changes it from other threads)
        to = g_client.host;
    }
    sendto(g_client.sock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    {   // the record: where the controller packets go and whether anything has come back, every ten seconds while
        // there is no picture yet (the wait that went wrong on 2026-11-26 left nothing in the joiner's log)
        static Clock::time_point said{};
        if (!have && now - g_client.joined > std::chrono::seconds(8) && now - said > std::chrono::seconds(10)) {
            said = now;
            LOGI("net: joining, no picture yet after %.0f s: sending to the host's port %u, %s", std::chrono::duration<double>(now - g_client.joined).count(),
                 unsigned(ntohs(to.sin_port)), HostNotStarted() ? "the host answers (he is in his menus)" : "nothing has come back from the host's game");
        }
    }
}

// the host's remote player's pad, when one is connected
bool RemotePad(input::Pad& p) {
    if (!HostHasPlayer() || !game_coop::InLevel()) return false;  // (in the host's menus only the host steers)
    std::lock_guard l(g_host.lock);
    p.connected = true;
    p.buttons = g_host.pad.buttons;
    std::memcpy(p.analog, g_host.pad.analog, 8);
    p.lx = g_host.pad.lx;
    p.ly = g_host.pad.ly;
    p.rx = g_host.pad.rx;
    p.ry = g_host.pad.ry;
    return true;
}

// once, before the first controller poll
void Init() {
    g_tap_before = audio::g_tap;
    audio::g_tap = SoundTap;
    if (std::getenv("S2_COOP_HOST")) Host(true);
    if (const char* peer = std::getenv("S2_COOP_PEER")) HostExpect(peer);
    if (const char* address = std::getenv("S2_COOP_JOIN")) {
        std::string to = address;
        std::thread([to] {  // (the window and the renderer first)
            std::this_thread::sleep_for(std::chrono::seconds(6));
            JoinAddress(to);
        }).detach();
    }
}
}  // namespace game_net
