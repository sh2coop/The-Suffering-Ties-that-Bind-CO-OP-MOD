// HD sound, made on the player's PC from his own disc image (asked for on 2026-11-27: an "HD" option for the sound like
// the HD texture pack, made by Build Recomp, never shipped). Nothing of the game is in this program or the folder.
//
// Every sound of the game (effects, music, voices, the sound of the cut scenes: the movies have no sound of their own)
// is a wave in one of the .sdu files, all Xbox ADPCM (4 bits a sample). The maker reads each wave from the image,
// decodes it exactly as the port plays it, and makes an enhanced copy, all plain signal processing in this file (no
// outside program, no download, no learned model). Only waves under 32 kHz get a copy (2,527 of 4,056; the others
// have their treble and are only read better, by the port's cubic). Steps 2 and 3 are used; step 1 is kept for the
// test tool but is off: on the game's own waves it made them measurably further from the clean sound.
//   1. ADPCM noise out (off). An ADPCM decoder knows how coarse each sample is (its step), so the hiss the format adds is
//      known sample by sample: (step / 4)^2 / 12. A short-time spectral filter (Wiener, decision directed, at most
//      -10 dB) takes out what lies under that floor, and leaves what is above it.
//   2. More samples. Waves below 32 kHz (most voices are 22 kHz) are resampled by a whole number to 32 kHz or more with
//      a long windowed-sinc filter; the port then reads them with a cubic in place of the console's straight lines
//      (src\game\hdsound.cpp), so no mirror tones above the original's top.
//   3. Treble back. A wave whose content reaches its format's top (a 22 kHz voice stops at 11 kHz) gets the octave
//      above it made from the octave below it (the octave below squared gives tones at the sums: the octave above),
//      shaped each 46 ms to continue that frame's own spectral slope, 3 dB under it. A wave that stops well under
//      its top (a muffled sound) is left dark.
// Each enhanced wave is packed losslessly (src\game\hdsound_format.h) into Game Files\sound\hd\<key>.hsw.
//
// Driven like the HD textures: MakeHdSound() (launcher.c) runs in the background ("Build Recomp.exe --hd-sound"), on all
// processors but one at below normal priority, slows to a trickle while the game runs, keeps what is made, and the
// next run goes on where it stopped. Progress: Game Files\sound\hd\progress.txt ("made total"), made.txt when whole
// ("version made failed"). Log: Logs\hd_sound.log.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

#include "../../src/game/hdsound_format.h"

#ifndef HS_PI
#define HS_PI 3.14159265358979323846
#endif

// ---- log --------------------------------------------------------------------------------------------------------------
static wchar_t g_hs_log[MAX_PATH * 2];
static CRITICAL_SECTION g_hs_log_lock;
static void HsLog(const wchar_t* format, ...) {
    if (!g_hs_log[0]) return;
    wchar_t text[1024];
    va_list a;
    va_start(a, format);
    _vsnwprintf(text, 1023, format, a);
    va_end(a);
    text[1023] = 0;
    EnterCriticalSection(&g_hs_log_lock);
    FILE* f = _wfopen(g_hs_log, L"a, ccs=UTF-8");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
        fclose(f);
    }
    LeaveCriticalSection(&g_hs_log_lock);
}

// ---- the disc: an .iso (XDVDFS) or the disc's files -------------------------------------------------------------------
typedef struct { wchar_t name[260]; uint64_t at; uint32_t size; } HsFile;  // at: byte offset in the image
typedef struct { HANDLE iso; uint64_t base; HsFile* files; int count, cap; } HsDisc;

static int HsWanted(const wchar_t* name) {
    size_t n = wcslen(name);
    if (n < 4 || _wcsicmp(name + n - 4, L".sdu")) return 0;
    // the French folder's waves are another language's; the PAL folder's are the NTSC ones again (the key dedupes)
    return !wcsstr(name, L"French") && !wcsstr(name, L"french");
}
static void HsAdd(HsDisc* d, const wchar_t* name, uint64_t at, uint32_t size) {
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->files = (HsFile*)realloc(d->files, d->cap * sizeof(HsFile));
    }
    HsFile* f = &d->files[d->count++];
    wcsncpy(f->name, name, 259);
    f->name[259] = 0;
    f->at = at;
    f->size = size;
}
static int HsReadAt(HANDLE h, uint64_t at, void* out, DWORD size) {
    OVERLAPPED o = {0};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD got = 0;
    return ReadFile(h, out, size, &got, &o) && got == size;
}
static void HsWalkIso(HsDisc* d, uint32_t sector, uint32_t size, const wchar_t* prefix, int depth) {
    if (!size || depth > 16) return;
    uint8_t* table = (uint8_t*)malloc(size);
    if (!table || !HsReadAt(d->iso, d->base + (uint64_t)sector * 2048, table, size)) { free(table); return; }
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
        for (int i = 0; i < n && len < 258; i++) name[len++] = (wchar_t)table[at + 14 + i];
        name[len] = 0;
        if (attr & 0x10) {
            wcscat(name, L"/");
            HsWalkIso(d, start, length, name, depth + 1);
        } else if (HsWanted(name)) {
            HsAdd(d, name, d->base + (uint64_t)start * 2048, length);
        }
        if (left && top < 511) stack[top++] = left * 4u;
        if (right && top < 511) stack[top++] = right * 4u;
    }
    free(table);
}
static void HsWalkFolder(HsDisc* d, const wchar_t* dir, int depth) {
    if (depth > 16) return;
    wchar_t pattern[MAX_PATH * 2], one[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.cFileName[0] == L'.') continue;
        _snwprintf(one, MAX_PATH * 2, L"%ls\\%ls", dir, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) HsWalkFolder(d, one, depth + 1);
        else if (HsWanted(one)) HsAdd(d, one, 0, f.nFileSizeLow);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}
