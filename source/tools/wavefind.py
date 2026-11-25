"""Is a wave of the game's sound file in a recording of a run (run.ps1 -Audio)? Decodes the wave (Xbox ADPCM, mono,
the header's rate) and looks for its waveform in the recording.
  python wavefind.py FILE.sdu ID(hex) RECORDING.wav [more recordings]"""
import os, struct, sys, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdu

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]

def decode(d):
    out = []
    for b in range(0, len(d) - 35, 36):
        pred, idx = struct.unpack_from('<hB', d, b)
        idx = min(idx, 88)
        out.append(pred)
        for byte in d[b + 4:b + 36]:
            for nib in (byte & 15, byte >> 4):
                step = STEP[idx]
                diff = step >> 3
                if nib & 1: diff += step >> 2
                if nib & 2: diff += step >> 1
                if nib & 4: diff += step
                pred = max(-32768, min(32767, pred - diff if nib & 8 else pred + diff))
                idx = max(0, min(88, idx + IDX[nib & 7]))
                out.append(pred)
    return np.array(out, dtype=np.float64)

def wave_of(path, ident):
    d = open(path, 'rb').read()
    w = sdu.directory(d)[ident]
    head = struct.unpack_from('<15I', d, w[0x302][0])
    off, size = w[0x304]
    return decode(d[off:off + size]), head[12]

if __name__ == '__main__':
    x, rate = wave_of(sys.argv[1], int(sys.argv[2], 16))
    print('wave: %.2f s at %d Hz, rms %.0f' % (len(x) / rate, rate, np.sqrt((x ** 2).mean())))
    for p in sys.argv[3:]:
        w = wave.open(p, 'rb'); sr = w.getframerate(); ch = w.getnchannels()
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64).reshape(-1, ch).mean(axis=1)
        r = np.interp(np.arange(0, len(x) - 1, rate / sr), np.arange(len(x)), x)[:int(0.5 * sr)]
        r = (r - r.mean()) / np.linalg.norm(r)
        size = 1 << (len(a) + len(r) - 1).bit_length()
        corr = np.fft.irfft(np.fft.rfft(a, size) * np.conj(np.fft.rfft(r, size)), size)[:len(a) - len(r)]
        c = np.concatenate(([0.0], np.cumsum(a ** 2)))
        score = np.abs(corr) / (np.sqrt(np.maximum(c[len(r):len(r) + len(corr)] - c[:len(corr)], 0)) + 1e-9)
        best = []
        s = score.copy()
        for k in range(3):
            i = int(np.argmax(s)); best.append('%.2f at %.2f s' % (s[i], i / sr)); s[max(0, i - sr // 4):i + sr // 4] = 0
        print('%s: %s (median %.3f)' % (p, ', '.join(best), float(np.sort(score)[len(score) // 2])))
