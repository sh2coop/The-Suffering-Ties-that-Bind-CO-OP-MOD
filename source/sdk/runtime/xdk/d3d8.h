#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the vertical blank clock the title sees: count, event and callback at this rate (60 as the console)
void SetVBlankHz(uint32_t hz);
uint32_t VBlankHz();

}  // namespace d3d
