// The meeting point: one short code instead of two internet addresses swapped by hand (asked for 2026-11-26, the
// study "Co-op joining: the easy way"). The host's game asks a small service on the internet for a code like
// WOLF-4271 and tells it this PC's game port as seen from outside; the friend's game sends the code and gets that
// address, then leaves its own; the host's game picks that up and lets him in (net.cpp HostExpect, as when the host
// typed the friend's code by hand). Only the two addresses pass through the service (meeting-point/worker.js in this
// repository): picture, sound and controls go from PC to PC as before.
//   S2_MEET_URL=<address>  another meeting point (tests: tools/meetlocal.py on this PC); "off" for none
// Without a meeting point (none set up, or it cannot be reached) the address codes work as before.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "core/log.h"

namespace game_net {  // net.cpp
std::string OutsideAddress();
std::string LocalAddress();
int OutsideKind();
bool Hosting();
bool HostHasPlayer();
bool HostExpect(const std::string& address);
int JoinState();
int JoinPhase();
bool JoinRetarget(const std::string& address);
}

namespace game_meet {
namespace {
using Clock = std::chrono::steady_clock;

// The meeting point's address. Empty until the service is online: the menu then offers the address codes alone.
constexpr char kDefaultUrl[] = "https://s2-meet.meeting-point.workers.dev";

std::string Url() {
    static const std::string url = [] {
        const char* v = std::getenv("S2_MEET_URL");
        std::string u = v ? v : kDefaultUrl;
        if (u == "off") u.clear();
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }();
    return url;
}

std::mutex g_lock;
std::string g_code;          // the host's code, "" none
std::string g_note;          // what went wrong last, in plain words ("" nothing)
int g_host_state = 0;        // 0 not inviting, 1 asking for a code, 2 the code is there, 3 no code (see the note)
std::string g_join_code;     // the joiner: the code typed, while its join is on the way
std::string g_join_host;     // ... the host's address the meeting point gave, for the menu to join ("" none yet)
int g_join_state = 0;        // 0 nothing, 1 asking the meeting point, 2 the address is there, 3 failed (see the note)

void Say(const std::string& note) {
    std::lock_guard l(g_lock);
    g_note = note;
}

#ifdef _WIN32
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// One POST of a small JSON body: the HTTP status (0: no answer) and the answer's text
int Post(const std::string& path, const std::string& body, std::string& answer) {
    answer.clear();
    std::string url = Url() + path;
    std::wstring wide = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256], way[512];
    parts.lpszHostName = host, parts.dwHostNameLength = 256;
    parts.lpszUrlPath = way, parts.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return 0;
    int status = 0;
    HINTERNET session = WinHttpOpen(L"S2-coop", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"S2-coop", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", way, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request && WinHttpSendRequest(request, L"content-type: application/json\r\n", DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()),
                                      DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = int(code);
        char buf[512];
        DWORD got = 0;
        while (answer.size() < 2000 && WinHttpReadData(request, buf, sizeof buf, &got) && got) answer.append(buf, got);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}
#else
int Post(const std::string&, const std::string&, std::string&) { return 0; }
#endif

// "key":"value" out of the answer ("" when it is not there or not a string)
std::string Field(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k);
    if (at == std::string::npos) return "";
    at = json.find(':', at + k.size());
    if (at == std::string::npos) return "";
    at = json.find_first_not_of(" \t", at + 1);
    if (at == std::string::npos || json[at] != '"') return "";
    size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

const char kNoMeeting[] = "The meeting point cannot be reached. Check your internet connection and try again.";

void Run() {
    std::string secret;
    {
        std::random_device rd;
        char hex[17];
        for (int i = 0; i < 2; i++) std::snprintf(hex, sizeof hex, "%08x%08x", rd(), rd()), secret += hex;
    }
    std::string code, told_addr, last_joiner, answer;
    Clock::time_point told{}, asked{}, expected{}, join_told{};
    bool was_hosting = false;
    LOGR("meet: the meeting point is %s", Url().c_str());
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Clock::time_point now = Clock::now();
        // ---- the host
        bool hosting = game_net::Hosting();
        if (!hosting && was_hosting) {
            if (!code.empty()) Post("/stop", "{\"code\":\"" + code + "\",\"secret\":\"" + secret + "\"}", answer), LOGR("meet: the code %s is given up", code.c_str());
            code.clear(), told_addr.clear(), last_joiner.clear();
            std::lock_guard l(g_lock);
            g_code.clear(), g_host_state = 0;
        }
        was_hosting = hosting;
        if (hosting) {
            std::string addr = game_net::OutsideAddress();
            // (this PC's address on its own network too: a friend behind the same router is given that one)
            std::string lan = game_net::LocalAddress();
            if (lan.empty() || lan.find_first_not_of("0123456789.") != std::string::npos) lan.clear();
            static std::string told_lan;
            bool due = code.empty() ? now - told > std::chrono::seconds(5) : (addr != told_addr || lan != told_lan || now - told > std::chrono::minutes(5));
            if (!addr.empty() && due) {
                if (code.empty()) {
                    std::lock_guard l(g_lock);
                    if (g_host_state != 3) g_host_state = 1;
                }
                told = now;
                bool dotted = !lan.empty();
                told_lan = lan;
                int status = Post("/host", "{\"addr\":\"" + addr + "\",\"secret\":\"" + secret + "\"" + (code.empty() ? "" : ",\"code\":\"" + code + "\"") +
                                               (dotted ? ",\"lan\":\"" + lan + ":27025\"" : "") + "}", answer);
                std::string got = Field(answer, "code");
                if (status == 200 && got.size() == 9) {
                    if (got != code) LOGR("meet: this game's code is %s (for %s)", got.c_str(), addr.c_str());
                    code = got, told_addr = addr;
                    std::lock_guard l(g_lock);
                    g_code = code, g_host_state = 2, g_note.clear();
                } else {
                    LOGR("meet: no code from the meeting point (status %d, %s)", status, answer.substr(0, 80).c_str());
                    if (code.empty()) {
                        std::lock_guard l(g_lock);
                        g_host_state = 3, g_note = kNoMeeting;
                    }
                }
            }
            // has a friend typed the code? Asked every three seconds while nobody is in.
            if (!code.empty() && !game_net::HostHasPlayer() && now - asked > std::chrono::seconds(3)) {
                asked = now;
                int status = Post("/wait", "{\"code\":\"" + code + "\",\"secret\":\"" + secret + "\"}", answer);
                std::string joiner = Field(answer, "joiner");
                if (status == 404) {
                    LOGR("meet: the code %s ran out, a new one is asked for", code.c_str());
                    code.clear(), told = {};
                } else if (status == 200 && !joiner.empty() && (joiner != last_joiner || now - expected > std::chrono::seconds(20))) {
                    last_joiner = joiner, expected = now;
                    bool ok = game_net::HostExpect(joiner);
                    LOGR("meet: a friend typed the code: he comes from %s, %s", joiner.c_str(), ok ? "the way in is opened for him" : "but this game is not hosting");
                }
            }
        }
        // ---- the one who joins
        std::string join_code, join_host;
        int join_state;
        {
            std::lock_guard l(g_lock);
            join_code = g_join_code, join_state = g_join_state, join_host = g_join_host;
        }
        if (join_state == 1) {
            int status = Post("/join", "{\"code\":\"" + join_code + "\"}", answer);
            std::string host = Field(answer, "host"), lan = Field(answer, "lan");
            if (status == 200 && !lan.empty()) {
                LOGR("meet: the host is on this PC's own network, at %s", lan.c_str());
                host = lan;
            }
            std::lock_guard l(g_lock);
            if (g_join_code != join_code) continue;  // (another code was typed meanwhile)
            if (status == 200 && !host.empty()) {
                g_join_host = host, g_join_state = 2, g_note.clear();
                LOGR("meet: the code %s is a game at %s", join_code.c_str(), host.c_str());
            } else if (status == 404) {
                g_join_state = 3, g_note = "Code not found. Check it with your friend. A code stops working when your friend closes the game.";
                LOGR("meet: the code %s is not known to the meeting point", join_code.c_str());
            } else {
                g_join_state = 3, g_note = kNoMeeting;
                LOGR("meet: no answer about the code %s (status %d)", join_code.c_str(), status);
            }
            join_told = {};
        }
        // this PC's own address goes to the meeting point until the host has answered (he opens the way in for it)
        if (join_state == 2 && game_net::JoinState() == 1 && game_net::JoinPhase() == 1 && now - join_told > std::chrono::seconds(4)) {
            std::string mine = game_net::OutsideAddress();
            if (!mine.empty()) {
                join_told = now;
                int status = Post("/join", "{\"code\":\"" + join_code + "\",\"addr\":\"" + mine + "\"}", answer);
                // (the address itself is not written: logs are sent around. Its port is what matters here.)
                size_t colon = mine.rfind(':');
                LOGI("meet: this PC's address (outside port %s) is left for the host (status %d)", colon == std::string::npos ? "?" : mine.c_str() + colon + 1, status);
                // The answer says where the host's game is NOW: its outside port can change while this one waits
                // (2026-11-26: it did, and this game sent to the old port for 90 s). net.cpp sends there from now on.
                std::string host = Field(answer, "host"), lan = Field(answer, "lan");
                if (status == 200 && !lan.empty()) host = lan;
                if (status == 200 && !host.empty()) game_net::JoinRetarget(host);
            }
        }
        {   // the join this code started is over (in the game, or given up): the code is done with
            static bool seen = false;
            if (join_state == 2 && game_net::JoinState() != 0) seen = true;
            if (join_state == 2 && seen && (game_net::JoinState() == 0 || game_net::JoinState() == 2)) {
                seen = false;
                std::lock_guard l(g_lock);
                if (g_join_code == join_code) g_join_state = 0;
            }
            if (join_state != 2) seen = false;
        }
    }
}
}  // namespace

