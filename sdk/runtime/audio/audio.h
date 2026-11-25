#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// sees each rendered block before it plays and may change it (output thread): online co-op sends the host's sound
// on and plays it on the joiner in place of the joiner's own
inline void (*g_tap)(float* out, uint32_t frames) = nullptr;

}  // namespace audio