// path: an .iso file or a folder holding the disc's files (or an .iso); -> 0 no image
static int HsOpenDisc(HsDisc* d, const wchar_t* path) {
    memset(d, 0, sizeof *d);
    d->iso = INVALID_HANDLE_VALUE;
    wchar_t iso[MAX_PATH * 2] = {0};
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wcsncpy(iso, path, MAX_PATH * 2 - 1);
    } else {
        wchar_t pattern[MAX_PATH * 2];
        _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*.iso", path);
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileW(pattern, &f);
        if (h != INVALID_HANDLE_VALUE) {
            _snwprintf(iso, MAX_PATH * 2, L"%ls\\%ls", path, f.cFileName);
            FindClose(h);
        }
    }
    if (iso[0]) {
        d->iso = CreateFileW(iso, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (d->iso == INVALID_HANDLE_VALUE) return 0;
        static const uint64_t bases[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
        for (int i = 0; i < 4; i++) {
            uint8_t head[32];
            if (!HsReadAt(d->iso, bases[i] + 32 * 2048, head, 32) || memcmp(head, "MICROSOFT*XBOX*MEDIA", 20)) continue;
            d->base = bases[i];
            HsWalkIso(d, *(uint32_t*)(head + 20), *(uint32_t*)(head + 24), L"", 0);
            return d->count > 0;
        }
        CloseHandle(d->iso);
        d->iso = INVALID_HANDLE_VALUE;
        return 0;
    }
    HsWalkFolder(d, path, 0);
    return d->count > 0;
}
static uint8_t* HsDiscRead(HsDisc* d, const HsFile* f) {
    uint8_t* b = (uint8_t*)malloc(f->size ? f->size : 1);
    if (!b) return NULL;
    if (d->iso != INVALID_HANDLE_VALUE) {
        if (!HsReadAt(d->iso, f->at, b, f->size)) { free(b); return NULL; }
        return b;
    }
    HANDLE h = CreateFileW(f->name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD got = 0;
    int ok = h != INVALID_HANDLE_VALUE && ReadFile(h, b, f->size, &got, NULL) && got == f->size;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { free(b); return NULL; }
    return b;
}

// ---- the waves of a .sdu file ----------------------------------------------------------------------------------------
// A .sdu ends in a table of 14-byte entries {u16 kind, u32 id, u32 offset, u32 size}: kind 0x302 a wave's 60-byte
// header, 0x304 its data, 0x305 something else of the wave. Header words: [2] flags (8 repeats), [11] format (low half
// channels, high half 4 = Xbox ADPCM), [12] rate, [14] data bytes.
typedef struct {
    uint64_t key, first;
    uint32_t channels, rate, bytes, flags;
    int file;           // the .sdu it was first seen in
    uint32_t at;        // its data's offset in that file
} HsWave;
typedef struct { HsWave* w; int count, cap; } HsWaves;

static int HsHave(const HsWaves* l, uint64_t key) {
    // a short list is searched; the full list (about 4,000) is kept sorted once read
    for (int i = 0; i < l->count; i++)
        if (l->w[i].key == key) return 1;
    return 0;
}
static void HsList(const uint8_t* d, uint32_t size, int file, HsWaves* l) {
    if (size < 28) return;
    uint32_t at = size - 14;
    while (at >= 14 && (*(uint16_t*)(d + at) >> 8) == 3) at -= 14;
    at += 14;
    // header and data of each id
    for (uint32_t e = at; e + 14 <= size; e += 14) {
        uint16_t kind = *(uint16_t*)(d + e);
        if (kind != 0x304) continue;
        uint32_t id = *(uint32_t*)(d + e + 2), off = *(uint32_t*)(d + e + 6), bytes = *(uint32_t*)(d + e + 10);
        uint32_t head = 0, head_size = 0;
        for (uint32_t g = at; g + 14 <= size; g += 14)
            if (*(uint16_t*)(d + g) == 0x302 && *(uint32_t*)(d + g + 2) == id) {
                head = *(uint32_t*)(d + g + 6);
                head_size = *(uint32_t*)(d + g + 10);
                break;
            }
        if (head_size < 60 || (uint64_t)head + 60 > size || (uint64_t)off + bytes > size) continue;
        const uint32_t* h = (const uint32_t*)(d + head);
        uint32_t format = h[11], channels = format & 0xFFFF, rate = h[12];
        if ((format >> 16) != 4 || channels < 1 || channels > 2 || rate < 1000 || rate > 96000) continue;
        uint32_t block = 36 * channels;
        if (bytes < block) continue;
        bytes -= bytes % block;
        uint64_t key = HsKey(d + off, bytes);
        if (HsHave(l, key)) continue;
        if (l->count == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 256;
            l->w = (HsWave*)realloc(l->w, l->cap * sizeof(HsWave));
        }
        HsWave* w = &l->w[l->count++];
        w->key = key;
        w->first = HsKey(d + off, block);
        w->channels = channels;
        w->rate = rate;
        w->bytes = bytes;
        w->flags = h[2];
        w->file = file;
        w->at = off;
    }
}

// ---- ADPCM, exactly as the port plays it (sdk\runtime\xdk\dsound.cpp DecodeBlock: 64 frames of each block) --------
static const int kHsIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int kHsStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};

// channel c of the wave into x (frames = blocks * 64), and each sample's quantisation noise variance into var
static void HsDecode(const uint8_t* data, uint32_t blocks, uint32_t ch, uint32_t c, float* x, float* var) {
    for (uint32_t k = 0; k < blocks; k++) {
        const uint8_t* b = data + (size_t)k * 36 * ch;
        int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
        float* o = x + (size_t)k * 64;
        float* v = var + (size_t)k * 64;
        o[0] = pred / 32768.0f;
        // the header's sample is exact up to the encoder's own error: give it the step's noise too
        { float q = kHsStep[idx] / 4.0f / 32768.0f; v[0] = q * q / 12.0f; }
        const uint8_t* d = b + ch * 4;
        int n = 1;
        for (int i = 0; i < 8 && n < 64; i++)
            for (int j = 0; j < 8 && n < 64; j++) {
                int nib = (d[(i * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kHsStep[idx], diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred = nib & 8 ? pred - diff : pred + diff;
                pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
                float q = step / 4.0f / 32768.0f;
                v[n] = q * q / 12.0f;
                idx += kHsIndex[nib];
                idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                o[n++] = pred / 32768.0f;
            }
    }
}

// ---- FFT (complex, radix 2, in place) --------------------------------------------------------------------------------
typedef struct { int n; float* cs; int* rev; } HsFft;
static void HsFftInit(HsFft* f, int n) {
    f->n = n;
    f->cs = (float*)malloc(sizeof(float) * n);
    f->rev = (int*)malloc(sizeof(int) * n);
    for (int i = 0; i < n / 2; i++) {
        f->cs[2 * i] = (float)cos(2 * HS_PI * i / n);
        f->cs[2 * i + 1] = (float)-sin(2 * HS_PI * i / n);
    }
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        f->rev[i] = r;
    }
}
static void HsFftFree(HsFft* f) { free(f->cs); free(f->rev); }
// re/im interleaved; inverse: conjugate twiddles, no scaling
static void HsFftRun(const HsFft* f, float* z, int inverse) {
    int n = f->n;
    for (int i = 0; i < n; i++) {
        int r = f->rev[i];
        if (r > i) {
            float a = z[2 * i], b = z[2 * i + 1];
            z[2 * i] = z[2 * r];
            z[2 * i + 1] = z[2 * r + 1];
            z[2 * r] = a;
            z[2 * r + 1] = b;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < len / 2; j++) {
                float wr = f->cs[2 * j * step], wi = f->cs[2 * j * step + 1];
                if (inverse) wi = -wi;
                float* a = z + 2 * (i + j);
                float* b = z + 2 * (i + j + len / 2);
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
    }
}
static int HsPow2Near(double samples) {
    int n = 64;
    while (n < 8192 && n * 1.41 < samples) n <<= 1;
    return n;
}

// ---- 1. ADPCM noise out -----------------------------------------------------------------------------------------------
// x and var have n samples plus pad on each side already filled (a loop wraps, else zeros); y gets the result (the
// same layout). Short-time spectra of about 23 ms, hop a quarter, square-root Hann both ways.
static void HsDenoise(const float* x, const float* var, float* y, size_t total, uint32_t rate) {
    int N = HsPow2Near(rate * 0.023), H = N / 4, K = N / 2 + 1;
    HsFft fft;
    HsFftInit(&fft, N);
    float* w = (float*)malloc(sizeof(float) * N);
    float wsum2 = 0;
    for (int i = 0; i < N; i++) {
        w[i] = (float)sqrt(0.5 - 0.5 * cos(2 * HS_PI * i / N));
        wsum2 += w[i] * w[i];
    }
    float ola = wsum2 / H;  // the synthesis sum of w^2 over the hops
    float* z = (float*)malloc(sizeof(float) * 2 * N);
    float* prev = (float*)calloc(K, sizeof(float));  // the last frame's clean power / noise power, per bin
    float* g = (float*)malloc(sizeof(float) * K);
    memset(y, 0, sizeof(float) * total);
    const float alpha = 0.98f, floor_gain = 0.316f;  // -10 dB at most
    for (long long start = -(long long)N; start < (long long)total; start += H) {
        double noise = 0;
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            float s = (t >= 0 && t < (long long)total) ? x[t] : 0.0f;
            float v = (t >= 0 && t < (long long)total) ? var[t] : 0.0f;
            z[2 * i] = s * w[i];
            z[2 * i + 1] = 0;
            noise += (double)v * w[i] * w[i];
        }
        HsFftRun(&fft, z, 0);
        float lam = (float)noise + 1e-20f;
        for (int k = 0; k < K; k++) {
            float p = z[2 * k] * z[2 * k] + z[2 * k + 1] * z[2 * k + 1];
            float gamma = p / lam;
            float xi = alpha * prev[k] + (1 - alpha) * (gamma > 1 ? gamma - 1 : 0);
            float gain = xi / (1 + xi);
            g[k] = gain;
            prev[k] = gain * gain * gamma;
        }
        // a little smoothing across bins against "musical" noise, then the floor
        for (int k = 0; k < K; k++) {
            float a = g[k > 0 ? k - 1 : k], b = g[k], c = g[k < K - 1 ? k + 1 : k];
            float s = 0.25f * a + 0.5f * b + 0.25f * c;
            s = s < b ? s : b;  // never more than the bin's own gain
            g[k] = s < floor_gain ? floor_gain : s;
        }
        for (int k = 0; k < K; k++) {
            z[2 * k] *= g[k];
            z[2 * k + 1] *= g[k];
            if (k > 0 && k < N / 2) {
                z[2 * (N - k)] = z[2 * k];
                z[2 * (N - k) + 1] = -z[2 * k + 1];
            }
        }
        HsFftRun(&fft, z, 1);
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            if (t >= 0 && t < (long long)total) y[t] += z[2 * i] / N * w[i] / ola;
        }
    }
    free(w); free(z); free(prev); free(g);
    HsFftFree(&fft);
}

// ---- 2. more samples ------------------------------------------------------------------------------------------------
#define HS_TAPS 32  // console samples on each side
static double HsBessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}
// in: n samples with HS_TAPS samples before and after (wrapped or zero); out: n * m samples
static void HsUpsample(const float* in, size_t n, uint32_t m, float* out) {
    if (m == 1) { memcpy(out, in + HS_TAPS, sizeof(float) * n); return; }  // out[i] is in[i + HS_TAPS], as below
    int taps = 2 * HS_TAPS;
    float* h = (float*)malloc(sizeof(float) * taps * m);
    double fc = 0.5 * 0.96 / m, beta = 9.0, half = HS_TAPS * (double)m;
    for (uint32_t p = 0; p < m; p++) {
        double sum = 0;
        for (int j = 0; j < taps; j++) {
            // output m*n0 + p takes input n0 - j + HS_TAPS - 1 ... : distance t in output samples
            double t = (double)(j - (HS_TAPS - 1)) * m + p;
            double s = t == 0 ? 2 * fc : sin(2 * HS_PI * fc * t) / (HS_PI * t);
            double r = t / half;
            double kw = fabs(r) >= 1 ? 0 : HsBessel0(beta * sqrt(1 - r * r)) / HsBessel0(beta);
            h[p * taps + j] = (float)(s * kw);
            sum += s * kw;
        }
        for (int j = 0; j < taps; j++) h[p * taps + j] = (float)(h[p * taps + j] / sum);
    }
    for (size_t n0 = 0; n0 < n; n0++) {
        for (uint32_t p = 0; p < m; p++) {
            const float* hp = h + p * taps;
            double acc = 0;
            // input index for tap j: n0 + (HS_TAPS - 1) - j, in the padded buffer + HS_TAPS
            const float* base = in + n0 + HS_TAPS + (HS_TAPS - 1);
            for (int j = 0; j < taps; j++) acc += base[-j] * hp[j];
            out[n0 * m + p] = (float)acc;
        }
    }
    free(h);
}

// ---- 3. treble back ---------------------------------------------------------------------------------------------------
// u: the wave at rate (the HD rate), total samples (with pads); top: the frequency its content stops at. Adds the
// octave above top (up to 20 kHz). -> 1 when it added anything
static int HsTreble(float* u, size_t total, uint32_t rate, double top) {
    int N = HsPow2Near(rate * 0.046), H = N / 4;
    double bin = (double)rate / N;
    double hi = 2 * top < 20000 ? 2 * top : 20000;
    if (hi > 0.48 * rate) hi = 0.48 * rate;
    if (hi < top * 1.2) return 0;
    int k_lo = (int)(top / 2 / bin), k_top = (int)(top / bin), k_hi = (int)(hi / bin);
    if (k_top - k_lo < 8) return 0;
    HsFft fft;
    HsFftInit(&fft, N);
    float* w = (float*)malloc(sizeof(float) * N);
    float wsum2 = 0;
    for (int i = 0; i < N; i++) {
        w[i] = (float)sqrt(0.5 - 0.5 * cos(2 * HS_PI * i / N));
        wsum2 += w[i] * w[i];
    }
    float ola = wsum2 / H;
    float* z = (float*)malloc(sizeof(float) * 2 * N);
    float* band = (float*)calloc(total, sizeof(float));  // the octave below top, then squared
    // a. the octave below top on its own
    for (long long start = -(long long)N; start < (long long)total; start += H) {
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            z[2 * i] = (t >= 0 && t < (long long)total) ? u[t] * w[i] : 0.0f;
            z[2 * i + 1] = 0;
        }
        HsFftRun(&fft, z, 0);
        for (int k = 0; k < N; k++) {
            int kk = k <= N / 2 ? k : N - k;
            if (kk < k_lo || kk > k_top) z[2 * k] = z[2 * k + 1] = 0;
        }
        HsFftRun(&fft, z, 1);
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            if (t >= 0 && t < (long long)total) band[t] += z[2 * i] / N * w[i] / ola;
        }
    }
    double rms = 0;
    for (size_t t = 0; t < total; t++) rms += (double)band[t] * band[t];
    rms = sqrt(rms / (total ? total : 1));
    if (rms < 1e-7) { free(w); free(z); free(band); HsFftFree(&fft); return 0; }
    for (size_t t = 0; t < total; t++) band[t] = (float)(band[t] * band[t] / rms);
    // b. frame by frame: the squared band's spectrum above top, shaped to continue the frame's slope
    float* out = (float*)calloc(total, sizeof(float));
    float* zu = (float*)malloc(sizeof(float) * 2 * N);
    // third-octave bands from top to hi
    int bands = 0;
    int edge[64];
    for (double f = top; f < hi && bands < 62; f *= 1.2599) edge[bands++] = (int)(f / bin);
    edge[bands] = k_hi;
    double ref_lo_hz = top * 0.7937, low_hi_hz = top / 2 * 1.2599;  // the top third octave, the bottom one
    int k_ref = (int)(ref_lo_hz / bin), k_low = (int)(low_hi_hz / bin);
    double slope_s = -12, level_s = -200;
    int added = 0;
    for (long long start = -(long long)N; start < (long long)total; start += H) {
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            int in = t >= 0 && t < (long long)total;
            z[2 * i] = in ? band[t] * w[i] : 0.0f;
            zu[2 * i] = in ? u[t] * w[i] : 0.0f;
            z[2 * i + 1] = zu[2 * i + 1] = 0;
        }
        HsFftRun(&fft, z, 0);
        HsFftRun(&fft, zu, 0);
        double e_ref = 0, e_low = 0, e_body = 0;
        for (int k = k_ref; k <= k_top; k++) e_ref += zu[2 * k] * zu[2 * k] + zu[2 * k + 1] * zu[2 * k + 1];
        for (int k = k_lo; k <= k_low; k++) e_low += zu[2 * k] * zu[2 * k] + zu[2 * k + 1] * zu[2 * k + 1];
        for (int k = 1; k <= k_top; k++) e_body += zu[2 * k] * zu[2 * k] + zu[2 * k + 1] * zu[2 * k + 1];
        e_ref /= (k_top - k_ref + 1);
        e_low /= (k_low - k_lo + 1);
        e_body /= k_top;
        double level = 10 * log10(e_ref + 1e-30), low = 10 * log10(e_low + 1e-30);
        double slope = (level - low) / (2.0 / 3.0);  // dB an octave between the two third octaves' centres
        if (slope > -3) slope = -3;
        if (slope < -30) slope = -30;
        slope_s = 0.5 * slope_s + 0.5 * slope;
        level_s = level_s < -150 ? level : 0.5 * level_s + 0.5 * level;
        // nothing where the top is silent, or 60 dB under the body of the sound
        int gate = e_ref < 1e-12 || e_ref < e_body * 1e-6;
        memset(zu, 0, sizeof(float) * 2 * N);
        if (!gate) {
            double centre = top * 0.8909;  // the reference third octave's centre
            for (int b = 0; b < bands; b++) {
                int k0 = edge[b], k1 = edge[b + 1];
                if (k1 <= k0) continue;
                double er = 0;
                for (int k = k0; k < k1; k++) er += z[2 * k] * z[2 * k] + z[2 * k + 1] * z[2 * k + 1];
                er /= (k1 - k0);
                if (er <= 1e-30) continue;
                double fmid = (k0 + k1) * 0.5 * bin;
                double target_db = level_s + (slope_s - 3) * log2(fmid / centre) - 3;
                double gain = sqrt(pow(10, target_db / 10) / er);
                for (int k = k0; k < k1; k++) {
                    double f = k * bin, fade = 1;
                    // in over a twelfth of an octave above top, out over the last fifth below hi
                    if (f < top * 1.0595) fade = (f - top) / (top * 0.0595);
                    if (f > hi * 0.8) fade *= (hi - f) / (hi * 0.2);
                    if (fade <= 0) continue;
                    if (fade > 1) fade = 1;
                    zu[2 * k] = (float)(z[2 * k] * gain * fade);
                    zu[2 * k + 1] = (float)(z[2 * k + 1] * gain * fade);
                    zu[2 * (N - k)] = zu[2 * k];
                    zu[2 * (N - k) + 1] = -zu[2 * k + 1];
                }
            }
            added = 1;
        }
        HsFftRun(&fft, zu, 1);
        for (int i = 0; i < N; i++) {
            long long t = start + i;
            if (t >= 0 && t < (long long)total) out[t] += zu[2 * i] / N * w[i] / ola;
        }
    }
    for (size_t t = 0; t < total; t++) u[t] += out[t];
    free(w); free(z); free(zu); free(band); free(out);
    HsFftFree(&fft);
    return added;
}

