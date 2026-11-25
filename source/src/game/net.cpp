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
void RemotePlayer(bool connected);  // the host's remote player came or went
bool SplitActive();                 // player two is in and the screen is split
bool StreamView();                  // player two is in and the frames handed over are his view, whole
}

namespace game_net {
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
constexpr int kFrameInterval = 33;     // ms between frames to the client (30 fps)
constexpr int kJpegQuality = 70;
constexpr size_t kChunk = 1200;
constexpr size_t kFrameHead = 23;         // a frame packet's bytes before the JPEG chunk
constexpr int kClientTimeout = 3000;   // ms without pad packets: the remote player left
constexpr int kHostTimeout = 15000;    // ms without frames (a host loading a level stalls a while): the joiner leaves
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
    std::vector<uint8_t> frame;  // RGB
    uint32_t frame_w = 0, frame_h = 0;
    std::atomic<float> frame_aspect{0};  // the shape that picture is shown in on the host (width / height)
    bool frame_new = false;
    Clock::time_point last_sent;
    sockaddr_in punch{};  // a visitor's outside address, sent to until punch_until (HostBeacon)
    Clock::time_point punch_until{};
} g_host;

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
        std::lock_guard l(g_host.lock);
        bool same = g_host.have_client && from.sin_addr.s_addr == g_host.client.sin_addr.s_addr &&
                    from.sin_port == g_host.client.sin_port;
        if (!same) {
            if (g_host.have_client && Clock::now() - g_host.last_input < std::chrono::milliseconds(kClientTimeout))
                continue;  // one remote player at a time
            g_host.client = from;
            g_host.have_client = true;
            g_host.seq = 0;
        }
        if (int32_t(seq - g_host.seq) > 0 || !g_host.seq) {
            g_host.seq = seq;
            g_host.pad = p;
        }
        g_host.last_input = Clock::now();
        if (!g_host.connected) {
            g_host.connected = true;
            LOGR("coop: remote player joined from %s:%u", IpText(from.sin_addr.s_addr).c_str(), unsigned(ntohs(from.sin_port)));
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

void HostEncode() {
    std::vector<uint8_t> rgb, jpeg;
    uint32_t frame = 0, w = 0, h = 0;
    while (g_host.run) {
        {
            std::unique_lock l(g_host.frame_lock);
            g_host.frame_cv.wait_for(l, std::chrono::milliseconds(100), [] { return g_host.frame_new || !g_host.run; });
            if (!g_host.frame_new) continue;
            g_host.frame_new = false;
            rgb = g_host.frame;
            w = g_host.frame_w;
            h = g_host.frame_h;
        }
        if (!ClientConnected()) continue;
        jpeg.clear();
        auto put = [](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<uint8_t>*>(ctx);
            v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        };
        if (!stbi_write_jpg_to_func(put, &jpeg, int(w), int(h), 3, rgb.data(), kJpegQuality)) continue;
        frame++;
        if (frame == 1 || frame % 300 == 0) LOGI("coop: picture %u to the remote player: %u x %u, %zu bytes", frame, w, h, jpeg.size());
        uint16_t parts = uint16_t((jpeg.size() + kChunk - 1) / kChunk);
        std::vector<uint8_t> pkt(kFrameHead + kChunk);
        float shape = g_host.frame_aspect.load();
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
        }
    }
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
        if (tick % 20 == 0) StunAsk(g_host.sock);  // (see Stun, above)
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
                punch = g_host.punch_until > Clock::now() && !g_host.connected;
                peer = g_host.punch;
            }
            if (punch && i % 2 == 0) {
                const uint8_t hello[5] = {'S', '2', 'C', '1', 6};
                sendto(g_host.sock, reinterpret_cast<const char*>(hello), sizeof hello, 0, reinterpret_cast<sockaddr*>(&peer), sizeof peer);
            }
        }
    }
}

