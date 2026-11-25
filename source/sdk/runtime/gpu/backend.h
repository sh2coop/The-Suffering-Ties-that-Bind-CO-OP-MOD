#pragma once
#include <atomic>
#include <vector>
#include <mutex>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    // [local change, The Suffering: Ties That Bind] the target as it is at this point of the frame, for the stream
    // sink (see g_stream_snapshot); in order with the draws
    virtual void StreamSnapshot(uint32_t /*data_va*/) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

// frame streaming: when set, each present hands the previous frame's pixels (BGRA, guest size, rows pitch bytes
// apart) to the sink, read back asynchronously through the late-lock ring; the pointer is only valid in the call
using StreamSink = void (*)(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch);
inline std::atomic<StreamSink> g_stream{nullptr};
// [local change, The Suffering: Ties That Bind] The sink can be given a picture from inside the frame in place of
// the finished one: a title that draws a second view first and its own over it (the second player's view of an
// online game, never shown here) calls SnapshotForStream() between the two. While g_stream_snapshot is set, each
// present hands the sink the newest finished snapshot and the finished frame is not read back.
inline std::atomic<bool> g_stream_snapshot{false};
void SnapshotForStream();  // the render target bound now (xdk/d3d8.cpp); the title's thread

// a picture shown in place of the title's frame (an online co-op joiner sees the host's game): BGRA, rows w * 4
// bytes apart, valid until the next call; null to show the title's own frame
using PresentOverride = const uint8_t* (*)(uint32_t& w, uint32_t& h);
inline std::atomic<PresentOverride> g_present_override{nullptr};

// the finished picture's brightness, 1 as drawn, 0 black: a title fades the screen with it (skipping a cut-scene)
inline std::atomic<float> g_fade{1.0f};

// split screen, set by the title around each half: draws made with the whole screen's viewport offset (the 2D HUD,
// where the title centres each half's world itself) move down dy pixels and clip to the scissor given (game thread)
struct HudSplit {
    bool on = false;
    float dx = 0, dy = 0;        // the HUD's draws, placed in screen pixels by their programs, move by this
    uint32_t scissor[4] = {};    // x0, y0, x1, y1: the pass's half
    float view_dx = 0, view_dy = 0;  // a view of its own (the radar) moved into the pass's half: its draws go by this
                                     // (set with its viewport constants, cleared by the next view's)
};
inline HudSplit g_hud_split;
// [local change, The Suffering: Ties That Bind] A title's 2D (menus, HUD, text, movies) is laid out for a 4:3
// picture. When the frame is shown in another shape the title sets this (game thread): draws of ready-made screen
// positions that belong to that layer are squeezed about the middle of the picture, by sx across (a wider
// picture) or sy down (a narrower one), so the layer keeps its shape. Which draws those are: xdk/d3d8.cpp.
// Two views side by side: while the title draws the layer for one of them it sets half (1 the left view, 2 the
// right). The layer, laid out for the whole 640 x 480 frame, then goes into that half: half as wide, and squeezed
// by half_sx across about the half's middle or by half_sy down so it keeps its shape. A piece squeezed down
// stays with the edge it belongs to: the top third of the layout with the top, the bottom third with the
// bottom, the rest about the middle.
struct Flat2D {
    bool on = false;
    float sx = 1, sy = 1;
    int half = 0;
    float half_sx = 1, half_sy = 1;
};
inline Flat2D g_flat2d;
// the halves side by side (left, right) rather than top and bottom: the divider's direction
inline std::atomic<bool> g_split_vertical{false};

// split screen: the title sets it to 2 each frame it draws the two halves, a present counts it down; while it is above
// 0 the frame gets a dark bar across the middle, where the halves meet
inline std::atomic<int> g_split_divider{0};
// [local change, The Suffering: Ties That Bind] Two views side by side: the second player's health, 0 to 1, shown
// as a bar at the top left of the right view, where the left view has the title's own health bar (below 0: no
// bar). The title sets it each frame it draws the two views; it is shown while the divider is.
inline std::atomic<float> g_second_health{-1.0f};
// ... and the partner's health for each player (decided 2026-11-24: each player sees his partner's health, marked as
// the partner's; Player 2's bar is blue): a thin bar under the player's own. g_first_health is Player 1's, for the
// thin bar in Player 2's view. g_partner_whole: one view fills the frame and it is Player 1's while Player 2 plays
// from another PC: his thin bar goes under the title's own bar of the whole frame.
inline std::atomic<float> g_first_health{-1.0f};
inline std::atomic<bool> g_partner_whole{false};
// The title's own health bar art, for those bars: the part of its HUD texture (256 x 256, DXT5) that holds the bar's
// strips, columns 0 to 99 and rows 208 to 255, decoded when the game uploads that texture. Rows 0 to 47: as the
// title has it (red); rows 48 to 95: the same with red and blue swapped (Player 2's). In it the filled strip is
// rows 3 to 15 and the empty bar rows 32 to 47, both columns 0 to 97. Nothing of it is in the program: it comes from
// the player's own disc image at run time. Until it has been seen the bars are plain rectangles.
constexpr uint32_t kHudArtW = 100, kHudArtH = 48;
struct HudArt {
    std::mutex lock;
    std::vector<uint8_t> bgra;       // kHudArtW x 2 kHudArtH, four bytes a texel
    std::atomic<uint32_t> ready{0};  // counts up each time it is taken anew
};
inline HudArt g_hud_art;
// ... and a small arrow above the partner's head, in the title's orange, drawn over the finished frame so that it
// shows through walls (no name tags). For each view (0: Player 1's, the left half or the whole frame; 1: Player
// 2's, the right half) the point it hangs over, as a share of that view across and down; x below 0: none.
struct PartnerArrow {
    std::atomic<float> x{-1.0f}, y{-1.0f};
};
inline PartnerArrow g_partner_arrow[2];
// where the title's picture is in the window (left, top, width, height in back buffer pixels), for a title that
// draws over it in the ImGui layer (GameInfo::ui)
inline std::atomic<float> g_picture[4] = {};

}  // namespace gpu
