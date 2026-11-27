// Test tool for the HD sound maker (tools\launcher\hdsound.h), not shipped.
//   hdsound_test make IMAGE DIR [threads]          the whole set, as Build Recomp makes it (log DIR\hd_sound.log)
//   hdsound_test list IMAGE                        every wave: number, key, channels, rate, seconds, loops, file
//   hdsound_test pair IMAGE DIR NUMBER OUT [steps] OUT_before.wav (what the port plays now: the ADPCM wave read with
//                                                  straight lines to 48 kHz) and OUT_after.wav (the HD wave from DIR
//                                                  read with the port's cubic), both at the wave's own pitch.
//                                                  With steps (d, t, dt or -) the HD wave is made here instead of read.
//   hdsound_test check DIR                         decodes every .hsw file and checks its index
// Build: tools\dev.bat clang-cl /O2 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS tools\hdsound_test.c /Fe...
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "launcher/hdsound.h"

static void WriteWav(const wchar_t* path, const float* lr, size_t frames) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    uint32_t rate = 48000, data = (uint32_t)(frames * 4), riff = 36 + data, fmt = 16, bytes = rate * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bytes, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (size_t i = 0; i < frames * 2; i++) {
        float v = lr[i] * 32768.0f;
        int s = (int)lrintf(v);
        int16_t o = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
        fwrite(&o, 2, 1, f);
    }
    fclose(f);
}

static HsSet* Listed(const wchar_t* image) {
    static int ready;
    if (!ready) { InitializeCriticalSection(&g_hs_log_lock); ready = 1; }
    HsSet* s = (HsSet*)calloc(1, sizeof(HsSet));
    if (!HsOpenDisc(&s->disc, image)) { fwprintf(stderr, L"no image\n"); exit(1); }
    for (int i = 0; i < s->disc.count; i++) {
        uint8_t* d = HsDiscRead(&s->disc, &s->disc.files[i]);
        if (!d) continue;
        HsList(d, s->disc.files[i].size, i, &s->waves);
        free(d);
    }
    InitializeCriticalSection(&s->read_lock);
    s->file_data = (uint8_t**)calloc(s->disc.count, sizeof(uint8_t*));
    return s;
}

