// HD textures, made on the player's PC from his own disc image. First asked for on 2026-11-25 as part of the build
// (version 32, taken out again: "it takes too long"), asked again on 2026-11-27: "create an hd texture pack option that
// can be enabled in the settings", "make sure u can build it when building the game since we can't distribute them",
// one set "by resolution" and "a 4k one", the HUD, the main menu, the intro, overlays and cutscenes upscaled too.
//
// Nothing of the game is in this program or comes with the folder. "Build Recomp.exe --hd" (started by Build Recomp
// beside the game's own build, and by the game when HD textures is on) reads every texture file (*.tdu) of the image in
// Game Files\assets and upscales each texture 4x with Real-ESRGAN's compact network, run by this program on the
// graphics card (hdgpu.h: Direct3D 11 compute shaders, its weights inside this program). Each texture is written twice,
// in Game Files\textures\hd: x4\ (the 4x picture, for outputs over 1080 rows: 1440p, 4K) and x2\ (the same one level
// down, for 1080p and less), as one DDS named by the key the port gives the console texture
// (gpu::texturedumper::KeyOf: a hash of the format, the size, the level 0 bytes and a P8 texture's colours), so the
// port draws it in place of that texture. The port writes each texture it shows that the set has not got into
// textures\hd\seen (a PNG): those come first, and the ones that are not in the texture files at all (HUD, menus and
// other pictures from other files) are made from those PNGs while the game runs.
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
// What is done stays done: a stopped maker continues where it stopped the next time.
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <tlhelp32.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../src/game/third_party/stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

#define HD_VERSION 5           // bump when the making changes: textures made by an older version are made again
#define HD_PAD 8               // source pixels of padding on each side
static int g_hd_background;    // "--hd": no window and no message boxes (the log only)

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

#include "hdgpu.h"

// ---- finishing one texture --------------------------------------------------------------------------------------------
// One network run gives the 4x picture; the 4x set gets it, the 2x set gets the same picture one level down (both
// sets are made together, so the game can switch between them with the resolution).
typedef struct {
    HdTex t;
    int alpha, wrap_x, wrap_y, seen;
    uint32_t pad_x, pad_y;
    uint8_t* source;       // w*h*4, after Bleed: the bicubic alpha's source
    uint8_t* up_rgb;       // the padded picture at 4x, RGBA
    uint8_t* up_a;         // the padded alpha at 4x (in the red of RGBA), or NULL
} HdJob;

static float Cubic(float t, float a, float b, float c, float d) {   // Catmull-Rom
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}
// channel ch (0 to 3) of the console texture at the 4x pixel (X, Y), bicubic
static uint8_t Bicubic(const HdJob* j, uint32_t X, uint32_t Y, int ch) {
    float fx = (X + 0.5f) / 4 - 0.5f, fy = (Y + 0.5f) / 4 - 0.5f;
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
            v[c] = j->source[((size_t)y * w + x) * 4 + ch];
        }
        row[r] = Cubic(tx, v[0], v[1], v[2], v[3]);
    }
    float a = Cubic(ty, row[0], row[1], row[2], row[3]);
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5f);
}
static uint8_t BicubicAlpha(const HdJob* j, uint32_t X, uint32_t Y) { return Bicubic(j, X, Y, 3); }
// the console texture's alpha at the 4x pixel (X, Y) as the console's own filter reads it (bilinear)
static float BilinearAlpha(const HdJob* j, uint32_t X, uint32_t Y) {
    int w = (int)j->t.w, h = (int)j->t.h;
    float fx = (X + 0.5f) / 4 - 0.5f, fy = (Y + 0.5f) / 4 - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - ix, ty = fy - iy, v[2][2];
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int x = ix + dx, y = iy + dy;
            x = j->wrap_x ? (x % w + w) % w : x < 0 ? 0 : x >= w ? w - 1 : x;
            y = j->wrap_y ? (y % h + h) % h : y < 0 ? 0 : y >= h ? h - 1 : y;
            v[dy][dx] = j->source[((size_t)y * w + x) * 4 + 3];
        }
    return (v[0][0] * (1 - tx) + v[0][1] * tx) * (1 - ty) + (v[1][0] * (1 - tx) + v[1][1] * tx) * ty;
}
// the least and the most alpha of the console texels around the 4x pixel (X, Y)
static void NearAlpha(const HdJob* j, uint32_t X, uint32_t Y, int* least_out, int* most_out) {
    int w = (int)j->t.w, h = (int)j->t.h, most = 0, least = 255;
    float fx = (X + 0.5f) / 4 - 0.5f, fy = (Y + 0.5f) / 4 - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int x = ix + dx, y = iy + dy;
            x = j->wrap_x ? (x % w + w) % w : x < 0 ? 0 : x >= w ? w - 1 : x;
            y = j->wrap_y ? (y % h + h) % h : y < 0 ? 0 : y >= h ? h - 1 : y;
            int a = j->source[((size_t)y * w + x) * 4 + 3];
            if (a > most) most = a;
            if (a < least) least = a;
        }
    *least_out = least;
    *most_out = most;
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
static void DdsHead(MemOut* out, uint32_t w, uint32_t h, uint32_t levels, int bc3) {
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
    ToMem(out, head, 128);
}
static int WriteDds(const wchar_t* dir, uint32_t w, uint32_t h, uint64_t key, const MemOut* out) {
    wchar_t name[MAX_PATH * 2], tmp[MAX_PATH * 2];
    _snwprintf(name, MAX_PATH * 2, L"%ls\\%ux%u_%016llx_HD.dds", dir, w, h, (unsigned long long)key);
    _snwprintf(tmp, MAX_PATH * 2, L"%ls.part", name);
    return WriteWhole(tmp, out->data, out->size) && MoveFileExW(tmp, name, MOVEFILE_REPLACE_EXISTING);
}