// where a wave's content stops: the highest frequency (under 0.96 of its top) whose long-time level is within 50 dB
// of its loudest band
static double HsContentTop(const float* x, size_t n, uint32_t rate) {
    int N = HsPow2Near(rate * 0.046), K = N / 2 + 1;
    if (n < (size_t)N) return 0;
    HsFft fft;
    HsFftInit(&fft, N);
    float* z = (float*)malloc(sizeof(float) * 2 * N);
    double* p = (double*)calloc(K, sizeof(double));
    for (size_t start = 0; start + N <= n; start += N / 2) {
        for (int i = 0; i < N; i++) {
            z[2 * i] = x[start + i] * (float)(0.5 - 0.5 * cos(2 * HS_PI * i / N));
            z[2 * i + 1] = 0;
        }
        HsFftRun(&fft, z, 0);
        for (int k = 0; k < K; k++) p[k] += z[2 * k] * z[2 * k] + z[2 * k + 1] * z[2 * k + 1];
    }
    // smooth over about a twelfth of an octave and find the loudest
    double best = 0, bin = (double)rate / N;
    double* s = (double*)calloc(K, sizeof(double));
    for (int k = 1; k < K; k++) {
        int r = (int)(k * 0.03) + 1, c = 0;
        for (int j = k - r; j <= k + r; j++)
            if (j > 0 && j < K) { s[k] += p[j]; c++; }
        s[k] /= c;
        if (k * bin > 100 && s[k] > best) best = s[k];
    }
    double top = 0;
    for (int k = 1; k < K; k++)
        if (k * bin <= 0.96 * rate / 2 && s[k] > best * 1e-5) top = k * bin;
    free(z); free(p); free(s);
    HsFftFree(&fft);
    return top;
}

