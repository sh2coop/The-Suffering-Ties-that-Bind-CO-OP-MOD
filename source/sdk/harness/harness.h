#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

// state the headless host's parts share
namespace harness {

inline std::atomic<uint64_t> g_frames{0};  // frames presented
// a pad script line's frame may start with '+': counted from this frame, which the title's code sets at a moment of
// its own (a level's play, a scripted warp); 0 while unset, when such lines wait
inline std::atomic<uint64_t> g_script_base{0};

// an environment number, fallback when unset
inline long Env(const char* name, long fallback = 0) {
    const char* v = std::getenv(name);
    return v && *v ? std::atol(v) : fallback;
}

}  // namespace harness
