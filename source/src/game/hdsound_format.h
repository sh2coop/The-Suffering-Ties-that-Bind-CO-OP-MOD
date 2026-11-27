// HD sound: the file format of one enhanced wave, shared by its maker (tools\launcher\hdsound.h, C) and the game
// (src\game\hdsound.cpp, C++). Plain C, no allocation in the reader.
//
// A wave file (Game Files\sound\hd\<key>.hsw, key = 16 hex digits of HsKey over the wave's ADPCM bytes):
//   HsHeader
//   u32 block_check[blocks]       HsCheck of each 36 * channels byte ADPCM block of the console wave, in order: the
//                                 game tells a region of guest memory is this wave (and which block) by them
//   u32 chunk_at[chunks + 1]      byte offsets of the chunks from the start of the file, the last is the file's size
//   chunks                        HS_CHUNK frames each (the last may be shorter), every channel coded on its own
// The enhanced wave runs at rate * mult frames a second; its frame j * mult is the console wave's frame j (the
// console plays 64 frames of each block: the header's sample and the first 63 decoded ones).
//
// A chunk of one channel: 5 bits kind: 0..3 a fixed polynomial predictor of that order (as in FLAC); 4 and up a linear
// predictor of kind - 3 taps, then 4 bits (coefficient bits - 1), 5 bits shift and the coefficients (two's complement),
// prediction = (sum q[j] * s[i - 1 - j]) >> shift. Then the residuals in partitions
// of HS_PART samples, each a 5-bit Rice parameter (31: raw residuals of 20 bits, offset by 2^19) and its codes.
// Samples are 16-bit. The first `order` (taps) samples of a chunk are coded raw (16 bits, offset by 32768). The bit stream is MSB first; each channel's stream
// starts on a byte.
//
// The index (Game Files\sound\hd\index.bin): "S2HI", u32 version, u32 count, then count HsIndexEntry: the game
// looks a region's first block up there, opens the wave's file, and checks its blocks against block_check.
#pragma once
#include <stdint.h>
#include <string.h>

#define HS_MAGIC 0x53483253u        // "S2HS"
#define HS_INDEX_MAGIC 0x49483253u  // "S2HI"
#define HS_VERSION 1                // a wave made by an older version is made again
#define HS_CHUNK 4096
#define HS_PART 256

typedef struct {
    uint32_t magic, version;
    uint32_t channels, rate, mult;  // console rate; the HD wave plays rate * mult
    uint32_t frames;                // HD frames per channel
    uint32_t blocks;                // console ADPCM blocks
    uint32_t chunks;
    uint32_t flags;                 // 1 loops (made as a circle), 2 treble restored, 4 noise taken out
    uint32_t reserved[3];
} HsHeader;

typedef struct {
    uint64_t first;   // HsKey of the wave's first block
    uint64_t key;     // HsKey of the whole wave (its file name)
    uint32_t blocks, channels, rate, reserved;
} HsIndexEntry;

// FNV-1a 64 over bytes (the same in the maker and the game)
static inline uint64_t HsKey(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
// a short check of one block
static inline uint32_t HsCheck(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// ---- reader ---------------------------------------------------------------------------------------------------------
typedef struct {
    const uint8_t* p;
    size_t n, at;  // at in bits
} HsBits;
static inline uint32_t HsGet(HsBits* b, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        size_t byte = b->at >> 3;
        uint32_t bit = byte < b->n ? (b->p[byte] >> (7 - (b->at & 7))) & 1u : 0u;
        v = (v << 1) | bit;
        b->at++;
    }
    return v;
}
static inline uint32_t HsUnary(HsBits* b) {
    uint32_t q = 0;
    while (b->at < b->n * 8) {
        size_t byte = b->at >> 3;
        // a whole zero byte at once
        if ((b->at & 7) == 0 && b->p[byte] == 0) {
            q += 8;
            b->at += 8;
            continue;
        }
        uint32_t bit = (b->p[byte] >> (7 - (b->at & 7))) & 1u;
        b->at++;
        if (bit) break;
        q++;
    }
    return q;
}
static inline int32_t HsSigned(uint32_t u) { return (int32_t)(u >> 1) ^ -(int32_t)(u & 1); }

// a channel's predictor: a fixed polynomial (order 0..3, lpc 0) or a linear predictor of `order` taps
typedef struct {
    int order, lpc, shift;
    int32_t q[32];
} HsPredictor;
static inline int32_t HsPredict(const HsPredictor* p, const int16_t* s, uint32_t i) {
    if (p->lpc) {
        int64_t acc = 0;
        for (int j = 0; j < p->order; j++) acc += (int64_t)p->q[j] * s[i - 1 - j];
        return (int32_t)(acc >> p->shift);
    }
    switch (p->order) {
        case 1: return s[i - 1];
        case 2: return 2 * s[i - 1] - s[i - 2];
        case 3: return 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3];
        default: return 0;
    }
}

// one channel of a chunk of n frames from p (size bytes) into out; -> bytes read, 0 when broken
static inline size_t HsDecodeChannel(const uint8_t* p, size_t size, uint32_t n, int16_t* out) {
    HsBits b = {p, size, 0};
    HsPredictor pr = {0};
    uint32_t kind = HsGet(&b, 5);
    if (kind <= 3) {
        pr.order = (int)kind;
    } else {
        pr.lpc = 1;
        pr.order = (int)kind - 3;
        int bits = (int)HsGet(&b, 4) + 1;
        pr.shift = (int)HsGet(&b, 5);
        for (int j = 0; j < pr.order; j++) {
            uint32_t u = HsGet(&b, bits);
            pr.q[j] = (int32_t)(u << (32 - bits)) >> (32 - bits);  // two's complement of `bits` bits
        }
    }
    int order = pr.order;
    for (uint32_t i = 0; i < n && i < (uint32_t)order; i++) out[i] = (int16_t)((int32_t)HsGet(&b, 16) - 32768);
    for (uint32_t start = 0; start < n; start += HS_PART) {
        uint32_t end = start + HS_PART < n ? start + HS_PART : n;
        uint32_t k = HsGet(&b, 5);
        for (uint32_t i = start; i < end; i++) {
            if (i < (uint32_t)order) continue;
            int32_t r;
            if (k == 31) r = (int32_t)HsGet(&b, 20) - 524288;
            else r = HsSigned((HsUnary(&b) << k) | HsGet(&b, (int)k));
            int32_t v = HsPredict(&pr, out, i) + r;
            out[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        if (b.at > size * 8 + 64) return 0;
    }
    return (b.at + 7) >> 3;
}

// a whole chunk (all channels, interleaved into out: n * channels samples)
static inline int HsDecodeChunk(const uint8_t* p, size_t size, uint32_t n, uint32_t channels, int16_t* out, int16_t* scratch) {
    size_t at = 0;
    for (uint32_t c = 0; c < channels; c++) {
        size_t used = HsDecodeChannel(p + at, size - at, n, scratch);
        if (!used) return 0;
        at += used;
        for (uint32_t i = 0; i < n; i++) out[i * channels + c] = scratch[i];
    }
    return 1;
}

// the HD wave's value between frames a and a + 1 (t in 0..1), from frames a - 1 .. a + 2: Catmull-Rom
static inline float HsCubic(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}