bool Available() { return !Url().empty(); }

// the host, for the menu: 0 not inviting, 1 asking for a code, 2 the code is there, 3 no code (Note says why)
int HostState() {
    std::lock_guard l(g_lock);
    return g_host_state;
}
std::string HostCode() {
    if (const char* fake = std::getenv("COOP_FAKE_MEET_CODE")) return fake;  // harness: pictures of the menu
    std::lock_guard l(g_lock);
    return g_code;
}
std::string Note() {
    std::lock_guard l(g_lock);
    return g_note;
}
void ClearNote() { Say(""); }

// "wolf4271", " WOLF-4271 " and the like as WOLF-4271; "" when it is not a code
std::string Tidy(const std::string& typed) {
    std::string letters, digits;
    for (char c : typed) {
        if (std::isalpha(static_cast<unsigned char>(c)) && digits.empty()) letters += char(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        else if (c != '-' && c != ' ' && c != '\t' && c != '\r' && c != '\n') return "";
    }
    return letters.size() == 4 && digits.size() == 4 ? letters + "-" + digits : "";
}

// the joiner typed a code: the meeting point is asked whose game it is
bool Join(const std::string& typed) {
    std::string code = Tidy(typed);
    if (code.empty() || !Available()) return false;
    std::lock_guard l(g_lock);
    g_join_code = code, g_join_host.clear(), g_join_state = 1, g_note.clear();
    return true;
}
// 0 nothing, 1 asking the meeting point, 2 the host's address is there, 3 failed (Note says why)
int JoinLookup() {
    std::lock_guard l(g_lock);
    return g_join_state;
}
// the host's address, once, for the menu to join it
bool TakeHost(std::string& address) {
    std::lock_guard l(g_lock);
    if (g_join_state != 2 || g_join_host.empty()) return false;
    address = g_join_host;
    g_join_host.clear();
    return true;
}
void JoinForget() {
    std::lock_guard l(g_lock);
    g_join_code.clear(), g_join_host.clear(), g_join_state = 0;
}

// GameInfo::init
void Init() {
    if (!Available()) {
        LOGR("meet: no meeting point is set up, joining works with the address codes");
        return;
    }
    std::thread(Run).detach();
}
}  // namespace game_meet
