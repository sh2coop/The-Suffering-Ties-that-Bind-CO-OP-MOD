"""Makes the next start of the game a cold one: drops the disc image's pages from Windows' file cache, so the game's
reads go to the drive and block, as on the first start after a pause. The image is only opened for reading.
  python coldcache.py            drop the cache of out/build/release/assets/Suffering2.iso
  python coldcache.py --time     also time a 64 MB buffered read before and after (shows whether it worked)
"""
import ctypes, os, sys, time
from ctypes import wintypes as wt

rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
path = os.path.abspath(os.path.join(rel, 'assets', 'Suffering2.iso'))
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]

def read_time(offset=1 << 30, size=64 << 20):
    t = time.perf_counter()
    with open(path, 'rb', buffering=0) as f:
        f.seek(offset)
        left = size
        while left > 0:
            b = f.read(min(left, 1 << 20))
            if not b: break
            left -= len(b)
    return time.perf_counter() - t

timing = '--time' in sys.argv
if timing: print('64 MB read before: %.3f s' % read_time())
# A handle without buffering makes Windows give up the file's cached pages when no other handle has it open.
h = k32.CreateFileW(path, 0x80000000, 7, None, 3, 0x20000000, None)
if h == wt.HANDLE(-1).value or not h:
    sys.exit('cannot open %s (error %d)' % (path, ctypes.get_last_error()))
k32.CloseHandle(wt.HANDLE(h))
print('cache dropped for', os.path.basename(path))
if timing:
    print('64 MB read after:  %.3f s' % read_time())
    k32.CloseHandle(wt.HANDLE(k32.CreateFileW(path, 0x80000000, 7, None, 3, 0x20000000, None)))