// ---- lossless packing (the reader is in hdsound_format.h) ------------------------------------------------------------
typedef struct { uint8_t* p; size_t size, cap; uint64_t acc; int nacc; } HsOut;
static void HsByte(HsOut* o, uint8_t b) {
    if (o->size == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 65536;
        o->p = (uint8_t*)realloc(o->p, o->cap);
    }
    o->p[o->size++] = b;
}
static void HsPut(HsOut* o, uint32_t v, int count) {
    for (int i = count - 1; i >= 0; i--) {
        o->acc = (o->acc << 1) | ((v >> i) & 1);
        if (++o->nacc == 8) {
            HsByte(o, (uint8_t)o->acc);
            o->acc = 0;
            o->nacc = 0;
        }
    }
}
static void HsAlign(HsOut* o) {
    while (o->nacc) HsPut(o, 0, 1);
}
static void HsUnaryPut(HsOut* o, uint32_t q) {
    while (q >= 24) { HsPut(o, 0, 24); q -= 24; }
    HsPut(o, 1, (int)q + 1);  // q zeros then a one
}
static uint32_t HsZig(int32_t r) { return ((uint32_t)r << 1) ^ (uint32_t)(r >> 31); }
static uint64_t HsRiceCost(const uint32_t* u, int n, int k) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) c += (u[i] >> k) + 1 + k;
    return c;
}
// a linear predictor of `order` taps for s (Levinson-Durbin on the Hann-windowed autocorrelation), quantised
static int HsLpc(const int16_t* s, uint32_t n, int order, HsPredictor* p) {
    double r[33] = {0}, a[33] = {0}, tmp[33];
    double* w = (double*)malloc(sizeof(double) * n);
    for (uint32_t i = 0; i < n; i++) w[i] = s[i] * (0.5 - 0.5 * cos(2 * HS_PI * (i + 0.5) / n));
    for (int lag = 0; lag <= order; lag++) {
        double acc = 0;
        for (uint32_t i = lag; i < n; i++) acc += w[i] * w[i - lag];
        r[lag] = acc;
    }
    free(w);
    if (r[0] <= 0) return 0;
    r[0] *= 1.0 + 1e-9;  // a whisker of white noise keeps it stable
    double err = r[0];
    for (int i = 1; i <= order; i++) {
        double k = r[i];
        for (int j = 1; j < i; j++) k -= a[j] * r[i - j];
        k /= err;
        for (int j = 1; j < i; j++) tmp[j] = a[j] - k * a[i - j];
        for (int j = 1; j < i; j++) a[j] = tmp[j];
        a[i] = k;
        err *= 1 - k * k;
        if (err <= 0) return 0;
    }
    const int bits = 15;
    double most = 0;
    for (int j = 1; j <= order; j++) most = fabs(a[j]) > most ? fabs(a[j]) : most;
    int shift = 0;
    while (shift < 31 && most * (double)(1 << (shift + 1)) < (double)((1 << (bits - 1)) - 1)) shift++;
    p->lpc = 1;
    p->order = order;
    p->shift = shift;
    double carry = 0;  // quantise with the error carried on, as FLAC does
    for (int j = 0; j < order; j++) {
        double v = a[j + 1] * (double)(1u << shift) + carry;
        long q = lrint(v);
        int lim = (1 << (bits - 1)) - 1;
        q = q > lim ? lim : q < -lim ? -lim : q;
        carry = v - q;
        p->q[j] = (int32_t)q;
    }
    return 1;
}
static uint64_t HsCost(const HsPredictor* p, const int16_t* s, uint32_t n) {
    uint64_t c = 0;
    for (uint32_t i = p->order; i < n; i++) {
        int64_t r = (int64_t)s[i] - HsPredict(p, s, i);
        if (r > 400000 || r < -400000) return ~0ull;  // would not fit the raw escape
        c += HsZig((int32_t)r);
    }
    return c;
}
static void HsEncodeChannel(HsOut* o, const int16_t* s, uint32_t n) {
    uint32_t* u = (uint32_t*)malloc(sizeof(uint32_t) * (n + 1));
    HsPredictor best = {0}, p;
    uint64_t best_cost = ~0ull;
    for (int order = 0; order <= 3 && (uint32_t)order < n; order++) {
        memset(&p, 0, sizeof p);
        p.order = order;
        uint64_t c = HsCost(&p, s, n);
        if (c < best_cost) { best_cost = c; best = p; }
    }
    static const int kTaps[] = {8, 16, 24};
    for (int t = 0; t < 3; t++) {
        if ((uint32_t)kTaps[t] * 8 > n) continue;
        memset(&p, 0, sizeof p);
        if (!HsLpc(s, n, kTaps[t], &p)) continue;
        // the coefficients cost a little: 15 bits each, against about one bit a sample of the sums
        uint64_t c = HsCost(&p, s, n);
        if (c != ~0ull) c += (uint64_t)kTaps[t] * 15 / 2;
        if (c < best_cost) { best_cost = c; best = p; }
    }
    int order = best.order;
    if (best.lpc) {
        HsPut(o, (uint32_t)(order + 3), 5);
        HsPut(o, 15 - 1, 4);
        HsPut(o, (uint32_t)best.shift, 5);
        for (int j = 0; j < order; j++) HsPut(o, (uint32_t)best.q[j] & 0x7FFF, 15);
    } else {
        HsPut(o, (uint32_t)order, 5);
    }
    for (int i = 0; i < order && (uint32_t)i < n; i++) HsPut(o, (uint32_t)(s[i] + 32768), 16);
    for (uint32_t start = 0; start < n; start += HS_PART) {
        uint32_t end = start + HS_PART < n ? start + HS_PART : n;
        uint32_t from = start < (uint32_t)order ? (uint32_t)order : start;
        int m = 0;
        uint64_t sum = 0;
        for (uint32_t i = from; i < end; i++) {
            u[m] = HsZig(s[i] - HsPredict(&best, s, i));
            sum += u[m++];
        }
        int k = 0;
        while (k < 30 && ((uint64_t)m << (k + 1)) <= sum) k++;
        uint64_t c = m ? HsRiceCost(u, m, k) : 0, raw = (uint64_t)m * 20;
        if (k > 0) {
            uint64_t c2 = HsRiceCost(u, m, k - 1);
            if (c2 < c) { c = c2; k--; }
        }
        if (k < 30) {
            uint64_t c3 = HsRiceCost(u, m, k + 1);
            if (c3 < c) { c = c3; k++; }
        }
        if (c > raw) {
            HsPut(o, 31, 5);
            for (uint32_t i = from; i < end; i++) HsPut(o, (uint32_t)(s[i] - HsPredict(&best, s, i) + 524288), 20);
        } else {
            HsPut(o, (uint32_t)k, 5);
            for (int i = 0; i < m; i++) {
                HsUnaryPut(o, u[i] >> k);
                if (k) HsPut(o, u[i] & ((1u << k) - 1), k);
            }
        }
    }
    HsAlign(o);
    free(u);
}

