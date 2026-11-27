// HD sound (asked for on 2026-11-27: "HD" for the sound as for the textures, made on each player's own PC).
//
// Build Recomp's maker (tools\launcher\hdsound.h) writes an enhanced copy of each of the game's waves under 32 kHz
// into Game Files\sound\hd: resampled to 32 kHz or more with a long sinc filter, the octave above the original's top
// made back (a 22 kHz voice stops at 11 kHz). This file finds them for the mixer (sdk\runtime\xdk\dsound.cpp, local
// change: audio::g_hd) and reads them; with HD sound on, every other voice is read with a cubic in place of the
// console's straight lines (audio::g_cubic), which takes off the dull top and the mirror tones above it.
//
// How a voice is matched to a copy: the game hands the mixer guest memory holding a wave's ADPCM bytes (a buffer's
// data, a stream's packets). The index (index.bin) lists each copy by its first block; a stream's next packet is
// tried as the next blocks of the copy before that. Each block the mixer comes to is checked against the copy's record
// of the console's bytes (hdsound_format.h), so a copy never plays for memory that holds something else.
//
// The setting `hd_sound` in settings.toml (Options, Display and Co-op, Sound settings, "HD sound"): 0 off (the
// default), 1 on. On and not made yet starts the maker ("Build Recomp.exe --hd-sound"); copies made meanwhile are
// picked up as the maker rewrites its index. Test switch: S2_HD_SOUND=0 / 1 overrides it (and never starts the maker).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"
#include "hdsound_format.h"

namespace audio {
// one copy: its file mapped, and where its parts are
struct HdWave {
    HsIndexEntry entry{};
    bool open = false, broken = false;
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* base = nullptr;
    size_t size = 0;
    HsHeader head{};
    const uint32_t* check = nullptr;  // blocks
    const uint32_t* chunk = nullptr;  // chunks + 1
    int slot[2] = {-1, -1};           // the decoded chunks it read last
};
}  // namespace audio

