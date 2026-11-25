// HD textures, made on the player's PC from his own disc image (asked for on 2026-11-25: "the game build will make the
// players own textures upscaled as a part of the building process", with an On / Off setting in the game).
//
// Nothing of the game is in this program or comes with the folder. Build Recomp reads every texture file (*.tdu) of the
// image in Game Files\assets, upscales each texture 4x with Real-ESRGAN (fetched once from its authors' GitHub release
// into Game Files\hd_tools, its SHA-256 checked) and writes one DDS per texture into Game Files\textures\hd. A DDS is
// named by the same key the port gives the console texture (gpu::texturedumper::KeyOf: a hash of the format, the size,
// the level 0 bytes and a P8 texture's colours), so the port draws it in place of that texture when "HD textures" is On.
//
// The .tdu files (format found 2026-11-25): "SRSC" and 12 bytes, then records to a table at the end. A record is 48
// bytes: width, height, row bytes, bits per pixel, 4, -1 (u32 each), flags (8 bytes), kind (u32: 2 DXT1, 3 DXT5, 0 P8),
// extra mip levels (u32), 8 bytes; then a P8 record's 16 bytes and 256 colours (B, G, R, A), then the levels. DXT data
// is the console's (blocks in rows); P8 pixels are swizzled.
//
// Fixes from the sample sheets (texture_upscale_sample\README.md):
//   - a texture whose edges continue each other (bricks, floors) is padded with its own opposite edge before
//     upscaling, so the result still tiles; other textures are padded with their edge pixels
//   - colour under transparent pixels of a DXT1 or P8 texture is filled from the nearest solid ones first (no dark
//     fringes on fences); a DXT5 texture's colour is left as it is (its alpha is often a blend mask, not holes: the
//     bloodier stages of Torque's face blend over the face by it, version 1 made him bald)
//   - mip levels go down to the console texture's own smallest level, no further (as the game had them), and an
//     on / off alpha keeps its coverage at every level
//   - alpha goes through the upscaler too (sharp text), but where the plain bicubic alpha is faint the bicubic value
//     is kept (glows, fog cards and HUD bars stay), and an alpha that was only on / off is made on / off again
//   - P8 textures are unswizzled first
// Work is in batches; each batch has a hard time limit, and what is done stays done: closing the window stops after
// the batch, and the next Build Recomp continues.
#include <intrin.h>
#include <math.h>
#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../src/game/third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../src/game/third_party/stb_image_write.h"
#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

#define HD_VERSION 2           // bump when the making changes: textures made by an older version are made again
#define HD_SCALE 4
#define HD_PAD 8               // source pixels of padding on each side
#define HD_BATCH 96            // textures per upscaler run
#define HD_BATCH_LIMIT_MS (20 * 60 * 1000)  // a batch that takes longer is stopped (a hung graphics driver)

static const wchar_t kUpscalerHost[] = L"github.com";
static const wchar_t kUpscalerPath[] = L"/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
static const BYTE kUpscalerSha256[32] = {0xab, 0xc0, 0x28, 0x04, 0xe1, 0x79, 0x82, 0xa3, 0xbe, 0x33, 0x67, 0x5e, 0x4d, 0x47, 0x1e, 0x91,
                                         0xea, 0x37, 0x4e, 0x65, 0xb7, 0x01, 0x67, 0xab, 0xc0, 0x9e, 0x31, 0xac, 0xb4, 0x12, 0x80, 0x2d};


// ---- log ------------------------------------------------------------------------------------------------------------
static void HdLog(const wchar_t* format, ...) {
    wchar_t path[MAX_PATH * 2], text[1024];
    va_list a;
    va_start(a, format);
    _vsnwprintf(text, 1023, format, a);
    va_end(a);
    text[1023] = 0;
    Path(path, MAX_PATH * 2, L"Logs");
    CreateDirectoryW(path, NULL);
    Path(path, MAX_PATH * 2, L"Logs\\hd_textures.log");
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
    fclose(f);
}

