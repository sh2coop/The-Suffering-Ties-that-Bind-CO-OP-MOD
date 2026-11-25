"""Probes a game process that tools cannot read: tries one query at a time, printing before each, so the last line
printed names the query that hangs. Opens the process by id only.
usage: python probe.py [pid]   (default: out/build/release/test.pid)"""
import ctypes, os, struct, sys
from ctypes import wintypes as wt

rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
pid = int(sys.argv[1]) if len(sys.argv) > 1 else int(open(os.path.join(rel, 'test.pid')).read().strip())
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
ntdll = ctypes.WinDLL('ntdll')
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE

def say(*a):
    print(*a)
    sys.stdout.flush()

say('pid', pid)
h = k32.OpenProcess(0x1F0FFF, False, pid)
say('open process', bool(h))
code = wt.DWORD(0)
k32.GetExitCodeProcess(h, ctypes.byref(code))
say('exit code', code.value, '(259 = running)')

# threads through the process's own list
ntdll.NtGetNextThread.argtypes = [wt.HANDLE, wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
k32.GetThreadId.argtypes = [wt.HANDLE]
threads = []
prev = wt.HANDLE(0)
say('walking threads')
while True:
    nxt = wt.HANDLE(0)
    if ntdll.NtGetNextThread(h, prev, 0x1FFFFF, 0, 0, ctypes.byref(nxt)) != 0: break
    threads.append((k32.GetThreadId(nxt), nxt.value))
    prev = nxt
say('threads', len(threads), [t for t, _ in threads])

# each thread: times and where its user-mode code stands (no suspend, no memory read)
class FILETIME(ctypes.Structure):
    _fields_ = [('lo', wt.DWORD), ('hi', wt.DWORD)]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(FILETIME)] * 4
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadPriority.argtypes = [wt.HANDLE]
ctx = ctypes.create_string_buffer(1232 + 16)
ca = (ctypes.addressof(ctx) + 15) & ~15
for tid, th in threads:
    c, e, kt, ut = FILETIME(), FILETIME(), FILETIME(), FILETIME()
    k32.GetThreadTimes(th, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    kernel_ms = ((kt.hi << 32) | kt.lo) / 10000.0
    user_ms = ((ut.hi << 32) | ut.lo) / 10000.0
    ctypes.memset(ca, 0, 1232)
    struct.pack_into('<I', (ctypes.c_char * 1232).from_address(ca), 0x30, 0x10000B)
    ok = k32.GetThreadContext(th, ctypes.c_void_p(ca))
    raw = ctypes.string_at(ca, 1232)
    rip = struct.unpack_from('<Q', raw, 0xF8)[0]
    rsp = struct.unpack_from('<Q', raw, 0x98)[0]
    say('thread %6d priority %3d kernel %9.1f ms user %9.1f ms rip %016X rsp %016X' % (tid, k32.GetThreadPriority(th), kernel_ms, user_ms, rip if ok else 0, rsp if ok else 0))

say('reading 8 bytes of the process at its exe base (ReadProcessMemory)')
buf = ctypes.create_string_buffer(8)
n = ctypes.c_size_t(0)
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ok = k32.ReadProcessMemory(h, ctypes.c_void_p(0x7FF000000000), buf, 8, ctypes.byref(n))
say('read returned', ok, 'error', ctypes.get_last_error())
say('done')