// ---- one wave ---------------------------------------------------------------------------------------------------------
typedef struct {
    int denoise, treble;  // the steps (both on for the pack; the test tool can turn them off)
} HsSteps;

static uint32_t HsMult(uint32_t rate) {
    if (rate >= 32000) return 1;
    uint32_t m = (32000 + rate - 1) / rate;
    while (m * rate < 44100 && m < 8 && (m + 1) * rate <= 48000) m++;
    return m > 8 ? 8 : m;
}

// the enhanced wave as 16-bit samples, interleaved; -> HD frames, 0 on failure. flags gets HsHeader flags.
static uint32_t HsEnhance(const uint8_t* data, uint32_t bytes, uint32_t ch, uint32_t rate, int loops, HsSteps steps,
                          int16_t** pcm_out, uint32_t* mult_out, uint32_t* flags_out) {
    uint32_t blocks = bytes / (36 * ch), n = blocks * 64, m = HsMult(rate);
    if (!blocks) return 0;
    size_t pad = 4096;  // console samples of context on each side (a loop: its other end)
    size_t total = n + 2 * pad, hd_total = total * m;
    float* x = (float*)malloc(sizeof(float) * n);
    float* var = (float*)malloc(sizeof(float) * n);
    float* px = (float*)calloc(total, sizeof(float));
    float* pv = (float*)calloc(total, sizeof(float));
    float* py = (float*)malloc(sizeof(float) * total);
    float* hd = (float*)malloc(sizeof(float) * hd_total);
    int16_t* pcm = (int16_t*)malloc(sizeof(int16_t) * (size_t)n * m * ch);
    uint32_t flags = loops ? 1 : 0;
    if (!x || !var || !px || !pv || !py || !hd || !pcm) {
        free(x); free(var); free(px); free(pv); free(py); free(hd); free(pcm);
        return 0;
    }
    for (uint32_t c = 0; c < ch; c++) {
        HsDecode(data, blocks, ch, c, x, var);
        for (size_t i = 0; i < total; i++) {
            long long t = (long long)i - (long long)pad;
            if (loops) t = ((t % (long long)n) + n) % n;
            if (t >= 0 && t < (long long)n) {
                px[i] = x[t];
                pv[i] = var[t];
            } else {
                px[i] = 0;
                pv[i] = 0;
            }
        }
        if (steps.denoise) {
            HsDenoise(px, pv, py, total, rate);
            flags |= 4;
        } else {
            memcpy(py, px, sizeof(float) * total);
        }
        // the upsampler reads HS_TAPS around each sample: the pad covers them, the first and last HS_TAPS outputs of
        // the padded buffer are not used
        memset(hd, 0, sizeof(float) * hd_total);
        HsUpsample(py, total - 2 * HS_TAPS, m, hd + (size_t)HS_TAPS * m);
        if (steps.treble && m > 1 && rate >= 8000) {
            double top = HsContentTop(x, n, rate);
            if (top >= 0.7 * rate / 2 * 0.96 && HsTreble(hd, hd_total, rate * m, top)) flags |= 2;
        }
        for (size_t i = 0; i < (size_t)n * m; i++) {
            float v = hd[pad * m + i] * 32768.0f;
            int s = (int)lrintf(v);
            pcm[i * ch + c] = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
        }
    }
    free(x); free(var); free(px); free(pv); free(py); free(hd);
    *pcm_out = pcm;
    *mult_out = m;
    *flags_out = flags;
    return n * m;
}