// ---- files ----------------------------------------------------------------------------------------------------------
static uint8_t* ReadWhole(const wchar_t* path, DWORD* size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER s;
    GetFileSizeEx(f, &s);
    uint8_t* b = (uint8_t*)malloc((size_t)s.QuadPart + 1);
    DWORD got = 0;
    if (b && !ReadFile(f, b, (DWORD)s.QuadPart, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!b || got != (DWORD)s.QuadPart) { free(b); return NULL; }
    *size = got;
    return b;
}
static int WriteWhole(const wchar_t* path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(f, data, (DWORD)size, &put, NULL) && put == size;
    CloseHandle(f);
    if (!ok) DeleteFileW(path);
    return ok;
}
static void EmptyDir(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH * 2], one[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snwprintf(one, MAX_PATH * 2, L"%ls\\%ls", dir, f.cFileName);
        DeleteFileW(one);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

// ---- the disc: an .iso (XDVDFS) or the disc's files -------------------------------------------------------------------
typedef struct { wchar_t name[260]; uint64_t at; uint32_t size; } HdFile;   // at: byte offset in the image (iso)
typedef struct { HANDLE iso; uint64_t base; HdFile* files; int count, cap; } HdDisc;

static void DiscAdd(HdDisc* d, const wchar_t* name, uint64_t at, uint32_t size) {
    if (d->count == d->cap) { d->cap = d->cap ? d->cap * 2 : 64; d->files = (HdFile*)realloc(d->files, d->cap * sizeof(HdFile)); }
    HdFile* f = &d->files[d->count++];
    wcsncpy(f->name, name, 259); f->name[259] = 0;
    f->at = at; f->size = size;
}
static int IsTdu(const wchar_t* name) { size_t n = wcslen(name); return n > 4 && _wcsicmp(name + n - 4, L".tdu") == 0; }

static int ReadAt(HANDLE h, uint64_t at, void* out, DWORD size) {
    OVERLAPPED o = {0};
    o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
    DWORD got = 0;
    return ReadFile(h, out, size, &got, &o) && got == size;
}
static void WalkIso(HdDisc* d, uint32_t sector, uint32_t size, const wchar_t* prefix, int depth) {
    if (!size || depth > 16) return;
    uint8_t* table = (uint8_t*)malloc(size);
    if (!table || !ReadAt(d->iso, d->base + (uint64_t)sector * 2048, table, size)) { free(table); return; }
    // the directory is a binary tree of entries; a stack walks it
    uint32_t stack[512];
    int top = 0;
    stack[top++] = 0;
    while (top) {
        uint32_t at = stack[--top];
        if (at + 14 > size) continue;
        uint16_t left = *(uint16_t*)(table + at), right = *(uint16_t*)(table + at + 2);
        if (left == 0xFFFF) continue;
        uint32_t start = *(uint32_t*)(table + at + 4), length = *(uint32_t*)(table + at + 8);
        uint8_t attr = table[at + 12], n = table[at + 13];
        if (at + 14 + n > size) continue;
        wchar_t name[260];
        int len = _snwprintf(name, 259, L"%ls", prefix);
        for (int i = 0; i < n && len < 258; i++) name[len++] = (wchar_t)table[at + 14 + i];  // names are Latin-1
        name[len] = 0;
        if (attr & 0x10) {
            wcscat(name, L"/");
            WalkIso(d, start, length, name, depth + 1);
        } else if (IsTdu(name)) {
            DiscAdd(d, name, d->base + (uint64_t)start * 2048, length);
        }
        if (left && top < 511) stack[top++] = left * 4u;
        if (right && top < 511) stack[top++] = right * 4u;
    }
    free(table);
}
static void WalkFolder(HdDisc* d, const wchar_t* dir, int depth) {
    if (depth > 16) return;
    wchar_t pattern[MAX_PATH * 2], one[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.cFileName[0] == L'.') continue;
        _snwprintf(one, MAX_PATH * 2, L"%ls\\%ls", dir, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) WalkFolder(d, one, depth + 1);
        else if (IsTdu(f.cFileName)) DiscAdd(d, one, 0, f.nFileSizeLow);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}
// -> 0 no image
static int OpenDisc(HdDisc* d, const wchar_t* assets) {
    memset(d, 0, sizeof *d);
    d->iso = INVALID_HANDLE_VALUE;
    wchar_t iso[MAX_PATH * 2];
    if (FindWithExt(assets, L".iso", iso, MAX_PATH * 2)) {
        d->iso = CreateFileW(iso, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (d->iso == INVALID_HANDLE_VALUE) return 0;
        static const uint64_t bases[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
        for (int i = 0; i < 4; i++) {
            uint8_t head[32];
            if (!ReadAt(d->iso, bases[i] + 32 * 2048, head, 32) || memcmp(head, "MICROSOFT*XBOX*MEDIA", 20)) continue;
            d->base = bases[i];
            WalkIso(d, *(uint32_t*)(head + 20), *(uint32_t*)(head + 24), L"", 0);
            return 1;
        }
        CloseHandle(d->iso);
        d->iso = INVALID_HANDLE_VALUE;
        return 0;
    }
    WalkFolder(d, assets, 0);
    return d->count > 0;
}
static uint8_t* DiscRead(HdDisc* d, const HdFile* f) {
    if (d->iso == INVALID_HANDLE_VALUE) { DWORD size; return ReadWhole(f->name, &size); }
    uint8_t* b = (uint8_t*)malloc(f->size);
    if (b && !ReadAt(d->iso, f->at, b, f->size)) { free(b); b = NULL; }
    return b;
}

// ---- the port's texture key (sdk\runtime\gpu\shaders.cpp Hash, texturedumper.cpp KeyOf) --------------------------------
static uint64_t Mix(uint64_t a, uint64_t b) { uint64_t hi, lo = _umul128(a, b, &hi); return lo ^ hi; }
static uint64_t HashBytes(const uint8_t* p, size_t n, uint64_t h) {
    const uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    uint64_t seed = h ^ k0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        memcpy(&x, p + i, 8);
        memcpy(&y, p + i + 8, 8);
        seed = Mix(x ^ k1, y ^ seed);
    }
    size_t r = n - i;
    if (r) {
        uint64_t x = 0, y = 0;
        if (r >= 8) { memcpy(&x, p + i, 8); memcpy(&y, p + n - 8, 8); }
        else memcpy(&x, p + i, r);
        seed = Mix(x ^ k1, y ^ seed ^ r);
    }
    return Mix(seed ^ k0, (uint64_t)n ^ k1);
}
static uint64_t TextureKey(uint32_t format, uint32_t w, uint32_t h, const uint8_t* data, size_t size, const uint8_t* palette) {
    uint32_t shape[3] = {format, w, h};
    uint64_t k = HashBytes(data, size, HashBytes((const uint8_t*)shape, sizeof shape, 1469598103934665603ull));
    if (palette) k = HashBytes(palette, 1024, k);
    return k ? k : 1;
}

// ---- the textures -------------------------------------------------------------------------------------------------------
typedef struct {
    uint64_t key;
    uint32_t w, h, kind;   // kind: 2 DXT1, 3 DXT5, 0 P8
    uint32_t mips;         // the console texture's extra levels
    int file;              // HdDisc index
    uint32_t at;           // level 0 (after the P8 colours)
} HdTex;
typedef struct { HdTex* t; int count, cap; } HdList;

static uint32_t LevelBytes(uint32_t kind, uint32_t w, uint32_t h) {
    if (kind == 2) return (w > 4 ? w / 4 : 1) * (h > 4 ? h / 4 : 1) * 8;
    if (kind == 3) return (w > 4 ? w / 4 : 1) * (h > 4 ? h / 4 : 1) * 16;
    return w * h;
}
static int SeenKey(HdList* l, uint64_t key) {
    for (int i = 0; i < l->count; i++) if (l->t[i].key == key) return 1;   // a few thousand: fine once
    return 0;
}
static void ListTextures(HdDisc* d, int file, const uint8_t* b, uint32_t size, HdList* l) {
    if (size < 16 || memcmp(b, "SRSC", 4)) return;
    uint32_t o = 16;
    while (o + 48 <= size) {
        const uint32_t* r = (const uint32_t*)(b + o);
        uint32_t w = r[0], h = r[1], bpp = r[3];
        if ((int32_t)r[5] != -1 || !w || !h || w > 4096 || h > 4096) break;
        uint32_t kind = r[8], mips = r[9], p = o + 48;
        if (kind != 0 && kind != 2 && kind != 3) break;
        const uint8_t* palette = NULL;
        if (kind == 0) { if (bpp != 8) break; palette = b + p + 16; p += 16 + 1024; }
        uint32_t l0 = LevelBytes(kind, w, h), all = 0, lw = w, lh = h;
        for (uint32_t m = 0; m <= mips && m < 14; m++) { all += LevelBytes(kind, lw, lh); lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
        if (p + all > size) break;
        static const uint32_t xbox_format[4] = {0x0B, 0, 0x0C, 0x0F};   // D3DFMT_P8, -, DXT1, DXT5 (the console's numbers)
        uint64_t key = TextureKey(xbox_format[kind], w, h, b + p, l0, palette);
        // the smallest are not worth it (and some are lookup tables)
        if (w >= 8 && h >= 4 && !SeenKey(l, key)) {
            if (l->count == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->t = (HdTex*)realloc(l->t, l->cap * sizeof(HdTex)); }
            HdTex t = {key, w, h, kind, mips, file, p};
            l->t[l->count++] = t;
        }
        o = p + all;
    }
    (void)d;
}

// ---- decoding to RGBA -------------------------------------------------------------------------------------------------
static void Rgb565(uint16_t c, uint8_t* o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31);
}
static void ColourBlock(const uint8_t* s, uint8_t* out, uint32_t pitch, int dxt1) {
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8);
    uint8_t c[4][4];
    Rgb565(c0, c[0]); Rgb565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = c[3][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (int i = 0; i < 3; i++) { c[2][i] = (uint8_t)((2 * c[0][i] + c[1][i]) / 3); c[3][i] = (uint8_t)((c[0][i] + 2 * c[1][i]) / 3); }
    } else {
        for (int i = 0; i < 3; i++) { c[2][i] = (uint8_t)((c[0][i] + c[1][i]) / 2); c[3][i] = 0; }
        c[3][3] = 0;
    }
    uint32_t bits = s[4] | s[5] << 8 | s[6] << 16 | (uint32_t)s[7] << 24;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) memcpy(out + y * pitch + x * 4, c[(bits >> (2 * (y * 4 + x))) & 3], 4);
}
static void AlphaBlock(const uint8_t* s, uint8_t* out, uint32_t pitch) {
    uint8_t a[8];
    a[0] = s[0]; a[1] = s[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) out[y * pitch + x * 4 + 3] = a[(bits >> (3 * (y * 4 + x))) & 7];
}
static uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t r = 0, bit = 1;
    for (uint32_t m = 1; m < w || m < h; m <<= 1) {
        if (m < w) { if (x & m) r |= bit; bit <<= 1; }
        if (m < h) { if (y & m) r |= bit; bit <<= 1; }
    }
    return r;
}
// -> w*h*4 bytes
static uint8_t* Decode(const HdTex* t, const uint8_t* b) {
    uint32_t w = t->w, h = t->h;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    const uint8_t* s = b + t->at;
    if (t->kind == 0) {
        const uint8_t* pal = b + t->at - 1024;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* c = pal + 4 * s[Swizzle(x, y, w, h)];
                uint8_t* o = px + ((size_t)y * w + x) * 4;
                o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = c[3];
            }
        return px;
    }
    uint32_t bw = w / 4 ? w / 4 : 1, bh = h / 4 ? h / 4 : 1, step = t->kind == 2 ? 8 : 16;
    uint8_t block[64];
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, s += step) {
            if (t->kind == 2) ColourBlock(s, block, 16, 1);
            else { ColourBlock(s + 8, block, 16, 0); AlphaBlock(s, block, 16); }
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++) memcpy(px + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 4, block + y * 16 + x * 4, 4);
        }
    return px;
}