// a whole .hsw file -> interleaved samples (caller frees), header in h
static int16_t* ReadHsw(const wchar_t* path, HsHeader* h) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(size);
    fread(b, 1, size, f);
    fclose(f);
    memcpy(h, b, sizeof *h);
    if (h->magic != HS_MAGIC) { free(b); return NULL; }
    const uint32_t* at = (const uint32_t*)(b + sizeof *h + 4 * h->blocks);
    int16_t* pcm = (int16_t*)malloc(sizeof(int16_t) * (size_t)h->frames * h->channels + 16);
    int16_t scratch[HS_CHUNK];
    for (uint32_t k = 0; k < h->chunks; k++) {
        uint32_t first = k * HS_CHUNK, n = h->frames - first < HS_CHUNK ? h->frames - first : HS_CHUNK;
        if (!HsDecodeChunk(b + at[k], at[k + 1] - at[k], n, h->channels, pcm + (size_t)first * h->channels, scratch)) {
            fwprintf(stderr, L"chunk %u broken\n", k);
            free(pcm); free(b);
            return NULL;
        }
    }
    free(b);
    return pcm;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { fwprintf(stderr, L"see the top of hdsound_test.c\n"); return 2; }
    if (!wcscmp(argv[1], L"make") && argc >= 4) {
        wchar_t log[MAX_PATH * 2];
        CreateDirectoryW(argv[3], NULL);
        _snwprintf(log, MAX_PATH * 2, L"%ls\\hd_sound.log", argv[3]);
        int threads = argc > 4 ? _wtoi(argv[4]) : 0;
        DWORD t0 = GetTickCount();
        int whole = MakeHdSoundIn(argv[2], argv[3], log, NULL, argc > 5 ? NULL : HsGameRunning, threads);  // a 5th word: never slow down
        wprintf(L"whole %d, %lu s\n", whole, (GetTickCount() - t0) / 1000);
        return whole ? 0 : 1;
    }
    if (!wcscmp(argv[1], L"list")) {
        HsSet* s = Listed(argv[2]);
        for (int i = 0; i < s->waves.count; i++) {
            HsWave* w = &s->waves.w[i];
            double secs = (double)w->bytes / (36 * w->channels) * 64 / w->rate;
            wprintf(L"%5d %016llX %u %6u %8.2f %d %ls\n", i, (unsigned long long)w->key, w->channels, w->rate, secs,
                    (w->flags & 8) != 0, s->disc.files[w->file].name);
        }
        return 0;
    }
    if (!wcscmp(argv[1], L"pair") && argc >= 6) {
        HsSet* s = Listed(argv[2]);
        int i = _wtoi(argv[4]);
        if (i < 0 || i >= s->waves.count) return 1;
        HsWave* w = &s->waves.w[i];
        const uint8_t* data = HsFileData(s, w->file) + w->at;
        uint32_t ch = w->channels, blocks = w->bytes / (36 * ch), n = blocks * 64;
        // before: as the port plays it
        float* x[2];
        float* var = (float*)malloc(sizeof(float) * n);
        for (uint32_t c = 0; c < ch; c++) {
            x[c] = (float*)malloc(sizeof(float) * n);
            HsDecode(data, blocks, ch, c, x[c], var);
        }
        size_t out_n = (size_t)((double)n * 48000 / w->rate);
        float* lr = (float*)malloc(sizeof(float) * 2 * out_n);
        double step = (double)w->rate / 48000;
        for (size_t o = 0; o < out_n; o++) {
            double p = o * step;
            uint32_t f = (uint32_t)p;
            float t = (float)(p - f);
            for (int c = 0; c < 2; c++) {
                const float* s0 = x[c < (int)ch ? c : 0];
                float a = s0[f], b = f + 1 < n ? s0[f + 1] : 0;
                lr[o * 2 + c] = a + (b - a) * t;
            }
        }
        wchar_t path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2, L"%ls_before.wav", argv[5]);
        WriteWav(path, lr, out_n);
        // after
        HsHeader h = {0};
        int16_t* pcm = NULL;
        if (argc > 6) {
            HsSteps st = {wcschr(argv[6], L'd') != NULL, wcschr(argv[6], L't') != NULL};
            uint32_t mult, flags;
            h.frames = HsEnhance(data, w->bytes, ch, w->rate, (w->flags & 8) != 0, st, &pcm, &mult, &flags);
            h.mult = mult;
            h.channels = ch;
            h.flags = flags;
        } else {
            _snwprintf(path, MAX_PATH * 2, L"%ls\\%016llX.hsw", argv[3], (unsigned long long)w->key);
            pcm = ReadHsw(path, &h);
        }
        if (!pcm) { fwprintf(stderr, L"no HD wave\n"); return 1; }
        double hstep = step * h.mult;
        for (size_t o = 0; o < out_n; o++) {
            double p = o * hstep;
            long long a = (long long)p;
            float t = (float)(p - a);
            for (int c = 0; c < 2; c++) {
                int cc = c < (int)ch ? c : 0;
                float v[4];
                for (int j = 0; j < 4; j++) {
                    long long k = a - 1 + j;
                    if (k < 0) k = 0;
                    if (k >= h.frames) k = h.frames - 1;
                    v[j] = pcm[k * ch + cc] / 32768.0f;
                }
                lr[o * 2 + c] = HsCubic(v[0], v[1], v[2], v[3], t);
            }
        }
        _snwprintf(path, MAX_PATH * 2, L"%ls_after.wav", argv[5]);
        WriteWav(path, lr, out_n);
        wprintf(L"wave %d: %u ch %u Hz x%u, %.2f s, flags %u, content top %.0f Hz\n", i, ch, w->rate, h.mult,
                (double)n / w->rate, h.flags, HsContentTop(x[0], n, w->rate));
        return 0;
    }
    // bwe IMAGE NUMBER OUT: a 44.1 kHz wave taken down to 22.05 kHz and brought back with and without the treble step,
    // OUT_orig.wav / OUT_plain.wav / OUT_treble.wav (mono, 44.1 kHz, written as 48 kHz files: the levels are what count)
    if (!wcscmp(argv[1], L"bwe") && argc >= 5) {
        HsSet* s = Listed(argv[2]);
        HsWave* w = &s->waves.w[_wtoi(argv[3])];
        const uint8_t* data = HsFileData(s, w->file) + w->at;
        uint32_t ch = w->channels, blocks = w->bytes / (36 * ch), n = blocks * 64, half = n / 2;
        float* x = (float*)malloc(sizeof(float) * n);
        float* var = (float*)malloc(sizeof(float) * n);
        HsDecode(data, blocks, ch, 0, x, var);
        // down: windowed sinc at 0.96 of the new top
        size_t pad = 4096;
        float* lo = (float*)calloc(half + 2 * pad, sizeof(float));
        for (uint32_t i = 0; i < half; i++) {
            double acc = 0, sum = 0;
            for (int j = -64; j <= 64; j++) {
                long long t = 2LL * i + j;
                double fc = 0.24, tt = j, sv = tt == 0 ? 2 * fc : sin(2 * HS_PI * fc * tt) / (HS_PI * tt);
                double r = tt / 65.0, kw = HsBessel0(9 * sqrt(1 - r * r)) / HsBessel0(9);
                sum += sv * kw;
                if (t >= 0 && t < n) acc += x[t] * sv * kw;
            }
            lo[pad + i] = (float)(acc / sum);
        }
        size_t total = half + 2 * pad;
        float* up = (float*)calloc(total * 2, sizeof(float));
        HsUpsample(lo, total - 2 * HS_TAPS, 2, up + HS_TAPS * 2);
        float* plain = (float*)malloc(sizeof(float) * total * 2);
        memcpy(plain, up, sizeof(float) * total * 2);
        double top = HsContentTop(lo + pad, half, w->rate / 2);
        int added = HsTreble(up, total * 2, w->rate, top);
        float* lr = (float*)malloc(sizeof(float) * 2 * n);
        const wchar_t* names[3] = {L"orig", L"plain", L"treble"};
        for (int k = 0; k < 3; k++) {
            for (uint32_t i = 0; i < 2 * half; i++) {
                float v = k == 0 ? x[i] : k == 1 ? plain[pad * 2 + i] : up[pad * 2 + i];
                lr[2 * i] = lr[2 * i + 1] = v;
            }
            wchar_t path[MAX_PATH * 2];
            _snwprintf(path, MAX_PATH * 2, L"%ls_%ls.wav", argv[4], names[k]);
            WriteWav(path, lr, 2 * half);
        }
        wprintf(L"top %.0f Hz, added %d\n", top, added);
        return 0;
    }
    // noise IMAGE NUMBER: the wave as decoded is taken as clean, coded to ADPCM again here (as the console's encoder:
    // each nibble the nearest), decoded and cleaned; prints how far each is from the clean one
    if (!wcscmp(argv[1], L"noise") && argc >= 4) {
        HsSet* s = Listed(argv[2]);
        HsWave* w = &s->waves.w[_wtoi(argv[3])];
        const uint8_t* data = HsFileData(s, w->file) + w->at;
        uint32_t ch = w->channels, blocks = w->bytes / (36 * ch), n = blocks * 64;
        float* x = (float*)malloc(sizeof(float) * n);
        float* var = (float*)malloc(sizeof(float) * n);
        HsDecode(data, blocks, ch, 0, x, var);
        // code mono blocks again: header sample, 63 nibbles (the 64th nibble is never played)
        uint8_t* again = (uint8_t*)calloc(blocks, 36);
        int idx = 0;
        for (uint32_t k = 0; k < blocks; k++) {
            uint8_t* b = again + k * 36;
            int pred = (int)lrintf(x[k * 64] * 32768.0f);
            pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
            b[0] = (uint8_t)pred; b[1] = (uint8_t)(pred >> 8); b[2] = (uint8_t)idx;
            for (int j = 0; j < 64; j++) {
                int target = j < 63 ? (int)lrintf(x[k * 64 + 1 + j] * 32768.0f) : pred;
                int best = 0, best_err = 1 << 30, best_pred = pred;
                for (int nib = 0; nib < 16; nib++) {
                    int step = kHsStep[idx], diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    int p = nib & 8 ? pred - diff : pred + diff;
                    p = p < -32768 ? -32768 : p > 32767 ? 32767 : p;
                    int e = abs(p - target);
                    if (e < best_err) { best_err = e; best = nib; best_pred = p; }
                }
                pred = best_pred;
                idx += kHsIndex[best];
                idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                b[4 + j / 2] |= (uint8_t)(best << ((j & 1) * 4));
            }
        }
        float* y = (float*)malloc(sizeof(float) * n);
        float* v2 = (float*)malloc(sizeof(float) * n);
        HsDecode(again, blocks, 1, 0, y, v2);
        float* z = (float*)malloc(sizeof(float) * n);
        { double me = 0, mv = 0; for (uint32_t i = 0; i < n; i++) { me += (double)(y[i] - x[i]) * (y[i] - x[i]); mv += v2[i]; }
          wprintf(L"error power / model %.3f ", me / mv); }
        float scale = argc > 4 ? (float)_wtof(argv[4]) : 1.0f;
        for (uint32_t i = 0; i < n; i++) v2[i] *= scale;
        HsDenoise(y, v2, z, n, w->rate);
        double e1 = 0, e2 = 0, sx = 0, q1 = 0, q2 = 0, qx = 0;
        for (uint32_t i = 0; i < n; i++) {
            double a = y[i] - x[i], b = z[i] - x[i];
            e1 += a * a; e2 += b * b; sx += (double)x[i] * x[i];
        }
        // segmental: 10 ms pieces, mean of the SNRs (quiet pieces count as much as loud ones)
        int seg = (int)(w->rate / 100), m = 0;
        double s1 = 0, s2 = 0;
        for (uint32_t i = 0; i + seg <= n; i += seg) {
            double ex = 0, a1 = 0, a2 = 0;
            for (int j = 0; j < seg; j++) {
                ex += (double)x[i + j] * x[i + j];
                a1 += (double)(y[i + j] - x[i + j]) * (y[i + j] - x[i + j]);
                a2 += (double)(z[i + j] - x[i + j]) * (z[i + j] - x[i + j]);
            }
            if (ex < 1e-9 * seg) continue;
            double r1 = 10 * log10(ex / (a1 + 1e-20)), r2 = 10 * log10(ex / (a2 + 1e-20));
            s1 += r1 < -10 ? -10 : r1 > 50 ? 50 : r1;
            s2 += r2 < -10 ? -10 : r2 > 50 ? 50 : r2;
            m++;
        }
        (void)q1; (void)q2; (void)qx;
        wprintf(L"wave %d (%u Hz): SNR adpcm %.2f dB, cleaned %.2f dB; segmental %.2f -> %.2f dB\n", _wtoi(argv[3]), w->rate,
                10 * log10(sx / e1), 10 * log10(sx / e2), m ? s1 / m : 0, m ? s2 / m : 0);
        return 0;
    }
    // block IMAGE KEY...: which waves hold a block of that HsKey (as the game's log names a block), and where
    if (!wcscmp(argv[1], L"block") && argc >= 4) {
        HsSet* s = Listed(argv[2]);
        for (int i = 0; i < s->waves.count; i++) {
            HsWave* w = &s->waves.w[i];
            const uint8_t* data = HsFileData(s, w->file) + w->at;
            uint32_t bs = 36 * w->channels, blocks = w->bytes / bs;
            for (uint32_t k = 0; k < blocks; k++) {
                uint64_t key = HsKey(data + (size_t)k * bs, bs);
                for (int a = 3; a < argc; a++)
                    if (key == wcstoull(argv[a], NULL, 16))
                        wprintf(L"%ls: wave %d (%u ch %u Hz, %u blocks) block %u\n", argv[a], i, w->channels, w->rate, blocks, k);
            }
        }
        return 0;
    }
    // levels IMAGE DIR: every copy against its console wave: loudness and likeness (the HD wave taken at the console's
    // frames); prints the ones that differ
    if (!wcscmp(argv[1], L"levels") && argc >= 4) {
        HsSet* s = Listed(argv[2]);
        int bad = 0, seen = 0;
        for (int i = 0; i < s->waves.count; i++) {
            HsWave* w = &s->waves.w[i];
            wchar_t path[MAX_PATH * 2];
            _snwprintf(path, MAX_PATH * 2, L"%ls\\%016llX.hsw", argv[3], (unsigned long long)w->key);
            HsHeader h;
            int16_t* pcm = ReadHsw(path, &h);
            if (!pcm) continue;
            seen++;
            const uint8_t* data = HsFileData(s, w->file) + w->at;
            uint32_t ch = w->channels, blocks = w->bytes / (36 * ch), n = blocks * 64;
            float* x = (float*)malloc(sizeof(float) * n);
            float* var = (float*)malloc(sizeof(float) * n);
            for (uint32_t c = 0; c < ch; c++) {
                HsDecode(data, blocks, ch, c, x, var);
                double ex = 0, ey = 0, exy = 0;
                for (uint32_t f = 0; f < n; f++) {
                    double a = x[f], b = (uint64_t)f * h.mult < h.frames ? pcm[((size_t)f * h.mult) * ch + c] / 32768.0 : 0;
                    ex += a * a; ey += b * b; exy += a * b;
                }
                double db = 10 * log10((ey + 1e-12) / (ex + 1e-12)), corr = exy / sqrt(ex * ey + 1e-30);
                if (ex > 1e-6 * n && (fabs(db) > 1.5 || corr < 0.9)) {
                    bad++;
                    wprintf(L"%5d %016llX ch%u %u Hz x%u %.2f s: level %+.1f dB, likeness %.3f, flags %u\n", i,
                            (unsigned long long)w->key, c, w->rate, h.mult, (double)n / w->rate, db, corr, h.flags);
                }
            }
            free(x); free(var); free(pcm);
        }
        wprintf(L"%d copies read, %d channels differ\n", seen, bad);
        return 0;
    }
    if (!wcscmp(argv[1], L"check")) {
        wchar_t pattern[MAX_PATH * 2], path[MAX_PATH * 2];
        _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*.hsw", argv[2]);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(pattern, &fd);
        int ok = 0, bad = 0;
        if (fh != INVALID_HANDLE_VALUE) do {
            _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", argv[2], fd.cFileName);
            HsHeader h;
            int16_t* p = ReadHsw(path, &h);
            if (p) ok++; else { bad++; fwprintf(stderr, L"bad %ls\n", fd.cFileName); }
            free(p);
        } while (FindNextFileW(fh, &fd));
        wprintf(L"%d decode, %d broken\n", ok, bad);
        return bad != 0;
    }
    return 2;
}