// the file of one wave (format: hdsound_format.h) into out; -> 1
static int HsPack(const uint8_t* data, uint32_t bytes, uint32_t ch, uint32_t rate, uint32_t mult, uint32_t flags,
                  const int16_t* pcm, uint32_t frames, HsOut* out) {
    uint32_t blocks = bytes / (36 * ch), chunks = (frames + HS_CHUNK - 1) / HS_CHUNK;
    HsHeader h = {0};
    h.magic = HS_MAGIC;
    h.version = HS_VERSION;
    h.channels = ch;
    h.rate = rate;
    h.mult = mult;
    h.frames = frames;
    h.blocks = blocks;
    h.chunks = chunks;
    h.flags = flags;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < sizeof h; i++) HsByte(out, ((uint8_t*)&h)[i]);
    for (uint32_t k = 0; k < blocks; k++) {
        uint32_t c = HsCheck(data + (size_t)k * 36 * ch, 36 * ch);
        for (int i = 0; i < 4; i++) HsByte(out, (uint8_t)(c >> (8 * i)));
    }
    size_t table = out->size;
    for (uint32_t k = 0; k <= chunks; k++)
        for (int i = 0; i < 4; i++) HsByte(out, 0);
    int16_t* one = (int16_t*)malloc(sizeof(int16_t) * HS_CHUNK);
    for (uint32_t k = 0; k < chunks; k++) {
        uint32_t at = (uint32_t)out->size;
        memcpy(out->p + table + 4 * k, &at, 4);
        uint32_t first = k * HS_CHUNK, count = frames - first < HS_CHUNK ? frames - first : HS_CHUNK;
        for (uint32_t c = 0; c < ch; c++) {
            for (uint32_t i = 0; i < count; i++) one[i] = pcm[(size_t)(first + i) * ch + c];
            HsEncodeChannel(out, one, count);
        }
    }
    uint32_t end = (uint32_t)out->size;
    memcpy(out->p + table + 4 * chunks, &end, 4);
    free(one);
    return 1;
}

