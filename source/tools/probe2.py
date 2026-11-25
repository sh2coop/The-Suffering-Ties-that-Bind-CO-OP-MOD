"""One thread of a stuck game: where it runs (several samples) and the code addresses on its stack, with names.
Prints before each step so a hang names its step. Run under a time limit:  timeout 25 python probe2.py TID [pid]"""
import ctypes, os, struct, sys, time
from ctypes import wintypes as wt

rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
tid = int(sys.argv[1])
pid = int(sys.argv[2]) if len(sys.argv) > 2 else int(open(os.path.join(rel, 'test.pid')).read().strip())
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi')
dbg = ctypes.WinDLL('dbghelp')
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE

def say(*a):
    print(*a)
    sys.stdout.flush()

h = k32.OpenProcess(0x1F0FFF, False, pid)
say('process', pid, bool(h))

say('modules')
arr = (ctypes.c_void_p * 1024)()
need = wt.DWORD(0)
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
mods = []
if psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3):
    for i in range(need.value // 8):
        mi = MI()
        name = ctypes.create_unicode_buffer(520)
        if psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
            psapi.GetModuleFileNameExW(h, arr[i], name, 520)
            mods.append((mi.base or 0, mi.size, name.value))
say('modules', len(mods))
for b, s, n in mods:
    if os.path.basename(n).lower().startswith('suffering2'): say('  %016X %08X %s' % (b, s, os.path.basename(n)))

def module(a):
    for b, s, n in mods:
        if b <= a < b + s: return b, os.path.basename(n)
    return None

say('symbols')
dbg.SymSetOptions.argtypes = [wt.DWORD]
dbg.SymSetOptions(0x2 | 0x4 | 0x10)
dbg.SymInitializeW.argtypes = [wt.HANDLE, ctypes.c_wchar_p, wt.BOOL]
dbg.SymInitializeW(h, None, False)
dbg.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
for b, s, n in mods:
    dbg.SymLoadModuleExW(h, None, n, None, b, s, None, 0)

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG), ('Scope', wt.ULONG),
                ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_char * 600)]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]

def name(a):
    m = module(a)
    if not m: return None
    si = SYMBOL_INFO()
    si.SizeOfStruct = 88
    si.MaxNameLen = 590
    disp = ctypes.c_uint64(0)
    if dbg.SymFromAddr(h, a, ctypes.byref(disp), ctypes.byref(si)):
        return '%s!%s+0x%X' % (m[1], si.Name.decode(errors='replace'), disp.value)
    return '%s+0x%X' % (m[1], a - m[0])

th = k32.OpenThread(0x1FFFFF, False, tid)
say('thread', tid, bool(th))
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ctx = ctypes.create_string_buffer(1232 + 16)
ca = (ctypes.addressof(ctx) + 15) & ~15

def sample(stack):
    k32.SuspendThread(th)
    ctypes.memset(ca, 0, 1232)
    struct.pack_into('<I', (ctypes.c_char * 1232).from_address(ca), 0x30, 0x10000B)
    ok = k32.GetThreadContext(th, ctypes.c_void_p(ca))
    raw = ctypes.string_at(ca, 1232)
    rip = struct.unpack_from('<Q', raw, 0xF8)[0]
    rsp = struct.unpack_from('<Q', raw, 0x98)[0]
    data = b''
    if stack and ok:
        buf = ctypes.create_string_buffer(stack)
        n = ctypes.c_size_t(0)
        k32.ReadProcessMemory(h, ctypes.c_void_p(rsp), buf, stack, ctypes.byref(n))
        data = buf.raw[:n.value]
    k32.ResumeThread(th)
    return rip, rsp, data

say('samples of where it runs')
seen = {}
for i in range(40):
    rip, rsp, _ = sample(0)
    seen[rip] = seen.get(rip, 0) + 1
    time.sleep(0.01)
for rip, n in sorted(seen.items(), key=lambda x: -x[1])[:12]:
    say('  %3d  %016X  %s' % (n, rip, name(rip)))

say('stack')
rip, rsp, data = sample(0x3000)
say('  rip %016X %s' % (rip, name(rip)))
shown = 0
for off in range(0, len(data) - 7, 8):
    v = struct.unpack_from('<Q', data, off)[0]
    nm = name(v)
    if nm:
        say('  +%04X %016X %s' % (off, v, nm))
        shown += 1
        if shown >= 60: break
say('done')