// ---- preparing one texture ------------------------------------------------------------------------------------------
enum { ALPHA_NONE, ALPHA_BINARY, ALPHA_SOFT, ALPHA_FAINT };

static int AlphaKind(const uint8_t* px, uint32_t n) {
    int lo = 255, hi = 0, mid = 0;
    for (uint32_t i = 0; i < n; i++) {
        int a = px[i * 4 + 3];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        if (a > 8 && a < 247) mid++;
    }
    if (lo >= 250) return ALPHA_NONE;
    if (hi < 96) return ALPHA_FAINT;       // a glow or fog card: the upscaler drops such alpha, so it is not asked
    if (mid * 50 < (int)n) return ALPHA_BINARY;
    return ALPHA_SOFT;
}
// colour under transparent pixels from the nearest solid ones (no dark fringes once upscaled)
static void Bleed(uint8_t* px, uint32_t w, uint32_t h) {
    uint32_t n = w * h;
    uint8_t* known = (uint8_t*)malloc(n);
    double sum[3] = {0}; uint32_t solid = 0;
    for (uint32_t i = 0; i < n; i++) {
        known[i] = px[i * 4 + 3] >= 128;
        if (known[i]) { for (int c = 0; c < 3; c++) sum[c] += px[i * 4 + c]; solid++; }
    }
    if (!solid || solid == n) { free(known); return; }
    uint8_t* next = (uint8_t*)malloc(n);
    for (int pass = 0; pass < 24; pass++) {
        int changed = 0;
        memcpy(next, known, n);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t i = y * w + x;
                if (known[i]) continue;
                int s[3] = {0}, k = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = (int)x + dx, yy = (int)y + dy;
                        if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h || !known[yy * w + xx]) continue;
                        for (int c = 0; c < 3; c++) s[c] += px[(yy * w + xx) * 4 + c];
                        k++;
                    }
                if (!k) continue;
                for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(s[c] / k);
                next[i] = 1;
                changed = 1;
            }
        memcpy(known, next, n);
        if (!changed) break;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!known[i]) for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(sum[c] / solid);
    free(next);
    free(known);
}
// do the left and right (top and bottom) edges continue each other, as in a texture made to repeat?
static int Tiles(const uint8_t* px, uint32_t w, uint32_t h, int vertical) {
    uint32_t len = vertical ? w : h, across = vertical ? h : w;
    if (across < 8) return 0;
    // the step across the edge against the mean step between neighbours inside
    double edge = 0, inner = 0;
    uint32_t inner_n = 0;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t* a = vertical ? px + (size_t)i * 4 : px + (size_t)i * w * 4;
        const uint8_t* b = vertical ? px + ((size_t)(h - 1) * w + i) * 4 : px + ((size_t)i * w + w - 1) * 4;
        for (int c = 0; c < 3; c++) edge += abs(a[c] - b[c]);
        for (uint32_t k = 1; k < across; k++) {
            const uint8_t* p = vertical ? px + ((size_t)(k - 1) * w + i) * 4 : px + ((size_t)i * w + k - 1) * 4;
            const uint8_t* q = vertical ? px + ((size_t)k * w + i) * 4 : px + ((size_t)i * w + k) * 4;
            for (int c = 0; c < 3; c++) inner += abs(p[c] - q[c]);
            inner_n++;
        }
    }
    return edge / len <= 1.6 * inner / inner_n + 6.0;
}
// pad (wrap or edge) -> (w + 2px) x (h + 2py)
static uint8_t* Pad(const uint8_t* px, uint32_t w, uint32_t h, uint32_t pxs, uint32_t pys, int wrap_x, int wrap_y) {
    uint32_t W = w + 2 * pxs, H = h + 2 * pys;
    uint8_t* o = (uint8_t*)malloc((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            int sx = (int)x - (int)pxs, sy = (int)y - (int)pys;
            if (wrap_x) sx = (sx % (int)w + (int)w) % (int)w; else sx = sx < 0 ? 0 : sx >= (int)w ? (int)w - 1 : sx;
            if (wrap_y) sy = (sy % (int)h + (int)h) % (int)h; else sy = sy < 0 ? 0 : sy >= (int)h ? (int)h - 1 : sy;
            memcpy(o + ((size_t)y * W + x) * 4, px + ((size_t)sy * w + sx) * 4, 4);
        }
    return o;
}

typedef struct { uint8_t* data; size_t size, cap; } MemOut;
static void ToMem(void* ctx, void* data, int size) {
    MemOut* m = (MemOut*)ctx;
    if (m->size + size > m->cap) { m->cap = (m->size + size) * 2; m->data = (uint8_t*)realloc(m->data, m->cap); }
    memcpy(m->data + m->size, data, size);
    m->size += size;
}
static int WritePng(const wchar_t* path, const uint8_t* px, uint32_t w, uint32_t h, int comp) {
    MemOut m = {0};
    stbi_write_png_compression_level = 1;
    int ok = stbi_write_png_to_func(ToMem, &m, (int)w, (int)h, comp, px, (int)w * comp) && WriteWhole(path, m.data, m.size);
    free(m.data);
    return ok;
}

// ---- finishing one texture --------------------------------------------------------------------------------------------
typedef struct {
    HdTex t;
    int alpha, wrap_x, wrap_y;
    uint32_t pad_x, pad_y;
    uint8_t* source;       // w*h*4, after Bleed: the bicubic alpha's source
    wchar_t in_rgb[MAX_PATH], in_a[MAX_PATH], out_rgb[MAX_PATH], out_a[MAX_PATH];
    int done;              // 1 written, -1 failed
} HdJob;

static float Cubic(float t, float a, float b, float c, float d) {   // Catmull-Rom
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}
static uint8_t BicubicAlpha(const HdJob* j, uint32_t X, uint32_t Y) {
    float fx = (X + 0.5f) / HD_SCALE - 0.5f, fy = (Y + 0.5f) / HD_SCALE - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - ix, ty = fy - iy, row[4];
    int w = (int)j->t.w, h = (int)j->t.h;
    for (int r = 0; r < 4; r++) {
        int y = iy - 1 + r;
        y = j->wrap_y ? (y % h + h) % h : y < 0 ? 0 : y >= h ? h - 1 : y;
        float v[4];
        for (int c = 0; c < 4; c++) {
            int x = ix - 1 + c;
            x = j->wrap_x ? (x % w + w) % w : x < 0 ? 0 : x >= w ? w - 1 : x;
            v[c] = j->source[((size_t)y * w + x) * 4 + 3];
        }
        row[r] = Cubic(tx, v[0], v[1], v[2], v[3]);
    }
    float a = Cubic(ty, row[0], row[1], row[2], row[3]);
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5f);
}