// ---- the whole set ----------------------------------------------------------------------------------------------------
typedef struct {
    HsDisc disc;
    HsWaves waves;
    wchar_t dir[MAX_PATH * 2];
    volatile LONG next, made, failed, done_bytes;
    volatile LONG* stop;
    int (*busy)(void);           // the game is running: slow down
    uint8_t** file_data;         // .sdu files, read once (shared, read only), by index
    CRITICAL_SECTION read_lock;
    char* state;                 // per wave: 0 to do, 1 made, 2 failed
    volatile LONG* file_left;    // per file: waves still to make from it (freed at 0)
    HsSteps steps;
} HsSet;

static void HsWavePath(const HsSet* s, uint64_t key, wchar_t* out, const wchar_t* ext) {
    _snwprintf(out, MAX_PATH * 2, L"%ls\\%016llX%ls", s->dir, (unsigned long long)key, ext);
}
static int HsMadeAlready(const HsSet* s, uint64_t key) {
    wchar_t path[MAX_PATH * 2];
    HsWavePath(s, key, path, L".hsw");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HsHeader h;
    DWORD got = 0;
    int ok = ReadFile(f, &h, sizeof h, &got, NULL) && got == sizeof h && h.magic == HS_MAGIC && h.version == HS_VERSION;
    CloseHandle(f);
    return ok;
}
static const uint8_t* HsFileData(HsSet* s, int file) {
    EnterCriticalSection(&s->read_lock);
    if (!s->file_data[file]) s->file_data[file] = HsDiscRead(&s->disc, &s->disc.files[file]);
    LeaveCriticalSection(&s->read_lock);
    return s->file_data[file];
}
static int HsMakeOne(HsSet* s, int i) {
    HsWave* w = &s->waves.w[i];
    const uint8_t* file = HsFileData(s, w->file);
    if (!file) return 0;
    const uint8_t* data = file + w->at;
    int16_t* pcm = NULL;
    uint32_t mult = 1, flags = 0;
    uint32_t frames = HsEnhance(data, w->bytes, w->channels, w->rate, (w->flags & 8) != 0, s->steps, &pcm, &mult, &flags);
    if (!frames) return 0;
    HsOut out;
    HsPack(data, w->bytes, w->channels, w->rate, mult, flags, pcm, frames, &out);
    free(pcm);
    wchar_t path[MAX_PATH * 2], temp[MAX_PATH * 2];
    HsWavePath(s, w->key, path, L".hsw");
    HsWavePath(s, w->key, temp, L".part");
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, out.p, (DWORD)out.size, &put, NULL) && put == out.size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(out.p);
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(temp);
    if (ok) InterlockedExchangeAdd(&s->done_bytes, (LONG)(put / 1024));
    return ok;
}
static DWORD WINAPI HsWorker(LPVOID p) {
    HsSet* s = (HsSet*)p;
    for (;;) {
        if (s->stop && *s->stop) return 0;
        LONG i = InterlockedIncrement(&s->next) - 1;
        if (i >= s->waves.count) return 0;
        if (s->state[i]) continue;  // made before (its file was never counted)
        DWORD t0 = GetTickCount();
        int ok = HsMakeOne(s, (int)i);
        s->state[i] = ok ? 1 : 2;
        int file = s->waves.w[i].file;
        if (InterlockedDecrement(&s->file_left[file]) == 0) {
            EnterCriticalSection(&s->read_lock);
            free(s->file_data[file]);
            s->file_data[file] = NULL;
            LeaveCriticalSection(&s->read_lock);
        }
        InterlockedIncrement(ok ? &s->made : &s->failed);
        if (!ok) HsLog(L"wave %016llX (%u ch, %u Hz, %u bytes) could not be made", (unsigned long long)s->waves.w[i].key,
                       s->waves.w[i].channels, s->waves.w[i].rate, s->waves.w[i].bytes);
        // while the game runs, take no more than a tenth of this processor
        if (s->busy && s->busy()) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
            DWORD took = GetTickCount() - t0;
            Sleep(took * 9 > 60000 ? 60000 : took * 9);
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
    }
}
static int HsCompare(const void* a, const void* b) {
    uint64_t x = ((const HsIndexEntry*)a)->first, y = ((const HsIndexEntry*)b)->first;
    return x < y ? -1 : x > y;
}
// the index of what is made so far (written whole, then put in place)
static void HsWriteIndex(HsSet* s) {
    int count = 0;
    for (int i = 0; i < s->waves.count; i++) count += s->state[i] == 1;
    HsIndexEntry* e = (HsIndexEntry*)calloc(count ? count : 1, sizeof(HsIndexEntry));
    int n = 0;
    for (int i = 0; i < s->waves.count; i++) {
        if (s->state[i] != 1) continue;
        HsWave* w = &s->waves.w[i];
        e[n].first = w->first;
        e[n].key = w->key;
        e[n].blocks = w->bytes / (36 * w->channels);
        e[n].channels = w->channels;
        e[n].rate = w->rate;
        n++;
    }
    qsort(e, n, sizeof *e, HsCompare);
    wchar_t path[MAX_PATH * 2], temp[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%ls\\index.bin", s->dir);
    _snwprintf(temp, MAX_PATH * 2, L"%ls\\index.part", s->dir);
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        uint32_t head[3] = {HS_INDEX_MAGIC, HS_VERSION, (uint32_t)n};
        DWORD put;
        int ok = WriteFile(f, head, sizeof head, &put, NULL) && WriteFile(f, e, (DWORD)(sizeof *e * n), &put, NULL);
        CloseHandle(f);
        if (ok) MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
        else DeleteFileW(temp);
    }
    free(e);
}
static void HsWriteText(const HsSet* s, const wchar_t* name, const char* text) {
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", s->dir, name);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(f, text, (DWORD)strlen(text), &put, NULL);
    CloseHandle(f);
}
static int HsGameRunning(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W p = {sizeof p};
    int found = 0;
    for (BOOL more = Process32FirstW(snap, &p); more && !found; more = Process32NextW(snap, &p))
        found = _wcsicmp(p.szExeFile, L"Suffering2.exe") == 0;
    CloseHandle(snap);
    return found;
}