static wchar_t g_hd_x2[MAX_PATH * 2], g_hd_x4[MAX_PATH * 2];

// Fix-ups for single textures whose pictures share texels (found by tracing the game's draws, S2_2D_TRACE with the
// vertices). Bump kFixUps when the table changes: the textures in it are made again, nothing else.
//   The menu's button icons (64 x 64, A B X Y, R L, the white square): the A and B buttons are cut from u 21.5 to 42.5
//   texels, so their right edge takes half of column 42, the left edge of the R and L icons' light rings. The game's
//   own filter showed it as a dim line; at 4x it was a crisp white one (the owner: "the a and b look weird still with
//   white strays"). The half of column 42 inside the A and B pictures is cleared (rows 19.5 to 63.5), and likewise the
//   half rows 19 and 41 that the A and B pictures take from the icons above them (a green or white fleck at their foot).
#define HD_FIXUPS 2
static const uint64_t kFixUpKeys[] = {0x7459a981c099d9a1ull};
static void FixUp(uint64_t key, uint8_t* px, uint32_t w, uint32_t h) {
    if (key == 0x7459a981c099d9a1ull && w == 256 && h == 256)
        for (uint32_t y = 78; y < 254; y++)
            for (uint32_t x = 168; x < 170; x++) px[((size_t)y * w + x) * 4 + 3] = 0;
    if (key == 0x7459a981c099d9a1ull && w == 256 && h == 256)   // and the half rows the A and B pictures take from the icon above
        for (uint32_t x = 0; x < 170; x++)
            for (int r = 0; r < 4; r++) px[((size_t)(r < 2 ? 78 + r : 164 + r) * w + x) * 4 + 3] = 0;
}