namespace game_hdsound {
uint32_t g_setting = 0;  // settings.toml hd_sound: off unless the player picks it (at Build Recomp or in Options)

void Report();

namespace {
using audio::HdWave;

std::vector<std::unique_ptr<HdWave>> g_waves;
std::unordered_multimap<uint64_t, HdWave*> g_first;  // by the HsKey of the first block
std::filesystem::path g_dir;
std::atomic<uint64_t> g_found{0}, g_followed{0}, g_missed{0}, g_missed_low{0};
size_t g_opened = 0;
std::mutex g_index_lock;  // g_first and g_waves: the maker's progress adds copies while sounds are looked up
int g_progress_made = 0, g_progress_total = 0;

// decoded chunks, shared by all copies (the mixer thread reads them, under its lock)
struct Slot {
    const HdWave* wave = nullptr;
    uint32_t chunk = 0;
    uint64_t used = 0;
    int16_t pcm[HS_CHUNK * 2];
};
constexpr int kSlots = 128;
Slot g_slot[kSlots];
uint64_t g_clock = 0;
int16_t g_scratch[HS_CHUNK];

bool Open(HdWave& w) {
    if (w.open) return !w.broken;
    w.open = true;
    char name[32];
    std::snprintf(name, sizeof name, "%016llX.hsw", (unsigned long long)w.entry.key);
    std::filesystem::path path = g_dir / name;
    w.broken = true;
    w.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (w.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(w.file, &size);
    w.size = size_t(size.QuadPart);
    if (w.size < sizeof(HsHeader)) return false;
    w.map = CreateFileMappingW(w.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!w.map) return false;
    w.base = static_cast<const uint8_t*>(MapViewOfFile(w.map, FILE_MAP_READ, 0, 0, 0));
    if (!w.base) return false;
    std::memcpy(&w.head, w.base, sizeof w.head);
    const HsHeader& h = w.head;
    size_t tables = sizeof h + 4ull * h.blocks + 4ull * (h.chunks + 1ull);
    if (h.magic != HS_MAGIC || h.version != HS_VERSION || h.blocks != w.entry.blocks || h.channels != w.entry.channels ||
        h.channels < 1 || h.channels > 2 || !h.mult || tables > w.size || h.chunks != (h.frames + HS_CHUNK - 1) / HS_CHUNK)
        return false;
    w.check = reinterpret_cast<const uint32_t*>(w.base + sizeof h);
    w.chunk = w.check + h.blocks;
    if (w.chunk[h.chunks] != w.size) return false;
    // the start of the sound is read ahead now, the rest as it plays
    WIN32_MEMORY_RANGE_ENTRY ahead{const_cast<uint8_t*>(w.base), w.size < (1u << 20) ? w.size : (1u << 20)};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &ahead, 0);
    w.broken = false;
    g_opened++;
    LOGI("HD sound: copy %016llX opened (%u ch, %u Hz x%u, %.2f s, flags %u), %zu open", (unsigned long long)w.entry.key,
         h.channels, h.rate, h.mult, double(h.frames) / (double(h.rate) * h.mult), h.flags, g_opened);
    return true;
}

const int16_t* Chunk(HdWave& w, uint32_t chunk) {
    for (int s : w.slot)
        if (s >= 0 && g_slot[s].wave == &w && g_slot[s].chunk == chunk) {
            g_slot[s].used = ++g_clock;
            return g_slot[s].pcm;
        }
    int pick = 0;
    for (int i = 1; i < kSlots; i++)
        if (g_slot[i].used < g_slot[pick].used) pick = i;
    Slot& s = g_slot[pick];
    uint32_t first = chunk * HS_CHUNK, n = w.head.frames - first < HS_CHUNK ? w.head.frames - first : HS_CHUNK;
    uint32_t at = w.chunk[chunk], end = w.chunk[chunk + 1];
    if (at > end || end > w.size || !HsDecodeChunk(w.base + at, end - at, n, w.head.channels, s.pcm, g_scratch)) {
        LOG_FIRST(8, "HD sound: copy %016llX chunk %u does not decode", (unsigned long long)w.entry.key, chunk);
        return nullptr;
    }
    s.wave = &w;
    s.chunk = chunk;
    s.used = ++g_clock;
    w.slot[1] = w.slot[0];
    w.slot[0] = pick;
    return s.pcm;
}

float Frame(HdWave& w, int64_t i, uint32_t c, const int16_t*& last_pcm, uint32_t& last_chunk) {
    if (i < 0) i = 0;
    if (i >= int64_t(w.head.frames)) i = int64_t(w.head.frames) - 1;
    uint32_t chunk = uint32_t(i / HS_CHUNK);
    if (!last_pcm || chunk != last_chunk) {
        last_pcm = Chunk(w, chunk);
        last_chunk = chunk;
        if (!last_pcm) return 0.0f;
    }
    return last_pcm[(i % HS_CHUNK) * w.head.channels + (c < w.head.channels ? c : 0)] / 32768.0f;
}

bool g_whole = false;
void Load();

bool Check(const HdWave* wave, uint32_t index, const uint8_t* block) {
    const HdWave& w = *wave;
    if (w.broken || index >= w.head.blocks) return false;
    return HsCheck(block, 36u * w.head.channels) == w.check[index];
}

const HdWave* Find(const uint8_t* block, uint32_t channels, uint32_t rate, uint32_t blocks, const HdWave* hint,
                   uint32_t hint_block, uint32_t* index) {
    (void)rate;
    static ULONGLONG last_report = GetTickCount64();
    if (GetTickCount64() - last_report > 10000) {
        last_report = GetTickCount64();
        Report();
        if (!g_whole) Load();  // copies the maker has made since
    }
    if (hint && hint->head.channels == channels && Check(hint, hint_block, block)) {
        *index = hint_block;
        g_followed++;
        return hint;
    }
    uint64_t key = HsKey(block, 36u * channels);
    std::lock_guard<std::mutex> lock(g_index_lock);
    auto range = g_first.equal_range(key);
    // several waves can start alike (variants of one sound): the one as long as the region wins, else the first
    HdWave* first = nullptr;
    for (auto it = range.first; it != range.second; ++it) {
        HdWave& w = *it->second;
        if (w.entry.channels != channels || !Open(w) || !Check(&w, 0, block)) continue;
        if (!first) first = &w;
        if (blocks && w.entry.blocks == blocks) {
            first = &w;
            break;
        }
    }
    if (first) {
        *index = 0;
        g_found++;
        return first;
    }
    // waves of 32 kHz and up have no copy by design; one under that should have had one
    if (rate < 32000) {
        g_missed_low++;
        LOG_FIRST(40, "HD sound: no copy for a %u Hz, %u channel sound (first block %016llX, %s)", rate, channels,
                  (unsigned long long)key, range.first == range.second ? "no copy starts so" : "a copy starts so");
    } else {
        g_missed++;
    }
    return nullptr;
}

float Sample(const HdWave* wave, double frame, uint32_t c) {
    HdWave& w = *const_cast<HdWave*>(wave);
    double p = frame * w.head.mult;
    int64_t a = int64_t(p);
    float t = float(p - double(a));
    const int16_t* pcm = nullptr;
    uint32_t chunk = 0;
    float p0 = Frame(w, a - 1, c, pcm, chunk), p1 = Frame(w, a, c, pcm, chunk), p2 = Frame(w, a + 1, c, pcm, chunk),
          p3 = Frame(w, a + 2, c, pcm, chunk);
    return HsCubic(p0, p1, p2, p3, t);
}

const audio::HdHooks kHooks = {Find, Check, Sample};
std::filesystem::file_time_type g_index_time{};
bool g_maker_started = false;

std::filesystem::path Dir() {
    if (g_dir.empty()) g_dir = app::ExeDir() / "sound" / "hd";
    return g_dir;
}

// reads the index when it is new or has changed (the maker rewrites it as it goes): copies not known yet are added,
// none is ever taken away (the mixer may hold one)
void Load() {
    std::error_code ec;
    std::filesystem::path index = Dir() / "index.bin";
    auto when = std::filesystem::last_write_time(index, ec);
    if (ec || when == g_index_time) return;
    g_index_time = when;
    g_whole = std::filesystem::exists(Dir() / "made.txt", ec);
    std::FILE* f = _wfopen(index.c_str(), L"rb");
    if (!f) return;
    uint32_t head[3] = {};
    std::vector<HsIndexEntry> e;
    if (std::fread(head, 4, 3, f) == 3 && head[0] == HS_INDEX_MAGIC && head[1] == HS_VERSION && head[2] < 1000000) {
        e.resize(head[2]);
        if (std::fread(e.data(), sizeof(HsIndexEntry), e.size(), f) != e.size()) e.clear();
    }
    std::fclose(f);
    std::lock_guard<std::mutex> lock(g_index_lock);
    size_t before = g_waves.size();
    for (const HsIndexEntry& x : e) {
        bool known = false;
        auto range = g_first.equal_range(x.first);
        for (auto it = range.first; it != range.second && !known; ++it) known = it->second->entry.key == x.key;
        if (known) continue;
        auto w = std::make_unique<HdWave>();
        w->entry = x;
        g_first.emplace(x.first, w.get());
        g_waves.push_back(std::move(w));
    }
    LOGR("HD sound: the set has %zu copies (%zu new)%s", g_waves.size(), g_waves.size() - before,
         g_whole ? "" : "; not whole yet: what is not made plays as on the console");
}

// Build Recomp's maker, in the background, once a run ("Build Recomp.exe --hd-sound": tools\launcher\launcher.c)
void StartMaker() {
    if (g_maker_started) return;
    g_maker_started = true;
    std::filesystem::path exe = app::ExeDir().parent_path() / "Build Recomp.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\" --hd-sound";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        exe.parent_path().c_str(), &si, &pi)) {
        LOGW("HD sound: %s could not be started (%lu)", exe.string().c_str(), GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOGR("HD sound: the maker started in the background");
}
}  // namespace

// at start and when the setting changes: the mixer reads what the setting says; on and not whole starts the maker
void Apply() {
    uint32_t on = g_setting;
    if (const char* e = std::getenv("S2_HD_SOUND")) on = std::atoi(e) != 0;
    if (on) {
        Load();
        if (!g_whole && !std::getenv("S2_HD_SOUND")) StartMaker();  // (a test run never starts it)
    }
    audio::g_cubic = on != 0;
    audio::g_hd = on ? &kHooks : nullptr;
    LOGR("HD sound: %s", !on ? "off" : g_whole ? "on" : "on, the set is being made");
}

// for the Options row: 0 off, 1 on, 2 on and being made (made of total so far)
int State(int* made, int* total) {
    *made = *total = 0;
    if (!g_setting) return 0;
    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 1000) {  // the menu asks every frame
        last = GetTickCount64();
        Load();
        if (std::FILE* f = _wfopen((Dir() / "progress.txt").c_str(), L"r")) {
            static int m = 0, t = 0;
            if (std::fscanf(f, "%d %d", &m, &t) != 2) m = t = 0;
            std::fclose(f);
            g_progress_made = m;
            g_progress_total = t;
        }
    }
    *made = g_progress_made;
    *total = g_progress_total;
    return g_whole ? 1 : 2;
}

// one line for the session record: how voices were matched since the last call
void Report() {
    uint64_t found = g_found.exchange(0), followed = g_followed.exchange(0), missed = g_missed.exchange(0),
             low = g_missed_low.exchange(0);
    if (found || followed || missed || low)
        LOGI("HD sound: %llu sounds started from a copy, %llu stream packets followed one, %llu of 32 kHz and up (no copy "
             "needed), %llu under 32 kHz without a copy; %zu copies open",
             (unsigned long long)found, (unsigned long long)followed, (unsigned long long)missed,
             (unsigned long long)low, g_opened);
}
}  // namespace game_hdsound