// image: the disc image or its files; dir: Game Files\sound\hd; log: the log file (or NULL); stop: set to stop after
// the waves in hand; busy: NULL, or tells that the game runs. -> 1 when every wave is made
static int MakeHdSoundIn(const wchar_t* image, const wchar_t* dir, const wchar_t* log, volatile LONG* stop, int (*busy)(void),
                         int threads) {
    static int log_ready;
    if (!log_ready) { InitializeCriticalSection(&g_hs_log_lock); log_ready = 1; }
    if (log) wcsncpy(g_hs_log, log, MAX_PATH * 2 - 1);
    HsSet* s = (HsSet*)calloc(1, sizeof(HsSet));
    wcsncpy(s->dir, dir, MAX_PATH * 2 - 1);
    s->stop = stop;
    s->busy = busy;
    // ADPCM noise removal is not used: on the game's own waves it made them measurably further from the clean sound
    // (tools\hdsound_test.c noise: SNR down 1 to 4 dB on every wave tried), so only the treble step is on
    s->steps.denoise = 0;
    s->steps.treble = 1;
    CreateDirectoryW(dir, NULL);
    DWORD t0 = GetTickCount();
    if (!HsOpenDisc(&s->disc, image)) {
        HsLog(L"no game image in %ls", image);
        free(s);
        return 0;
    }
    InitializeCriticalSection(&s->read_lock);
    s->file_data = (uint8_t**)calloc(s->disc.count, sizeof(uint8_t*));
    // list the waves: every file is read once here and dropped (it is read again by the first wave that needs it)
    for (int i = 0; i < s->disc.count; i++) {
        uint8_t* d = HsDiscRead(&s->disc, &s->disc.files[i]);
        if (!d) continue;
        int before = s->waves.count;
        HsList(d, s->disc.files[i].size, i, &s->waves);
        (void)before;
        free(d);
    }
    // waves of 32 kHz and up have their treble already: the port plays them as they are (with its cubic)
    int kept = 0;
    for (int i = 0; i < s->waves.count; i++)
        if (HsMult(s->waves.w[i].rate) > 1) s->waves.w[kept++] = s->waves.w[i];
    s->waves.count = kept;
    s->state = (char*)calloc(s->waves.count ? s->waves.count : 1, 1);
    s->file_left = (volatile LONG*)calloc(s->disc.count, sizeof(LONG));
    int already = 0;
    for (int i = 0; i < s->waves.count; i++) {
        if (HsMadeAlready(s, s->waves.w[i].key)) { s->state[i] = 1; already++; }
        else s->file_left[s->waves.w[i].file]++;
    }
    HsLog(L"HD sound: %d waves in %d sound files, %d made before; %d to make", s->waves.count, s->disc.count, already,
          s->waves.count - already);
    char text[128];
    sprintf(text, "%d %d\n", already, s->waves.count);
    HsWriteText(s, L"progress.txt", text);
    // the waves go in file order, so few .sdu files are in memory at once; a file is dropped when its last wave is made
    if (threads <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        threads = (int)si.dwNumberOfProcessors - 1;
    }
    if (threads < 1) threads = 1;
    if (threads > 32) threads = 32;
    HANDLE th[32];
    for (int t = 0; t < threads; t++) {
        th[t] = CreateThread(NULL, 0, HsWorker, s, 0, NULL);
        SetThreadPriority(th[t], THREAD_PRIORITY_BELOW_NORMAL);
    }
    DWORD last_index = GetTickCount();
    for (;;) {
        DWORD r = WaitForMultipleObjects(threads, th, TRUE, 2000);
        sprintf(text, "%ld %d\n", already + s->made, s->waves.count);
        HsWriteText(s, L"progress.txt", text);
        if (GetTickCount() - last_index > 30000) {
            HsWriteIndex(s);
            last_index = GetTickCount();
        }
        if (r != WAIT_TIMEOUT) break;
    }
    for (int t = 0; t < threads; t++) CloseHandle(th[t]);
    HsWriteIndex(s);
    int whole = already + s->made == s->waves.count;
    DWORD took = (GetTickCount() - t0) / 1000;
    HsLog(L"HD sound: %ld made, %ld failed in %lu s (%ld MB written)%ls", s->made, s->failed, took, s->done_bytes / 1024,
          whole ? L"; the set is whole" : L"; it goes on next time");
    if (whole) {
        sprintf(text, "%d %d %ld\n", HS_VERSION, s->waves.count, s->failed);
        HsWriteText(s, L"made.txt", text);
    }
    for (int i = 0; i < s->disc.count; i++) free(s->file_data[i]);
    free(s->file_data);
    free(s->state);
    free((void*)s->file_left);
    free(s->waves.w);
    free(s->disc.files);
    if (s->disc.iso != INVALID_HANDLE_VALUE) CloseHandle(s->disc.iso);
    DeleteCriticalSection(&s->read_lock);
    free(s);
    return whole;
}