static int FinishJob(HdJob* j) {
    uint32_t PW = (j->t.w + 2 * j->pad_x) * 4;
    uint32_t w = j->t.w * 4, h = j->t.h * 4, ox = j->pad_x * 4, oy = j->pad_y * 4;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* o = px + ((size_t)y * w + x) * 4;
            const uint8_t* s = j->up_rgb + ((size_t)(y + oy) * PW + x + ox) * 4;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            if (j->alpha == ALPHA_NONE) { o[3] = 255; continue; }
            uint8_t bic = BicubicAlpha(j, x, y);
            float a;
            if (j->alpha == ALPHA_FAINT || !j->up_a) a = bic;
            else {
                float ai = j->up_a[((size_t)(y + oy) * PW + x + ox) * 4];
                if (j->alpha == ALPHA_BINARY) a = ai >= 128 ? 255 : 0;
                else {
                    float t = (bic - 32) / 64.0f;   // faint: bicubic; solid: the upscaler's (sharp text and edges)
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    a = bic + t * (ai - bic);
                }
            }
            // (version 4, 2026-11-27: the owner saw "weird white outlining" round the A and B buttons, and the buttons
            // "circular completely") the alpha stays between the least and the most of the console texels around:
            // the upscaler's alpha rang, out past the shapes and down inside them (between the B button's red and the
            // light ring the console draws round it, 163 became 51: a see-through gap, so the ring stood apart as a
            // white outline). Where it is partly see-through the colour is the console's own, bicubic.
            int least, most;
            NearAlpha(j, x, y, &least, &most);
            if (most < 8) a = 0;
            else if (j->alpha != ALPHA_BINARY) {
                a = a < least ? least : a > most ? most : a;
                // (version 5, the owner again: "still some fraying on the a and b buttons ... the white stuff on the
                // edge") the menu's icons sit two texels apart in their texture: the A button's circle ends, two clear
                // texels, then the white square's edge, which the A button's own picture reaches into. The console's
                // filter smears that edge a whole texel into the clear ones (a faint white line in the original
                // game); the upscaler made it a crisp one. In a clear console texel the alpha now stays clear but
                // for the one 4x pixel on its edge towards a shown texel, which gets no more than the console's filter
                // gives there: a quarter of the smear at most
                float bil = BilinearAlpha(j, x, y);
                a = a > bil + 24 ? bil + 24 : a < bil - 48 ? bil - 48 : a;
                int tw = (int)j->t.w, th = (int)j->t.h, tx = (int)(x / 4), ty = (int)(y / 4), sx = (int)(x % 4), sy = (int)(y % 4);
                if (j->source[((size_t)ty * tw + tx) * 4 + 3] < 8) {
                    int nx = sx == 0 ? tx - 1 : sx == 3 ? tx + 1 : -1, ny = sy == 0 ? ty - 1 : sy == 3 ? ty + 1 : -1, edge = 0;
                    if (nx >= 0 && nx < tw && j->source[((size_t)ty * tw + nx) * 4 + 3] >= 8) edge = 1;
                    if (ny >= 0 && ny < th && j->source[((size_t)ny * tw + tx) * 4 + 3] >= 8) edge = 1;
                    if (nx >= 0 && nx < tw && ny >= 0 && ny < th && j->source[((size_t)ny * tw + nx) * 4 + 3] >= 8) edge = 1;
                    if (!edge) a = 0;
                    else if (a > bil) a = bil;
                }
            }
            o[3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a + 0.5f);
            float k = (a - 96) / 128.0f;
            k = k < 0 ? 0 : k > 1 ? 1 : k;
            if (k < 1)
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)(Bicubic(j, x, y, c) + k * (s[c] - Bicubic(j, x, y, c)) + 0.5f);
        }

    FixUp(j->t.key, px, w, h);

    // the levels: 4x, 2x, the console's own size, then the console's own extra ones (as the game had them); an on /
    // off alpha keeps its coverage at every level. The 4x set's file has them all, the 2x set's from the second on.
    int bc3 = j->alpha != ALPHA_NONE;
    uint32_t levels = 0;
    for (uint32_t s = w > h ? w : h; s; s >>= 1) levels++;
    uint32_t own = 3 + j->t.mips;
    if (levels > own) levels = own;
    uint32_t covered = 0;
    if (j->alpha == ALPHA_BINARY)
        for (size_t i = 0; i < (size_t)w * h; i++) covered += px[i * 4 + 3] >= 128;
    double coverage = (double)covered / ((double)w * h);
    MemOut four = {0}, two = {0};
    DdsHead(&four, w, h, levels, bc3);
    DdsHead(&two, w / 2, h / 2, levels - 1, bc3);
    uint32_t lw = w, lh = h;
    uint8_t* level = px;
    for (uint32_t l = 0; l < levels; l++) {
        if (l == 0) Compress(level, lw, lh, bc3, &four);
        else {
            size_t at = two.size;
            Compress(level, lw, lh, bc3, &two);
            ToMem(&four, two.data + at, (int)(two.size - at));   // the same blocks
        }
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
    // the 2x first: the game's own check for a finished texture (HdExists) looks at the 4x one
    int ok = WriteDds(g_hd_x2, w / 2, h / 2, j->t.key, &two) && WriteDds(g_hd_x4, w, h, j->t.key, &four);
    free(four.data);
    free(two.data);
    return ok;
}