// the renderer's frame (render thread): the half or whole screen as RGB for the encoder, at most every kFrameInterval
void Sink(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch) {
    static std::atomic<uint32_t> calls{0};
    if (++calls == 1) LOGI("coop: the renderer hands over frames (%u x %u)", w, h);
    auto now = Clock::now();
    if (now - g_host.last_sent < std::chrono::milliseconds(kFrameInterval) || !ClientConnected()) return;
    g_host.last_sent = now;
    bool half = game_coop::SplitActive();
    // the right half (and not the divider's own columns), halved again in size when it is large: the picture is
    // JPEG in UDP packets, 30 a second
    uint32_t x0 = half ? w / 2 + std::max<uint32_t>(2, w / 320) : 0, ww = w - x0;
    uint32_t step = ww > 1100 || h > 1100 ? 2 : 1, ow = ww / step, oh = h / step;
    std::lock_guard l(g_host.frame_lock);
    g_host.frame.resize(size_t(ow) * oh * 3);
    for (uint32_t y = 0; y < oh; y++) {
        const uint8_t* s = bgra + size_t(y * step) * pitch + size_t(x0) * 4;
        uint8_t* d = g_host.frame.data() + size_t(y) * ow * 3;
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
                uint8_t* d = g_host.frame.data() + (size_t(y) * ow + size_t(x)) * 3;
                d[0] = red, d[1] = green, d[2] = blue;
            }
    };
    auto bar = [&](int left, int top, int right, int bottom) {
        int filled = left + int(float(right - left) * std::clamp(health, 0.0f, 1.0f));
        rect(left - 1, top - 1, right + 1, bottom + 1, 5, 3, 3);
        rect(left, top, right, bottom, 41, 8, 8);
        rect(left, top, filled, bottom, 204, 56, 61);
    };
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
        int cx = int(ow) / 2, cy = int(oh) / 2;
        int arm = std::max(3, int(5.0f * ky)), gap = std::max(1, int(2.0f * ky)), thick = std::max(1, int(1.0f * ky + 0.5f));
        for (int pass = 0; pass < 2; pass++) {  // the dark edge, then the light cross
            int e = pass ? 0 : 1;
            uint8_t c = pass ? 225 : 20;
            rect(cx - gap - arm - e, cy - e, cx - gap + e, cy + thick + e, c, c, c);
            rect(cx + gap + thick - e, cy - e, cx + gap + thick + arm + e, cy + thick + e, c, c, c);
            rect(cx - e, cy - gap - arm - e, cx + thick + e, cy - gap + e, c, c, c);
            rect(cx - e, cy + gap + thick - e, cx + thick + e, cy + gap + thick + arm + e, c, c, c);
        }
    }
    g_host.frame_aspect = half ? app::g_frame_aspect.load() * float(ww) / float(w) : app::g_frame_aspect.load();
    g_host.frame_w = ow;
    g_host.frame_h = oh;
    g_host.frame_new = true;
    g_host.frame_cv.notify_one();
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
    bool back = false;
    // frames: assembling, and the newest decoded one (BGRA, 640 x 480 letterboxed), handed to the renderer
    std::mutex lock;
    std::vector<uint8_t> shown;
    bool have_frame = false;
    std::vector<uint8_t> present;  // the renderer's copy
} g_client;

