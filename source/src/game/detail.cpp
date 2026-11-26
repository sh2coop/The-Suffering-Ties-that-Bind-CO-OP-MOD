// The detail record: everything too bulky for the session log, written all the time (asked for 2026-11-26: "the
// logs need to be extremely detailed, to catch graphics etc."; "i dont think the fps thing would be an issue").
// Beside the session log suffering2_NNN.log:
//   suffering2_NNN_detail.log.gz   one line for every frame of the game (both players, both cameras, what each view
//                                  drew and with what fog, light and projection) and, once a second, every object
//                                  that appeared, moved, changed or ended (coop.cpp); on a joined player's PC a line
//                                  for every picture that came from the host (net.cpp). Text, packed (gzip: 7-Zip,
//                                  WinRAR and Windows 11's Explorer open it). Each line starts with the seconds of
//                                  the session log's clock, so the two files read side by side.
//   suffering2_NNN_pictures\       a small picture of the game's window every few seconds, the picture the joined
//                                  player is sent, and the full-size pictures F9 marks (record.cpp, net.cpp).
// Kept for the three newest sessions (the session logs themselves for thirty): about 20 MB of detail and 70 MB of
// pictures for an hour of play. S2_NO_DETAIL=1 switches it off.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"

// (third_party/stb_image_write.h, built in net.cpp: its deflate)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace game_detail {
namespace {
namespace fs = std::filesystem;
std::mutex g_lock;
std::string g_text;                 // lines waiting to be packed
std::string g_file, g_pictures;     // "" until Init
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_lines{0}, g_packed{0}, g_raw{0};

uint32_t Crc(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// one gzip member at the end of the file (a file of several members is a gzip file of them all)
void Pack(const std::string& text) {
    if (text.empty()) return;
    int n = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(text.data())), int(text.size()), &n, 8);
    if (!z || n < 6) {
        std::free(z);
        return;
    }
    if (FILE* f = std::fopen(g_file.c_str(), "ab")) {
        const uint8_t head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
        uint32_t tail[2] = {Crc(reinterpret_cast<const uint8_t*>(text.data()), text.size()), uint32_t(text.size())};
        std::fwrite(head, 1, sizeof head, f);
        std::fwrite(z + 2, 1, size_t(n) - 6, f);  // (the deflate stream inside its zlib wrapper)
        std::fwrite(tail, 4, 2, f);
        std::fclose(f);
        g_packed += uint64_t(n) + 12;
        g_raw += text.size();
    }
    std::free(z);
}

void Run() {
    auto said = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string text;
        {
            std::lock_guard l(g_lock);
            text.swap(g_text);
        }
        Pack(text);
        auto now = std::chrono::steady_clock::now();
        if (now - said > std::chrono::minutes(5)) {
            said = now;
            LOGI("detail: %llu lines so far, %.1f MB packed into %.1f MB", (unsigned long long)g_lines.load(), g_raw.load() / 1048576.0, g_packed.load() / 1048576.0);
        }
    }
}

// the run number in "suffering2_012.log", "suffering2_012_detail.log.gz", "suffering2_012_pictures" (0: another name)
int RunOf(const std::string& name, const std::string& prefix) {
    if (name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_")) return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}
}  // namespace

bool On() { return g_on.load(); }
std::string PicturesDir() { return g_pictures; }

void Line(const char* fmt, ...) {
    if (!g_on.load()) return;
    char msg[2048];
    int head = std::snprintf(msg, 16, "%.3f ", logx::Seconds());
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(msg + head, sizeof msg - size_t(head) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n = std::min(n + head, int(sizeof msg) - 2);
    msg[n++] = '\n';
    g_lines++;
    std::lock_guard l(g_lock);
    if (g_text.size() < (8u << 20)) g_text.append(msg, size_t(n));  // (a writer that fell behind loses lines, not memory)
}

// GameInfo::init
void Init() {
    if (std::getenv("S2_NO_DETAIL")) {
        LOGR("detail: switched off (S2_NO_DETAIL)");
        return;
    }
    fs::path log = logx::Path();
    if (log.empty()) return;
    std::string stem = log.stem().string();  // suffering2_012
    size_t under = stem.rfind('_');
    std::string prefix = under == std::string::npos ? stem : stem.substr(0, under);
    int mine = RunOf(stem, prefix);
    std::error_code ec;
    // the three newest sessions' detail and pictures stay, older ones go
    constexpr int kKeep = 3;
    if (mine) {
        for (const auto& e : fs::directory_iterator(log.parent_path(), ec)) {
            std::string name = e.path().filename().string();
            bool ours = name.find("_detail.log.gz") != std::string::npos || (e.is_directory(ec) && name.size() > 9 && name.compare(name.size() - 9, 9, "_pictures") == 0);
            int run = ours ? RunOf(name, prefix) : 0;
            if (run && (run <= mine - kKeep || run > mine)) fs::remove_all(e.path(), ec);
        }
    }
    g_file = (log.parent_path() / (stem + "_detail.log.gz")).string();
    g_pictures = (log.parent_path() / (stem + "_pictures")).string();
    fs::create_directories(g_pictures, ec);
    g_on = true;
    LOGR("detail: every frame and every object go to %s, pictures to %s (the three newest sessions are kept)", fs::path(g_file).filename().string().c_str(),
         fs::path(g_pictures).filename().string().c_str());
    Line("detail: lines start with the session log's seconds. f = a frame (coop.cpp says what its fields are in the first line it writes), o = an object, x = an object that ended");
    std::thread(Run).detach();
}
}  // namespace game_detail