// ---- the finishing threads ---------------------------------------------------------------------------------------------
// The graphics card runs the network one texture after another on the main thread; the compression to BC1 / BC3 (the
// slow part on the processor) is done by threads on all processors but one, from a short queue.
#define HD_QUEUE 16
typedef struct {
    HdJob* q[HD_QUEUE];
    int head, tail, count, closing;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE put, got;
    volatile LONG ok, failed;
    HANDLE threads[64];
    int nthreads;
} HdPool;
static HdPool g_pool;

static void FreeJob(HdJob* j) {
    free(j->source); free(j->up_rgb); free(j->up_a);
    free(j);
}
static DWORD WINAPI PoolWork(LPVOID unused) {
    (void)unused;
    for (;;) {
        EnterCriticalSection(&g_pool.lock);
        while (!g_pool.count && !g_pool.closing) SleepConditionVariableCS(&g_pool.put, &g_pool.lock, INFINITE);
        if (!g_pool.count) { LeaveCriticalSection(&g_pool.lock); return 0; }
        HdJob* j = g_pool.q[g_pool.head];
        g_pool.head = (g_pool.head + 1) % HD_QUEUE;
        g_pool.count--;
        WakeConditionVariable(&g_pool.got);
        LeaveCriticalSection(&g_pool.lock);
        if (FinishJob(j)) InterlockedIncrement(&g_pool.ok); else InterlockedIncrement(&g_pool.failed);
        FreeJob(j);
    }
}
static void PoolStart(void) {
    memset(&g_pool, 0, sizeof g_pool);
    InitializeCriticalSection(&g_pool.lock);
    InitializeConditionVariable(&g_pool.put);
    InitializeConditionVariable(&g_pool.got);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1;
    n = n < 1 ? 1 : n > 64 ? 64 : n;
    g_pool.nthreads = n;
    for (int i = 0; i < n; i++) {
        g_pool.threads[i] = CreateThread(NULL, 0, PoolWork, NULL, 0, NULL);
        SetThreadPriority(g_pool.threads[i], THREAD_PRIORITY_BELOW_NORMAL);
    }
}
static void PoolPut(HdJob* j) {
    EnterCriticalSection(&g_pool.lock);
    while (g_pool.count == HD_QUEUE) SleepConditionVariableCS(&g_pool.got, &g_pool.lock, INFINITE);
    g_pool.q[g_pool.tail] = j;
    g_pool.tail = (g_pool.tail + 1) % HD_QUEUE;
    g_pool.count++;
    WakeConditionVariable(&g_pool.put);
    LeaveCriticalSection(&g_pool.lock);
}
static void PoolEnd(void) {
    EnterCriticalSection(&g_pool.lock);
    g_pool.closing = 1;
    WakeAllConditionVariable(&g_pool.put);
    LeaveCriticalSection(&g_pool.lock);
    for (int i = 0; i < g_pool.nthreads; i++) {
        while (WaitForSingleObject(g_pool.threads[i], 100) == WAIT_TIMEOUT) if (!g_hd_background) ShowLine(g_line);
        CloseHandle(g_pool.threads[i]);
    }
    DeleteCriticalSection(&g_pool.lock);
}

// ---- making the set ------------------------------------------------------------------------------------------------------
static int HdTell(const wchar_t* text, UINT kind) {
    if (g_hd_background) { HdLog(L"%ls", text); return IDOK; }
    return Tell(text, kind);
}
static void HdMarker(wchar_t* out) { Path(out, MAX_PATH * 2, L"Game Files\\textures\\hd\\made.txt"); }
// the disc's textures are all made (by this version)?
static int HdDone(void) {
    wchar_t marker[MAX_PATH * 2];
    HdMarker(marker);
    char text[64];
    return ReadSmall(marker, text, sizeof text) && atoi(text) == HD_VERSION;
}
static int HdExists(uint64_t key, uint32_t w, uint32_t h) {
    wchar_t name[MAX_PATH * 2];
    _snwprintf(name, MAX_PATH * 2, L"%ls\\%ux%u_%016llx_HD.dds", g_hd_x4, w * 4, h * 4, (unsigned long long)key);
    return Exists(name);
}
// "made total" for the game's Options page (textures\hd\progress.txt)
static void HdProgress(const wchar_t* hd_dir, int made, int total) {
    wchar_t path[MAX_PATH * 2];
    char text[64];
    _snwprintf(path, MAX_PATH * 2, L"%ls\\progress.txt", hd_dir);
    _snprintf(text, sizeof text, "%d %d\n", made, total);
    WriteWhole(path, text, strlen(text));
}
// is the game running? (Game Files\Suffering2.exe)
static int GameRunning(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof e};
    int found = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, L"Suffering2.exe") == 0;
    CloseHandle(snap);
    return found;
}

