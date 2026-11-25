// Test tooling: what the game is playing, taken from the mixer's output just before it goes to the sound card.
//   S2_AUDIO_WAV=<file>   every block is also written to that file (16 bit stereo, 48 kHz)
//   S2_AUDIO_LOG=1        a log line a second with the peak and the average level of that second
//   S2_FREEZE_REPORT_AT=n one freeze report (watchdog.cpp) n seconds after start, to check the report itself
//   S2_LEVEL=<level>      the game starts in that level, with no menus: a level file under English\NTSC\Levels
//                         without its extension, such as 01_Harbor\01a_Harbor_Base. It fills the game's own
//                         start-level name (0x392030), which its "level" launch option fills (0x163EA0) and its
//                         start-up tests (0x16D77D): the console's debug kit way of jumping to a level.
// None is set in a normal run and nothing is changed or tapped then.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace game_diag {
namespace {
FILE* g_wav = nullptr;
bool g_log = false;
uint64_t g_frames = 0;       // frames written to the file
double g_sum = 0;            // this second's squares
float g_peak = 0;
uint32_t g_second_frames = 0, g_second = 0;

void Header(FILE* f, uint32_t data_bytes) {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    uint32_t riff = 36 + data_bytes, rate = audio::kRate, bytes = audio::kRate * 4;
    uint16_t align = 4, bits = 16;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &bytes, 4);
    std::memcpy(h + 32, &align, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data_bytes, 4);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, f);
    std::fseek(f, 0, SEEK_END);
}

// output thread
void Tap(float* out, uint32_t frames) {
    static std::vector<int16_t> pcm;
    if (g_wav) pcm.resize(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames * 2; i++) {
        float v = out[i];
        g_sum += double(v) * v;
        g_peak = std::fmax(g_peak, std::fabs(v));
        if (g_wav) pcm[i] = int16_t(std::lrintf(std::fmax(-1.0f, std::fmin(1.0f, v)) * 32767.0f));
    }
    if (g_wav) {
        std::fwrite(pcm.data(), 2, pcm.size(), g_wav);
        g_frames += frames;
    }
    g_second_frames += frames;
    if (g_second_frames >= audio::kRate) {
        if (g_log) LOGR("audio: second %u peak %.3f rms %.4f", g_second, g_peak, std::sqrt(g_sum / (g_second_frames * 2.0)));
        if (g_wav) {  // the header follows the file, so a killed run still leaves a file that opens
            Header(g_wav, uint32_t(g_frames * 4));
            std::fflush(g_wav);
        }
        g_second++;
        g_second_frames = 0;
        g_sum = 0;
        g_peak = 0;
    }
}
}  // namespace

// GameInfo::init
void Init() {
    const char* wav = std::getenv("S2_AUDIO_WAV");
    g_log = std::getenv("S2_AUDIO_LOG") != nullptr;
    if (wav && *wav) {
        g_wav = std::fopen(wav, "wb");
        if (g_wav) Header(g_wav, 0);
        else LOGE("diag: cannot write %s", wav);
    }
    if (g_wav || g_log) audio::g_tap = Tap;
}

// GameInfo::loaded: the image is in memory and its entry point has not run
void Loaded() {
    constexpr uint32_t kStartLevel = 0x392030, kStartLevelBytes = 0x104;
    const char* level = std::getenv("S2_LEVEL");
    if (!level || !*level) return;
    std::string path = std::string("d:\\English\\NTSC\\Levels\\") + level + ".lvl";
    if (path.size() >= kStartLevelBytes) return;
    std::memcpy(kernel::HostPtr(kStartLevel), path.c_str(), path.size() + 1);
    LOGR("diag: start level %s", path.c_str());
}
}  // namespace game_diag
