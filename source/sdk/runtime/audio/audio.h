#pragma once
#include <cstdint>
#include <atomic>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// sees each rendered block before it plays and may change it (output thread): online co-op sends the host's sound
// on and plays it on the joiner in place of the joiner's own
inline void (*g_tap)(float* out, uint32_t frames) = nullptr;

// [local change, The Suffering: Ties That Bind] HD sound (src\game\hdsound.cpp): enhanced copies of the title's ADPCM
// waves, made on the player's PC. The mixer asks for the copy of a region of guest memory by its first block, checks
// each block it plays against the copy's record of the console bytes, and reads the copy in place of the ADPCM.
struct HdWave;  // the game layer's
struct HdHooks {
    // the wave whose block *index holds these 36 * channels bytes: the hint's block hint_block first, then the
    // waves that start with these bytes (index 0); nullptr when none
    // (blocks: the region's length in blocks when known, else 0: of several waves that start alike, the one as long)
    const HdWave* (*find)(const uint8_t* block, uint32_t channels, uint32_t rate, uint32_t blocks, const HdWave* hint,
                          uint32_t hint_block, uint32_t* index);
    // does the wave's block index hold these bytes
    bool (*check)(const HdWave* wave, uint32_t index, const uint8_t* block);
    // channel c at console frame `frame` of the wave (fractional), read with a cubic; mixer thread, under its lock
    float (*sample)(const HdWave* wave, double frame, uint32_t c);
};
inline const HdHooks* g_hd = nullptr;  // set while HD sound is on and a set is made
inline bool g_cubic = false;           // every voice read with a cubic in place of straight lines (HD sound on)
inline std::atomic<uint32_t> g_mix_longest_us{0};  // the longest mix of a block since the game layer last read it

}  // namespace audio