// the textures the game has shown (textures\hd\seen\<w>x<h>_<key>.png, written by the port): the ones on the disc go to
// the front of the list, the others (HUD, menus and pictures from other files) are added as textures of their own
typedef struct { uint64_t key; uint32_t w, h; wchar_t file[64]; } HdSeen;
static int CompareKeys(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}
static HdSeen* ListSeen(const wchar_t* seen_dir, int* count) {
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*.png", seen_dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    int n = 0, cap = 0;
    HdSeen* list = NULL;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            unsigned w = 0, hh = 0;
            unsigned long long key = 0;
            if (swscanf(f.cFileName, L"%ux%u_%llx", &w, &hh, &key) != 3 || !key || wcslen(f.cFileName) > 63) continue;
            if (n == cap) { cap = cap ? cap * 2 : 256; list = (HdSeen*)realloc(list, cap * sizeof(HdSeen)); }
            list[n].key = key; list[n].w = w; list[n].h = hh;
            wcscpy(list[n].file, f.cFileName);
            n++;
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    if (n) qsort(list, n, sizeof *list, CompareKeys);   // the key is the first member
    *count = n;
    return list;
}
static HdSeen* FindSeen(HdSeen* seen, int n, uint64_t key) {
    return n ? (HdSeen*)bsearch(&key, seen, n, sizeof *seen, CompareKeys) : NULL;
}
// the disc's list in the order to make: shown ones first, keeping the order of each part
static void ShownFirst(HdList* l, HdSeen* seen, int n) {
    if (!n) return;
    HdTex* sorted = (HdTex*)malloc(l->count * sizeof(HdTex));
    int front = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < l->count; i++)
            if ((FindSeen(seen, n, l->t[i].key) != NULL) == !pass) sorted[front++] = l->t[i];
    memcpy(l->t, sorted, l->count * sizeof(HdTex));
    free(sorted);
}

// a job ready for the network: decoded, colour bled under holes, padded; -> NULL when it could not be read
static HdJob* Prepare(const HdTex* t, uint8_t* px, int bleed, int* failed) {
    if (!px) { (*failed)++; return NULL; }
    HdJob* j = (HdJob*)calloc(1, sizeof(HdJob));
    j->t = *t;
    j->alpha = AlphaKind(px, t->w * t->h);
    if (j->alpha != ALPHA_NONE && bleed) Bleed(px, t->w, t->h);
    j->wrap_x = Tiles(px, t->w, t->h, 0);
    j->wrap_y = Tiles(px, t->w, t->h, 1);
    j->pad_x = t->w < HD_PAD ? t->w : HD_PAD;
    j->pad_y = t->h < HD_PAD ? t->h : HD_PAD;
    j->source = px;
    return j;
}
// the network over the job's colour (and alpha); -> 0 the graphics card failed
static int Upscale(HdJob* j) {
    uint32_t PW = j->t.w + 2 * j->pad_x, PH = j->t.h + 2 * j->pad_y;
    uint8_t* padded = Pad(j->source, j->t.w, j->t.h, j->pad_x, j->pad_y, j->wrap_x, j->wrap_y);
    uint8_t* rgb = (uint8_t*)malloc((size_t)PW * PH * 3);
    for (uint32_t i = 0; i < PW * PH; i++) memcpy(rgb + i * 3, padded + i * 4, 3);
    j->up_rgb = (uint8_t*)malloc((size_t)PW * PH * 16 * 4);
    int ok = HdGpuUpscale(rgb, PW, PH, j->up_rgb, 4);
    if (ok && (j->alpha == ALPHA_BINARY || j->alpha == ALPHA_SOFT)) {
        for (uint32_t i = 0; i < PW * PH; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = padded[i * 4 + 3];
        j->up_a = (uint8_t*)malloc((size_t)PW * PH * 16 * 4);
        ok = HdGpuUpscale(rgb, PW, PH, j->up_a, 4);
    }
    free(rgb);
    free(padded);
    return ok;
}

// the network's weights: in this program (resource 2, tools\launcher\third_party\realesr-animevideov3-x2.bin)
static int OpenUpscaler(void) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const uint8_t* bin = g ? (const uint8_t*)LockResource(g) : NULL;
    return bin && HdGpuOpen(bin, SizeofResource(NULL, r));
}

