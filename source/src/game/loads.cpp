// A load that is done and not seen as done: the rare start-up hang (about 1 start in 100).
//
// A resource's state is the low two bits of the 16-bit word at resource +0x0E (1 wanted, 2 loading, 3 loaded); the
// same word holds a "keep" flag (bit 3), a priority (bits 5 to 8) and more. The file thread marks a resource loaded
// with one instruction, `or byte ptr [res+0x0E], 3` (0x1FC223, the first of 0x1FC220). The main thread, waiting
// for a screen's resources (0x13E890 asks 0x13E420 again and again, which asks the resource manager, 0x1A54B0, for
// each), changes the same word on every ask with one instruction too: `xor [res+0x0E], dx` (0x1A5542; 0x1A520B is
// its twin for a new resource), outside any critical section.
//
// On the console that is safe: it has one processor, and an instruction is never cut in two by a thread switch. In
// the recompiled program each of those instructions is a read, a change and a write, and the two threads really
// run at once: when the main thread has read the word, the file thread sets "loaded", and the main thread writes
// its copy back, the mark is gone. The main thread then waits for good for a load that is long finished, and the
// file thread waits for work: exactly the stacks of runs\startup_hang_20261125_0101.log. (The fourth way the game
// leans on one processor; the other three are in xbox\NOTES.md, "Threads".)
//
// The fix puts a lost mark back, there and then. The late write lands within a few instructions of the mark. So
// when the file thread has marked a resource loaded and the state reads "loading" again as the call returns, a
// microsecond later, it is the other thread's write of its old copy: nothing sends a resource back to loading that
// fast (asking for it again goes through the manager's lock and a new job). The mark is set again (one locked
// instruction) and the log says so.
// (A first version looked for 100 ms after the call. That was wrong: streamed resources really go back to loading
// 40 to 100 ms after each part is in, and were "repaired" six times in one run.)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game_loads {
namespace {
constexpr uint32_t kState = 0x0E;
constexpr uint8_t kLoading = 2, kLoaded = 3;
const bool g_off = std::getenv("S2_NO_LOAD_REPAIR") != nullptr;  // tests: the game as it was

void SetLoadedAgain(uint8_t* word) {
#ifdef _WIN32
    InterlockedOr8(reinterpret_cast<char*>(word), kLoaded);
#else
    __atomic_fetch_or(word, kLoaded, __ATOMIC_SEQ_CST);
#endif
}
}  // namespace
}  // namespace game_loads

// this resource is loaded (this = resource): the file thread, when a resource's data is in
X86_EXTERN_FUNC(__imp__sub_001FC220);
X86_FUNC(sub_001FC220) {
    using namespace game_loads;
    uint32_t resource = ctx.ecx;
    uint8_t* word = mem::Spans(resource, 0x10) ? static_cast<uint8_t*>(kernel::HostPtr(resource + kState)) : nullptr;
    uint8_t before = word ? *word & 3 : kLoaded;
    __imp__sub_001FC220(ctx, base);
    if (!word || before != kLoading || !mem::Spans(resource, 0x10)) return;
    // Tests: S2_LOAD_FAULT=<n> loses the mark of every n-th load on purpose (the state goes back to what it was, as
    // the other thread's late write would leave it). With S2_NO_LOAD_REPAIR=1 that must give the hang as it was
    // seen; without, a "put back" line and no hang.
    static const int fault = std::getenv("S2_LOAD_FAULT") ? std::atoi(std::getenv("S2_LOAD_FAULT")) : 0;
    static std::atomic<int> count{0};
    if (fault > 0 && ++count % fault == 0) {
        *word = uint8_t((*word & ~3) | before);
        LOGR("loads: test: the loaded mark of resource %08X is taken away again (state %u)", resource, unsigned(before));
    }
    if (g_off || (*word & 3) != kLoading) return;
    SetLoadedAgain(word);
    LOGR("loads: resource %08X read as loading again right after it was marked loaded: the mark was lost between two threads and is put back",
         resource);
}