// a decoded frame into the 640 x 480 BGRA frame this game shows: the picture keeps the shape it has on the host
// (shape, width / height), centred, with bars where this game's own picture (app::g_frame_aspect) is wider or taller
void Letterbox(const uint8_t* rgba, int w, int h, float shape, std::vector<uint8_t>& out) {
    constexpr int W = 640, H = 480;
    out.assign(size_t(W) * H * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    float mine = app::g_frame_aspect.load();
    if (mine <= 0) mine = 4.0f / 3.0f;
    if (shape <= 0) shape = float(w) / float(h);
    int dw = W, dh = H;  // the part of the 640 x 480 frame the picture fills
    if (shape < mine) dw = std::max(1, int(W * shape / mine + 0.5f));
    else dh = std::max(1, int(H * mine / shape + 0.5f));
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

void ClientRecv() {
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> parts;
    std::map<uint32_t, uint16_t> have;
    uint32_t shown = 0;
    std::vector<uint8_t> buf(2048), frame;
    while (g_client.run) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n = int(recvfrom(g_client.sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &len));
        static std::atomic<uint32_t> packets{0};
        if (n > 0 && ++packets == 1) LOGI("coop: first packet from the host (%d bytes, type %d)", n, n > 4 ? buf[4] : -1);
        if (StunAnswer(buf.data(), n)) continue;                                        // (how the joiner looks from outside)
        if (n >= 5 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 6) continue;  // (the host's hole-punching hello)
        if (n >= 11 && std::memcmp(buf.data(), "S2C1", 4) == 0 && buf[4] == 4) {
            SoundIn(buf.data(), n);
            continue;
        }
        if (n < int(kFrameHead) || std::memcmp(buf.data(), "S2C1", 4) != 0 || buf[4] != 2) continue;
        uint32_t id;
        float shape;
        std::memcpy(&shape, &buf[19], 4);
        uint16_t w, h, count, part, bytes;
        std::memcpy(&id, &buf[5], 4);
        std::memcpy(&w, &buf[9], 2);
        std::memcpy(&h, &buf[11], 2);
        std::memcpy(&count, &buf[13], 2);
        std::memcpy(&part, &buf[15], 2);
        std::memcpy(&bytes, &buf[17], 2);
        if ((id <= shown && shown - id < 1000) || part >= count || int(kFrameHead) + bytes > n) continue;
        auto& p = parts[id];
        if (p.empty()) p.resize(count);
        if (!p[part].empty()) continue;
        p[part].assign(&buf[kFrameHead], &buf[kFrameHead] + bytes);
        if (++have[id] < count) continue;
        frame.clear();
        for (auto& c : p) frame.insert(frame.end(), c.begin(), c.end());
        int iw, ih, comp;
        uint8_t* px = stbi_load_from_memory(frame.data(), int(frame.size()), &iw, &ih, &comp, 4);
        for (auto it = parts.begin(); it != parts.end();) it = it->first <= id ? parts.erase(it) : std::next(it);
        for (auto it = have.begin(); it != have.end();) it = it->first <= id ? have.erase(it) : std::next(it);
        shown = id;
        if (!px) continue;
        std::vector<uint8_t> boxed;
        Letterbox(px, iw, ih, shape, boxed);
        stbi_image_free(px);
        std::lock_guard l(g_client.lock);
        if (!g_client.have_frame) LOGR("coop: joined %s, first picture in", g_client.name.c_str());
        g_client.shown.swap(boxed);
        g_client.have_frame = true;
        g_client.last_frame = Clock::now();
    }
}

// the renderer, each present while joined: the host's picture over the joiner's own (BGRA, 640 x 480)
const uint8_t* Present(uint32_t& w, uint32_t& h) {
    if (!g_client.run) return nullptr;
    std::lock_guard l(g_client.lock);
    if (!g_client.have_frame) return nullptr;
    g_client.present = g_client.shown;
    static uint32_t calls = 0;
    if (++calls % 300 == 1) {  // (a check now and then: is there a picture in it)
        uint64_t sum = 0;
        for (size_t i = 0; i < g_client.present.size(); i += 4 * 37) sum += g_client.present[i] + g_client.present[i + 1] + g_client.present[i + 2];
        LOGI("coop: showing the host's picture (present %u, average level %.1f of 255)", calls,
             double(sum) / double(g_client.present.size() / (4 * 37) * 3 + 1));
    }
    // S2_JOIN_DUMP=<file>: the picture handed to the renderer 20 seconds after joining, as a .bmp (tests: the
    // joiner's own frame capture is taken before this picture goes over it)
    if (calls == 1200) {
        if (const char* path = std::getenv("S2_JOIN_DUMP")) {
            if (FILE* f = std::fopen(path, "wb")) {
                uint32_t head[13] = {640u * 480u * 4u + 54u, 0, 54, 40, 640, uint32_t(-480), 1u | (32u << 16), 0, 640u * 480u * 4u, 2835, 2835, 0, 0};
                std::fwrite("BM", 1, 2, f);
                std::fwrite(head, 4, 13, f);
                std::fwrite(g_client.present.data(), 1, g_client.present.size(), f);
                std::fclose(f);
                LOGI("coop: the picture shown was written to %s", path);
            }
        }
    }
    w = 640;
    h = 480;
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
    if (g_sound.playing && r.size() < size_t(frames)) g_sound.playing = false;  // ran dry: silence until it fills again
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
    g_sound.ring.insert(g_sound.ring.end(), s, s + frames * 2);
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

// the joiner's state for the menu: 0 not joined, 1 connecting, 2 playing in the host's game
int JoinState() {
    if (!g_client.run) return 0;
    std::lock_guard l(g_client.lock);
    return g_client.have_frame ? 2 : 1;
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
    g_client.sock = Bind(std::getenv("S2_COOP_PORT") ? uint16_t(std::atoi(std::getenv("S2_COOP_PORT"))) : uint16_t(0), false);
    if (g_client.sock == kBad) return false;
    g_client.host = {};
    g_client.host.sin_family = AF_INET;
    g_client.host.sin_port = htons(target.port);
    g_client.host.sin_addr.s_addr = target.ip;
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
    uint8_t pkt[27];
    std::memcpy(pkt, "S2C1", 4);
    pkt[4] = 1;
    uint32_t seq = ++g_client.seq;
    std::memcpy(pkt + 5, &seq, 4);
    std::memcpy(pkt + 9, &p.buttons, 2);
    std::memcpy(pkt + 11, p.analog, 8);
    std::memcpy(pkt + 19, &p.lx, 2);
    std::memcpy(pkt + 21, &p.ly, 2);
    std::memcpy(pkt + 23, &p.rx, 2);
    std::memcpy(pkt + 25, &p.ry, 2);
    sendto(g_client.sock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<sockaddr*>(&g_client.host),
           sizeof g_client.host);
}

// the host's remote player's pad, when one is connected
bool RemotePad(input::Pad& p) {
    if (!HostHasPlayer()) return false;
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