// -> 1 the disc's textures are all made, 0 stopped or failed (said why). In the background it keeps going while the
// game runs, for the textures the game shows that are not on the disc's list.
static int MakeHdTextures(void) {
    // one maker at a time for this folder (another copy of the game elsewhere has its own)
    wchar_t mutex_name[96];
    uint32_t folder_hash = 2166136261u;
    for (const wchar_t* c = g_root; *c; c++) folder_hash = (folder_hash ^ (uint32_t)towlower(*c)) * 16777619u;
    _snwprintf(mutex_name, 96, L"Local\\Suffering2HdTextures_%08x", folder_hash);
    HANDLE one = CreateMutexW(NULL, TRUE, mutex_name);
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(one); return 0; }
    wchar_t assets[MAX_PATH * 2], hd_dir[MAX_PATH * 2], seen_dir[MAX_PATH * 2], marker[MAX_PATH * 2];
    Path(assets, MAX_PATH * 2, L"Game Files\\assets");
    Path(hd_dir, MAX_PATH * 2, L"Game Files\\textures");
    CreateDirectoryW(hd_dir, NULL);
    Path(hd_dir, MAX_PATH * 2, L"Game Files\\textures\\hd");
    CreateDirectoryW(hd_dir, NULL);
    Path(seen_dir, MAX_PATH * 2, L"Game Files\\textures\\hd\\seen");
    CreateDirectoryW(seen_dir, NULL);
    Path(g_hd_x2, MAX_PATH * 2, L"Game Files\\textures\\hd\\x2");
    Path(g_hd_x4, MAX_PATH * 2, L"Game Files\\textures\\hd\\x4");
    HdMarker(marker);
    g_hd_stop = 0;
    g_hd_running = 1;
    {
        wchar_t version_file[MAX_PATH * 2];
        char text[32] = {0}, want[32];
        _snwprintf(version_file, MAX_PATH * 2, L"%ls\\version.txt", hd_dir);
        _snprintf(want, sizeof want, "%d\n", HD_VERSION);
        if (!ReadSmall(version_file, text, sizeof text) || strcmp(text, want) != 0) {
            EmptyDir(g_hd_x2); EmptyDir(g_hd_x4);   // made by another version (or none): every texture is made again
            DeleteFileW(marker);
            WriteWhole(version_file, want, strlen(want));
        }
    }
    CreateDirectoryW(g_hd_x2, NULL);
    CreateDirectoryW(g_hd_x4, NULL);
    {   // the fix-up table changed: its textures are made again
        wchar_t fix_file[MAX_PATH * 2];
        char text[32] = {0}, want[32];
        _snwprintf(fix_file, MAX_PATH * 2, L"%ls\\fixups.txt", hd_dir);
        _snprintf(want, sizeof want, "%d\n", HD_FIXUPS);
        if (!ReadSmall(fix_file, text, sizeof text) || strcmp(text, want) != 0) {
            for (size_t i = 0; i < sizeof kFixUpKeys / sizeof kFixUpKeys[0]; i++)
                for (int set = 0; set < 2; set++) {
                    wchar_t pattern[MAX_PATH * 2], one[MAX_PATH * 2];
                    const wchar_t* dir = set ? g_hd_x4 : g_hd_x2;
                    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*_%016llx_HD.dds", dir, (unsigned long long)kFixUpKeys[i]);
                    WIN32_FIND_DATAW f;
                    HANDLE h = FindFirstFileW(pattern, &f);
                    if (h == INVALID_HANDLE_VALUE) continue;
                    do {
                        _snwprintf(one, MAX_PATH * 2, L"%ls\\%ls", dir, f.cFileName);
                        DeleteFileW(one);
                    } while (FindNextFileW(h, &f));
                    FindClose(h);
                }
            WriteWhole(fix_file, want, strlen(want));
        }
    }
    HdLog(L"making HD textures (version %d, %ls)", HD_VERSION, g_hd_background ? L"in the background" : L"with a window");

    HdDisc disc;
    memset(&disc, 0, sizeof disc);
    disc.iso = INVALID_HANDLE_VALUE;
    HdList list = {0};
    int whole = 0, made = 0, failed = 0, gpu_failed = 0;
    DWORD started = GetTickCount();
    if (!g_hd_background) ShowLine(L"Making HD textures\n\nReading the textures of your game image");
    if (!OpenDisc(&disc, assets)) {
        HdTell(L"The HD textures could not be made: no game image was found in Game Files\\assets.", MB_ICONERROR);
        goto out;
    }
    const wchar_t* only = _wgetenv(L"S2_HD_ONLY");   // tests: only the texture files whose names hold this
    for (int i = 0; i < disc.count; i++) {
        const wchar_t* name = disc.files[i].name;
        if (only && !wcsstr(name, only)) continue;
        if (!_wcsnicmp(name, L"French", 6) || wcsstr(name, L"\\French\\")) continue;   // the English game's only
        uint8_t* b = DiscRead(&disc, &disc.files[i]);
        if (b) ListTextures(&disc, i, b, disc.files[i].size, &list);
        free(b);
    }
    if (!OpenUpscaler()) {
        HdTell(L"The HD textures could not be made: this PC's graphics could not run the upscaler (Direct3D 11 is needed).\n\nThe game works without HD textures (Logs\\hd_textures.log says more).", MB_ICONINFORMATION);
        goto out;
    }
    PoolStart();
    {
        // the disc's textures (shown ones first), then again and again the shown ones from elsewhere while the game runs
        int seen_n = 0;
        HdSeen* seen = ListSeen(seen_dir, &seen_n);
        ShownFirst(&list, seen, seen_n);
        int todo = 0;
        for (int i = 0; i < list.count; i++) if (!HdExists(list.t[i].key, list.t[i].w, list.t[i].h)) todo++;
        made = list.count - todo;
        HdLog(L"%d texture files, %d textures on the disc (%d still to make), %d shown by the game", disc.count, list.count, todo, seen_n);
        HdProgress(hd_dir, made, list.count);
        // room for what is still to make: both sets together take about 450 KB a texture, and 200 MB are kept free
        ULARGE_INTEGER free_bytes;
        unsigned long long need = (unsigned long long)todo * 450 * 1024 + (200ull << 20);
        if (todo && GetDiskFreeSpaceExW(hd_dir, &free_bytes, NULL, NULL) && free_bytes.QuadPart < need) {
            wchar_t text[400];
            _snwprintf(text, 400, L"The HD textures need about %.1f GB more free on this drive, and it has %.1f GB.\n\nThe game works without them. Free some room, then switch HD textures on again in Options.",
                       need / 1073741824.0, free_bytes.QuadPart / 1073741824.0);
            HdTell(text, MB_ICONINFORMATION);
            todo = 0;
            gpu_failed = -1;   // (nothing made, and not "all made")
        }
        uint8_t* file_data = NULL;
        int file_index = -1, last_note = 0;
        DWORD last_progress = GetTickCount();
        for (int i = 0; i < list.count && !g_hd_stop && !gpu_failed; i++) {
            HdTex* t = &list.t[i];
            if (HdExists(t->key, t->w, t->h)) continue;
            if (t->file != file_index) {
                free(file_data);
                file_index = t->file;
                file_data = DiscRead(&disc, &disc.files[t->file]);
            }
            HdJob* j = Prepare(t, file_data ? Decode(t, file_data) : NULL, t->kind != 3, &failed);
            if (!j) continue;
            DWORD gpu_start = GetTickCount();
            if (!Upscale(j)) { gpu_failed = 1; FreeJob(j); break; }
            PoolPut(j);
            made++;
            if (GameRunning()) {   // the game has the graphics card to itself at least as long again
                SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
                DWORD spent = GetTickCount() - gpu_start;
                Sleep(spent < 1000 ? spent + 5 : 1000);
            } else {
                SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
            }
            if (GetTickCount() - last_progress > 2000) {
                last_progress = GetTickCount();
                HdProgress(hd_dir, made, list.count);
                if (made - last_note >= 500) { last_note = made; HdLog(L"%d of %d (%.0f s)", made, list.count, (GetTickCount() - started) / 1000.0); }
                if (!g_hd_background) {
                    double seconds = (GetTickCount() - started) / 1000.0;
                    int since = made - (list.count - todo), left = list.count - made;
                    int minutes = since > 0 ? (int)(seconds / since * left / 60.0 + 0.999) : 0;
                    wchar_t text[400];
                    _snwprintf(text, 400, L"Making HD textures\n\n%d of %d done%ls\n\nClosing this window stops after this texture; it continues later.",
                               made, list.count, since > 0 && seconds > 5 ? (minutes <= 1 ? L", about a minute left" : L"") : L"");
                    ShowLine(text);
                }
            }
        }
        free(file_data);
        whole = !g_hd_stop && !gpu_failed && !only;
        if (gpu_failed < 0) gpu_failed = 0;   // no room: said above
        HdProgress(hd_dir, made, list.count);
        HdLog(L"the disc's textures: %d of %d made, %d could not be read, %ls, %.0f s", made, list.count, failed,
              whole ? L"all" : L"stopped", (GetTickCount() - started) / 1000.0);
        if (whole) {
            char text[64];
            _snprintf(text, 64, "%d %d %d\n", HD_VERSION, made, failed);
            WriteWhole(marker, text, strlen(text));
        }
        // the textures from elsewhere: in the background, as long as the game runs (it writes more as it shows them)
        int idle = 0, others = 0;
        while (!g_hd_stop && !gpu_failed) {
            free(seen);
            seen = ListSeen(seen_dir, &seen_n);
            int did = 0;
            for (int i = 0; i < seen_n && !g_hd_stop; i++) {
                HdSeen* s = &seen[i];
                if (HdExists(s->key, s->w, s->h)) continue;
                int on_disc = 0;
                for (int k = 0; k < list.count && !on_disc; k++) on_disc = list.t[k].key == s->key;
                if (on_disc) continue;   // made above, or could not be (the disc's list is done by now)
                wchar_t path[MAX_PATH * 2];
                _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", seen_dir, s->file);
                DWORD size = 0;
                uint8_t* file = ReadWhole(path, &size);
                int w = 0, h = 0, n = 0;
                uint8_t* px = file ? stbi_load_from_memory(file, (int)size, &w, &h, &n, 4) : NULL;
                free(file);
                if (!px || (uint32_t)w != s->w || (uint32_t)h != s->h) { if (px) stbi_image_free(px); DeleteFileW(path); continue; }
                uint8_t* copy = (uint8_t*)malloc((size_t)w * h * 4);   // the job frees with free()
                memcpy(copy, px, (size_t)w * h * 4);
                stbi_image_free(px);
                HdTex t = {s->key, s->w, s->h, 2, 0, -1, 0};
                HdJob* j = Prepare(&t, copy, 0, &failed);
                if (!j) continue;
                j->seen = 1;
                DWORD gpu_start = GetTickCount();
                if (!Upscale(j)) { gpu_failed = 1; FreeJob(j); break; }
                PoolPut(j);
                did++;
                others++;
                if (GameRunning()) {
                    DWORD spent = GetTickCount() - gpu_start;
                    Sleep(spent < 1000 ? spent + 5 : 1000);
                }
            }
            if (did) { idle = 0; continue; }
            if (!g_hd_background || !GameRunning()) {
                if (++idle >= 2) break;   // once more after the game closed, for its last pictures
            }
            Sleep(5000);
        }
        free(seen);
        if (others) HdLog(L"%d textures from outside the disc's texture files made", others);
    }
    PoolEnd();
    if (gpu_failed > 0)
        HdTell(L"The graphics card stopped the upscaler. What was made so far is kept; it continues the next time the game starts with HD textures on (Logs\\hd_textures.log says more).", MB_ICONINFORMATION);
    HdLog(L"done: %d made, %d finishing failed, %.0f s", (int)g_pool.ok, (int)g_pool.failed, (GetTickCount() - started) / 1000.0);
out:
    free(list.t);
    free(disc.files);
    if (disc.iso != INVALID_HANDLE_VALUE) CloseHandle(disc.iso);
    g_hd_running = 0;
    if (one) { ReleaseMutex(one); CloseHandle(one); }
    return whole;
}

// starts the maker in the background (no window); -> 1 started
static int StartHdInBackground(void) {
    wchar_t self[MAX_PATH * 2], command[MAX_PATH * 3];
    GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    _snwprintf(command, MAX_PATH * 3, L"\"%ls\" --hd", self);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS | BELOW_NORMAL_PRIORITY_CLASS, NULL, g_root, &si, &pi)) return 0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}