// one mip level into BC1 / BC3 blocks
static void Compress(const uint8_t* px, uint32_t w, uint32_t h, int bc3, MemOut* out) {
    uint8_t block[64], dest[16];
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1, sy = by * 4 + y < h ? by * 4 + y : h - 1;
                    memcpy(block + (y * 4 + x) * 4, px + ((size_t)sy * w + sx) * 4, 4);
                }
            stb_compress_dxt_block(dest, block, bc3, STB_DXT_HIGHQUAL);
            ToMem(out, dest, bc3 ? 16 : 8);
        }
}
static void Put32(uint8_t* b, int at, uint32_t v) { memcpy(b + at, &v, 4); }

static int FinishJob(HdJob* j, const wchar_t* hd_dir) {
    DWORD size = 0;
    uint8_t* file = ReadWhole(j->out_rgb, &size);
    int W = 0, H = 0, n = 0;
    uint8_t* rgb = file ? stbi_load_from_memory(file, (int)size, &W, &H, &n, 4) : NULL;
    free(file);
    uint32_t PW = (j->t.w + 2 * j->pad_x) * HD_SCALE, PH = (j->t.h + 2 * j->pad_y) * HD_SCALE;
    if (!rgb || (uint32_t)W != PW || (uint32_t)H != PH) { if (rgb) stbi_image_free(rgb); return 0; }
    uint8_t* ai_a = NULL;
    if (j->alpha == ALPHA_BINARY || j->alpha == ALPHA_SOFT) {
        file = ReadWhole(j->out_a, &size);
        int aw = 0, ah = 0;
        ai_a = file ? stbi_load_from_memory(file, (int)size, &aw, &ah, &n, 1) : NULL;
        free(file);
        if (ai_a && ((uint32_t)aw != PW || (uint32_t)ah != PH)) { stbi_image_free(ai_a); ai_a = NULL; }
    }
    uint32_t w = j->t.w * HD_SCALE, h = j->t.h * HD_SCALE, ox = j->pad_x * HD_SCALE, oy = j->pad_y * HD_SCALE;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* o = px + ((size_t)y * w + x) * 4;
            const uint8_t* s = rgb + ((size_t)(y + oy) * PW + x + ox) * 4;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            if (j->alpha == ALPHA_NONE) { o[3] = 255; continue; }
            uint8_t bic = BicubicAlpha(j, x, y);
            if (j->alpha == ALPHA_FAINT || !ai_a) { o[3] = bic; continue; }
            float ai = ai_a[(size_t)(y + oy) * PW + x + ox];
            if (j->alpha == ALPHA_BINARY) { o[3] = ai >= 128 ? 255 : 0; continue; }
            float t = (bic - 32) / 64.0f;   // faint: bicubic; solid: the upscaler's (sharp text and edges)
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float a = bic + t * (ai - bic);
            o[3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5f);
        }
    stbi_image_free(rgb);
    if (ai_a) stbi_image_free(ai_a);

    // DDS: BC1 when solid, else BC3, every level (the port's ReadDds)
    int bc3 = j->alpha != ALPHA_NONE;
    uint32_t levels = 0;
    for (uint32_t s = w > h ? w : h; s; s >>= 1) levels++;
    uint32_t own = 3 + j->t.mips;   // 4x: 3 levels down to the console size, then the console's own extra ones
    if (levels > own) levels = own;
    uint32_t covered = 0;           // an on / off alpha: how much of level 0 is solid
    if (j->alpha == ALPHA_BINARY)
        for (size_t i = 0; i < (size_t)w * h; i++) covered += px[i * 4 + 3] >= 128;
    double coverage = (double)covered / ((double)w * h);
    MemOut out = {0};
    uint8_t head[128] = {0};
    memcpy(head, "DDS ", 4);
    Put32(head, 4, 124);
    Put32(head, 8, 0x1007 | 0x20000 | 0x80000);
    Put32(head, 12, h); Put32(head, 16, w);
    Put32(head, 20, (w / 4) * (h / 4) * (bc3 ? 16 : 8));
    Put32(head, 28, levels);
    Put32(head, 76, 32); Put32(head, 80, 0x4);
    memcpy(head + 84, bc3 ? "DXT5" : "DXT1", 4);
    Put32(head, 108, 0x1000 | 0x8 | 0x400000);
    ToMem(&out, head, 128);
    uint32_t lw = w, lh = h;
    uint8_t* level = px;
    for (uint32_t l = 0; l < levels; l++) {
        Compress(level, lw, lh, bc3, &out);
        if (l + 1 == levels) break;
        uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        uint8_t* next = (uint8_t*)malloc((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    uint32_t x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : lw - 1;
                    uint32_t y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : lh - 1;
                    int s = level[((size_t)y0 * lw + x0) * 4 + c] + level[((size_t)y0 * lw + x1) * 4 + c] +
                            level[((size_t)y1 * lw + x0) * 4 + c] + level[((size_t)y1 * lw + x1) * 4 + c];
                    next[((size_t)y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                }
        if (level != px) free(level);
        level = next; lw = nw; lh = nh;
        if (j->alpha == ALPHA_BINARY) {  // on / off again, as much of it solid as at level 0
            uint32_t hist[256] = {0}, n = lw * lh, want = (uint32_t)(coverage * n + 0.5), sum = 0;
            for (uint32_t i = 0; i < n; i++) hist[level[i * 4 + 3]]++;
            int cut = 256;
            while (cut > 0 && sum + hist[cut - 1] <= want) sum += hist[--cut];
            if (cut == 256 && want) cut = 255;
            for (uint32_t i = 0; i < n; i++) level[i * 4 + 3] = level[i * 4 + 3] >= cut ? 255 : 0;
        }
    }
    if (level != px) free(level);
    free(px);
    wchar_t name[MAX_PATH * 2], tmp[MAX_PATH * 2];
    _snwprintf(name, MAX_PATH * 2, L"%ls\\%ux%u_%016llx_HD.dds", hd_dir, w, h, (unsigned long long)j->t.key);
    _snwprintf(tmp, MAX_PATH * 2, L"%ls.part", name);
    int ok = WriteWhole(tmp, out.data, out.size) && MoveFileExW(tmp, name, MOVEFILE_REPLACE_EXISTING);
    free(out.data);
    return ok;
}

// the finishing of a batch on all processors but one, beside the next batch's upscaling
typedef struct { HdJob* jobs; int count; volatile LONG next, ok, failed; const wchar_t* hd_dir; HANDLE threads[64]; int nthreads; } HdPool;
static DWORD WINAPI PoolWork(LPVOID p) {
    HdPool* pool = (HdPool*)p;
    for (;;) {
        LONG i = InterlockedIncrement(&pool->next) - 1;
        if (i >= pool->count) return 0;
        HdJob* j = &pool->jobs[i];
        if (FinishJob(j, pool->hd_dir)) InterlockedIncrement(&pool->ok); else InterlockedIncrement(&pool->failed);
        DeleteFileW(j->in_rgb); DeleteFileW(j->out_rgb);
        if (j->in_a[0]) { DeleteFileW(j->in_a); DeleteFileW(j->out_a); }
        free(j->source); j->source = NULL;
    }
}
static void PoolStart(HdPool* pool, HdJob* jobs, int count, const wchar_t* hd_dir) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1;
    n = n < 1 ? 1 : n > 64 ? 64 : n;
    pool->jobs = jobs; pool->count = count; pool->next = 0; pool->ok = 0; pool->failed = 0; pool->hd_dir = hd_dir; pool->nthreads = n;
    for (int i = 0; i < n; i++) pool->threads[i] = CreateThread(NULL, 0, PoolWork, pool, 0, NULL);
}
static void PoolWait(HdPool* pool) {
    for (int i = 0; i < pool->nthreads; i++) {
        while (WaitForSingleObject(pool->threads[i], 100) == WAIT_TIMEOUT) ShowLine(g_line);
        CloseHandle(pool->threads[i]);
    }
    pool->nthreads = 0;
}

// ---- the upscaler --------------------------------------------------------------------------------------------------------
static int DownloadFrom(const wchar_t* host, const wchar_t* path, const wchar_t* to, const wchar_t* what) {
    int result = -1;
    HINTERNET session = WinHttpOpen(L"Suffering2-Build", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (session) WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL);
        if (status != 200) result = (int)status;
        else {
            HANDLE file = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                static char buffer[1 << 16];
                DWORD got = 0, written = 0, total = 0, last = 0;
                result = 0;
                while (WinHttpReadData(request, buffer, sizeof buffer, &got) && got) {
                    if (!WriteFile(file, buffer, got, &written, NULL) || written != got) { result = -1; break; }
                    total += got;
                    if (GetTickCount() - last > 200) {
                        wchar_t text[300];
                        _snwprintf(text, 300, L"%ls\n\n%.1f MB", what, total / 1048576.0);
                        ShowLine(text);
                        last = GetTickCount();
                    }
                }
                CloseHandle(file);
                if (result) DeleteFileW(to);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return result;
}
// Game Files\hd_tools\realesrgan-ncnn-vulkan.exe, fetched and checked once -> 0 when it cannot be had
static int GetUpscaler(const wchar_t* tools, wchar_t* exe) {
    _snwprintf(exe, MAX_PATH * 2, L"%ls\\realesrgan-ncnn-vulkan.exe", tools);
    wchar_t model[MAX_PATH * 2];
    _snwprintf(model, MAX_PATH * 2, L"%ls\\models\\realesrgan-x4plus.bin", tools);
    if (Exists(exe) && Exists(model)) return 1;
    CreateDirectoryW(tools, NULL);
    wchar_t zip[MAX_PATH * 2];
    _snwprintf(zip, MAX_PATH * 2, L"%ls\\upscaler.zip", tools);
    int got = DownloadFrom(kUpscalerHost, kUpscalerPath, zip, L"Fetching the upscaler (Real-ESRGAN, 45 MB, once)");
    BYTE hash[32];
    if (got != 0 || !HashFile(zip, hash) || memcmp(hash, kUpscalerSha256, 32) != 0) {
        HdLog(L"the upscaler could not be fetched (result %d, or its checksum did not match)", got);
        DeleteFileW(zip);
        return 0;
    }
    wchar_t command[MAX_PATH * 6];
    _snwprintf(command, MAX_PATH * 6, L"\"%ls\\System32\\tar.exe\" -xf \"%ls\" -C \"%ls\"", _wgetenv(L"SystemRoot") ? _wgetenv(L"SystemRoot") : L"C:\\Windows", zip, tools);
    int ok = RunQuiet(command) == 0 && Exists(exe) && Exists(model);
    DeleteFileW(zip);
    static const wchar_t* const extras[] = {L"onepiece_demo.mp4", L"input.jpg", L"input2.jpg", L"vcomp140d.dll"};
    for (int i = 0; i < 4; i++) { wchar_t p[MAX_PATH * 2]; _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", tools, extras[i]); DeleteFileW(p); }
    if (!ok) HdLog(L"the upscaler could not be unpacked");
    return ok;
}

// ---- making the set ------------------------------------------------------------------------------------------------------
static int HdMarker(wchar_t* out) { Path(out, MAX_PATH * 2, L"Game Files\\textures\\hd\\made.txt"); return 0; }
// the set is whole (made by this version)?
static int HdDone(void) {
    wchar_t marker[MAX_PATH * 2];
    HdMarker(marker);
    char text[64];
    return ReadSmall(marker, text, sizeof text) && atoi(text) == HD_VERSION;
}
static int HdExists(const wchar_t* hd_dir, const HdTex* t) {
    wchar_t name[MAX_PATH * 2];
    _snwprintf(name, MAX_PATH * 2, L"%ls\\%ux%u_%016llx_HD.dds", hd_dir, t->w * HD_SCALE, t->h * HD_SCALE, (unsigned long long)t->key);
    return Exists(name);
}

// runs the upscaler over one folder; -> its exit code, -2 stopped by the time limit
static int RunUpscaler(const wchar_t* exe, const wchar_t* tools, const wchar_t* in, const wchar_t* out) {
    wchar_t command[MAX_PATH * 8];
    _snwprintf(command, MAX_PATH * 8, L"\"%ls\" -i \"%ls\" -o \"%ls\" -n realesrgan-x4plus -s 4 -f png -m \"%ls\\models\"", exe, in, out, tools);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, tools, &si, &pi)) return -1;
    DWORD start = GetTickCount();
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        ShowLine(g_line);
        if (GetTickCount() - start > HD_BATCH_LIMIT_MS) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return -2;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// -> 1 the set is whole, 0 stopped or failed (said why)
static int MakeHdTextures(void) {
    wchar_t assets[MAX_PATH * 2], tools[MAX_PATH * 2], hd_dir[MAX_PATH * 2], work[MAX_PATH * 2], exe[MAX_PATH * 2], marker[MAX_PATH * 2];
    Path(assets, MAX_PATH * 2, L"Game Files\\assets");
    Path(tools, MAX_PATH * 2, L"Game Files\\hd_tools");
    Path(hd_dir, MAX_PATH * 2, L"Game Files\\textures");
    CreateDirectoryW(hd_dir, NULL);
    Path(hd_dir, MAX_PATH * 2, L"Game Files\\textures\\hd");
    CreateDirectoryW(hd_dir, NULL);
    Path(work, MAX_PATH * 2, L"Game Files\\textures\\hd_work");
    CreateDirectoryW(work, NULL);
    HdMarker(marker);
    g_hd_stop = 0;
    g_hd_running = 1;
    HdLog(L"making HD textures (version %d)", HD_VERSION);
    {
        wchar_t version_file[MAX_PATH * 2];
        char text[32] = {0};
        _snwprintf(version_file, MAX_PATH * 2, L"%ls\\version.txt", hd_dir);
        if (!ReadSmall(version_file, text, sizeof text) || atoi(text) != HD_VERSION) {
            EmptyDir(hd_dir);   // made by another version (or none): every texture is made again
            _snprintf(text, sizeof text, "%d\n", HD_VERSION);
            WriteWhole(version_file, text, strlen(text));
        }
    }

    ULARGE_INTEGER free_bytes;
    if (GetDiskFreeSpaceExW(hd_dir, &free_bytes, NULL, NULL) && free_bytes.QuadPart < 3ull << 30) {
        Tell(L"The HD textures need about 3 GB free on this drive, and it has less.\n\nThe game works without them. Free some room, then double-click \"Build Recomp\" again to make them.", MB_ICONINFORMATION);
        g_hd_running = 0;
        return 0;
    }
    ShowLine(L"Making HD textures\n\nReading the textures of your game image");
    HdDisc disc;
    if (!OpenDisc(&disc, assets)) {
        Tell(L"The HD textures could not be made: no game image was found in Game Files\\assets.", MB_ICONERROR);
        g_hd_running = 0;
        return 0;
    }
    HdList list = {0};
    const wchar_t* only = _wgetenv(L"S2_HD_ONLY");   // tests: only the texture files whose names hold this
    for (int i = 0; i < disc.count; i++) {
        if (only && !wcsstr(disc.files[i].name, only)) continue;
        uint8_t* b = DiscRead(&disc, &disc.files[i]);
        if (b) ListTextures(&disc, i, b, disc.files[i].size, &list);
        free(b);
    }
    // what is left to make, file by file (each file is read once)
    int todo = 0;
    for (int i = 0; i < list.count; i++) if (!HdExists(hd_dir, &list.t[i])) todo++;
    HdLog(L"%d texture files, %d textures, %d still to make", disc.count, list.count, todo);
    if (!list.count) {
        Tell(L"The HD textures could not be made: no textures were found in the game image.", MB_ICONERROR);
        g_hd_running = 0;
        return 0;
    }
    if (todo && !GetUpscaler(tools, exe)) {
        Tell(L"The upscaler could not be fetched (it comes from the Real-ESRGAN project's page on GitHub).\n\nCheck the internet connection, then double-click \"Build Recomp\" again. The game works without HD textures.", MB_ICONINFORMATION);
        g_hd_running = 0;
        return 0;
    }

    HdJob* jobs[2] = {(HdJob*)calloc(HD_BATCH, sizeof(HdJob)), (HdJob*)calloc(HD_BATCH, sizeof(HdJob))};
    int counts[2] = {0, 0};
    HdPool pool = {0};
    int pool_side = -1, made = list.count - todo, failed = 0, result = 1, side = 0;
    DWORD started = GetTickCount();
    int started_made = made;
    int cursor = 0;
    uint8_t* file_data = NULL;
    int file_index = -1;
    while (cursor < list.count && !g_hd_stop) {
        // the next batch's inputs
        wchar_t in[MAX_PATH * 2], out[MAX_PATH * 2];
        _snwprintf(in, MAX_PATH * 2, L"%ls\\in%d", work, side);
        _snwprintf(out, MAX_PATH * 2, L"%ls\\out%d", work, side);
        CreateDirectoryW(in, NULL); CreateDirectoryW(out, NULL);
        EmptyDir(in); EmptyDir(out);
        HdJob* batch = jobs[side];
        int n = 0;
        for (; cursor < list.count && n < HD_BATCH; cursor++) {
            HdTex* t = &list.t[cursor];
            if (HdExists(hd_dir, t)) continue;
            if (t->file != file_index) {
                free(file_data);
                file_index = t->file;
                file_data = DiscRead(&disc, &disc.files[t->file]);
            }
            if (!file_data) { failed++; continue; }
            HdJob* j = &batch[n];
            memset(j, 0, sizeof *j);
            j->t = *t;
            uint8_t* px = Decode(t, file_data);
            j->alpha = AlphaKind(px, t->w * t->h);
            if (j->alpha != ALPHA_NONE && t->kind != 3) Bleed(px, t->w, t->h);
            j->wrap_x = Tiles(px, t->w, t->h, 0);
            j->wrap_y = Tiles(px, t->w, t->h, 1);
            j->pad_x = t->w < HD_PAD ? t->w : HD_PAD;
            j->pad_y = t->h < HD_PAD ? t->h : HD_PAD;
            uint8_t* padded = Pad(px, t->w, t->h, j->pad_x, j->pad_y, j->wrap_x, j->wrap_y);
            uint32_t PW = t->w + 2 * j->pad_x, PH = t->h + 2 * j->pad_y;
            _snwprintf(j->in_rgb, MAX_PATH, L"%ls\\%016llx.png", in, (unsigned long long)t->key);
            _snwprintf(j->out_rgb, MAX_PATH, L"%ls\\%016llx.png", out, (unsigned long long)t->key);
            uint8_t* rgb = (uint8_t*)malloc((size_t)PW * PH * 3);
            for (uint32_t i = 0; i < PW * PH; i++) memcpy(rgb + i * 3, padded + i * 4, 3);
            int ok = WritePng(j->in_rgb, rgb, PW, PH, 3);
            if (ok && (j->alpha == ALPHA_BINARY || j->alpha == ALPHA_SOFT)) {
                _snwprintf(j->in_a, MAX_PATH, L"%ls\\%016llx_a.png", in, (unsigned long long)t->key);
                _snwprintf(j->out_a, MAX_PATH, L"%ls\\%016llx_a.png", out, (unsigned long long)t->key);
                for (uint32_t i = 0; i < PW * PH; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = padded[i * 4 + 3];
                ok = WritePng(j->in_a, rgb, PW, PH, 3);
            }
            free(rgb); free(padded);
            if (!ok) { free(px); failed++; continue; }
            j->source = px;
            n++;
        }
        counts[side] = n;
        if (!n) break;
        // upscale this batch while the last one is finished
        int code = RunUpscaler(exe, tools, in, out);
        if (pool_side >= 0) { PoolWait(&pool); made += pool.ok; failed += pool.failed; pool_side = -1; }
        if (code != 0) {
            HdLog(L"the upscaler stopped with %d on a batch of %d", code, n);
            for (int i = 0; i < n; i++) { free(batch[i].source); batch[i].source = NULL; }
            result = 0;
            Tell(code == -2 ? L"Making the HD textures took far too long on one step and was stopped.\n\nWhat was made so far is kept: double-click \"Build Recomp\" again to continue. The game works without HD textures (Logs\\hd_textures.log says more)."
                            : L"The upscaler could not run on this PC's graphics (it needs Vulkan, which current NVIDIA, AMD and Intel drivers have).\n\nUpdating the graphics driver may help; then double-click \"Build Recomp\" again. The game works without HD textures (Logs\\hd_textures.log says more).",
                 MB_ICONINFORMATION);
            break;
        }
        PoolStart(&pool, batch, n, hd_dir);
        pool_side = side;
        side ^= 1;
        // how far, and how long still
        double seconds = (GetTickCount() - started) / 1000.0;
        int since = made + pool.count - started_made, left = list.count - made - pool.count;
        wchar_t text[400];
        if (since > 0 && seconds > 5) {
            int minutes = (int)(seconds / since * (left > 0 ? left : 0) / 60.0 + 0.999);
            _snwprintf(text, 400, L"Making HD textures\n\n%d of %d done, about %d minute%ls left\n\nClosing this window stops after this step; the next \"Build Recomp\" continues.",
                       made + pool.count, list.count, minutes, minutes == 1 ? L"" : L"s");
        } else {
            _snwprintf(text, 400, L"Making HD textures\n\n%d of %d done\n\nClosing this window stops after this step; the next \"Build Recomp\" continues.", made + pool.count, list.count);
        }
        ShowLine(text);
    }
    if (pool_side >= 0) { PoolWait(&pool); made += pool.ok; failed += pool.failed; }
    free(file_data);
    free(jobs[0]); free(jobs[1]);
    EmptyDir(work);
    for (int s = 0; s < 2; s++) {
        wchar_t d[MAX_PATH * 2];
        _snwprintf(d, MAX_PATH * 2, L"%ls\\in%d", work, s); EmptyDir(d); RemoveDirectoryW(d);
        _snwprintf(d, MAX_PATH * 2, L"%ls\\out%d", work, s); EmptyDir(d); RemoveDirectoryW(d);
    }
    RemoveDirectoryW(work);
    int whole = result && !g_hd_stop && cursor >= list.count && !only;
    HdLog(L"%d of %d made, %d failed, %ls, %.0f s", made, list.count, failed, whole ? L"whole" : L"stopped", (GetTickCount() - started) / 1000.0);
    if (whole) {
        char text[64];
        _snprintf(text, 64, "%d %d %d\n", HD_VERSION, made, failed);
        WriteWhole(marker, text, strlen(text));
    }
    free(list.t);
    free(disc.files);
    if (disc.iso != INVALID_HANDLE_VALUE) CloseHandle(disc.iso);
    g_hd_running = 0;
    return whole;
}
