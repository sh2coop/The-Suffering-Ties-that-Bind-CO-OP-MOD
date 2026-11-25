"""Where is the running game? Suspends each thread of Suffering2.exe for a moment, reads its instruction pointer and
stack, and names every address that lies in the recompiled game code (Suffering2.dll) by its exported function
(sub_XXXXXXXX is the guest address of the function). The stack is scanned, not unwound, so a list may hold stale
entries; the first ones are the most reliable.
usage: python sample.py [--depth 24] [--samples 3] [--all]
Run it under a time limit (timeout 60 python sample.py ...): it stops each thread for its stack, and a thread that
never gets the processor makes that wait. For a game that is stuck, look with threads.py first: it only asks Windows.
"""
import ctypes, os, struct, sys, time
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
depth = int(sys.argv[sys.argv.index('--depth') + 1]) if '--depth' in sys.argv else 24
samples = int(sys.argv[sys.argv.index('--samples') + 1]) if '--samples' in sys.argv else 3
show_all = '--all' in sys.argv
rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 2, 4, 8, 0x10

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_char * 260)]

class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('GlblcntUsage', wt.DWORD), ('ProccntUsage', wt.DWORD), ('modBaseAddr', ctypes.c_void_p),
                ('modBaseSize', wt.DWORD), ('hModule', wt.HMODULE), ('szModule', ctypes.c_char * 256),
                ('szExePath', ctypes.c_char * 260)]

k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]

def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32First(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.decode(errors='replace').lower() == name.lower(): pid = e.th32ProcessID
        ok = k32.Process32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid

def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    e = MODULEENTRY32(); e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Module32First(snap, ctypes.byref(e))
    while ok:
        out.append((e.modBaseAddr or 0, e.modBaseSize, e.szModule.decode(errors='replace')))
        ok = k32.Module32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out

def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32(); e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid: out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out

def exports(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    opt = pe + 24
    ddir = opt + 112
    exp_rva = struct.unpack_from('<I', b, ddir)[0]
    secs = []
    so = opt + struct.unpack_from('<H', b, pe + 20)[0]
    for n in range(nsec):
        vsize, va, rsize, raw = struct.unpack_from('<IIII', b, so + n * 40 + 8)
        secs.append((va, max(vsize, rsize), raw))
    def off(r):
        for va, sz, raw in secs:
            if va <= r < va + sz: return raw + r - va
        raise ValueError(hex(r))
    e = off(exp_rva)
    nname = struct.unpack_from('<I', b, e + 24)[0]
    afunc, aname, aord = struct.unpack_from('<III', b, e + 28)
    out = []
    for n in range(nname):
        nr = struct.unpack_from('<I', b, off(aname) + n * 4)[0]
        o = struct.unpack_from('<H', b, off(aord) + n * 2)[0]
        fr = struct.unpack_from('<I', b, off(afunc) + o * 4)[0]
        s = off(nr)
        nm = b[s: b.index(b'\0', s)].decode()
        if nm.startswith('__imp__sub_') or nm.startswith('sub_'): out.append((fr, nm.replace('__imp__', '')))
    out.sort()
    return out

# The test's game is named by out/build/release/test.pid; --pid N names another. The system-wide lists are only a
# fallback: a game stuck in the kernel can make them hang.
pid = None
if '--pid' in sys.argv: pid = int(sys.argv[sys.argv.index('--pid') + 1])
elif os.path.exists(os.path.join(rel, 'test.pid')): pid = int(open(os.path.join(rel, 'test.pid')).read().strip() or 0)
if not pid: pid = find_pid('Suffering2.exe')
if not pid: sys.exit('Suffering2.exe is not running')
hproc = k32.OpenProcess(0x1F0FFF, False, pid)
if not hproc: sys.exit('cannot open process %d' % pid)

psapi = ctypes.WinDLL('psapi')
ntdll = ctypes.WinDLL('ntdll')

def modules(pid):
    arr = (wt.HMODULE * 1024)()
    need = wt.DWORD(0)
    psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
    if not psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(need), 3): return []
    out = []
    class MI(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]
    psapi.GetModuleInformation.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.c_void_p, wt.DWORD]
    psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.c_wchar_p, wt.DWORD]
    for i in range(need.value // ctypes.sizeof(wt.HMODULE)):
        mi = MI()
        name = ctypes.create_unicode_buffer(260)
        if psapi.GetModuleInformation(hproc, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
            psapi.GetModuleBaseNameW(hproc, arr[i], name, 260)
            out.append((mi.base or 0, mi.size, name.value))
    return out

def threads(pid):
    # NtGetNextThread walks the process's own thread list
    out = []
    prev = wt.HANDLE(0)
    ntdll.NtGetNextThread.argtypes = [wt.HANDLE, wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
    k32.GetThreadId.argtypes = [wt.HANDLE]
    while True:
        nxt = wt.HANDLE(0)
        if ntdll.NtGetNextThread(hproc, prev, 0x0002 | 0x0008 | 0x0040 | 0x0800, 0, 0, ctypes.byref(nxt)) != 0: break
        out.append(k32.GetThreadId(nxt))
        if prev.value: k32.CloseHandle(prev)
        prev = nxt
    if prev.value: k32.CloseHandle(prev)
    return out

mods = modules(pid)
dll = next((m for m in mods if m[2].lower() == 'suffering2.dll'), None)
if not dll: sys.exit('Suffering2.dll is not loaded (%d modules seen)' % len(mods))
ex = exports(os.path.join(rel, 'Suffering2.dll'))
rvas = [e[0] for e in ex]
import bisect

def name(addr):
    r = addr - dll[0]
    i = bisect.bisect_right(rvas, r) - 1
    if i < 0: return None
    return '%s+%X' % (ex[i][1], r - ex[i][0])

def module_of(addr):
    for base, size, nm in mods:
        if base <= addr < base + size: return '%s+%X' % (nm, addr - base)
    return '%X' % addr

hp = hproc

# the runtime's own functions (Suffering2.exe), named from its PDB
exe = next((m for m in mods if m[2].lower() == 'suffering2.exe'), None)
dbg = None
try:
    dbg = ctypes.WinDLL('dbghelp')
    dbg.SymSetOptions(0x00000002 | 0x00000004 | 0x00000010)  # undecorate, deferred loads, load lines
    dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
    if not dbg.SymInitialize(hp, rel.encode(), True): dbg = None
except OSError:
    dbg = None

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_ulonglong * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_ulonglong), ('Flags', wt.ULONG),
                ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 256)]

def exe_name(addr):
    if not dbg or not exe or not (exe[0] <= addr < exe[0] + exe[1]): return None
    si = SYMBOL_INFO(); si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 256; si.MaxNameLen = 255
    disp = ctypes.c_ulonglong(0)
    dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
    if dbg.SymFromAddr(hp, addr, ctypes.byref(disp), ctypes.byref(si)):
        return 'exe:%s+%X' % (si.Name.decode(errors='replace'), disp.value)
    return None
ctx = ctypes.create_string_buffer(1232 + 16)
ctx_addr = (ctypes.addressof(ctx) + 15) & ~15
for sample in range(samples):
    print('--- sample %d' % (sample + 1))
    for tid in threads(pid):
        ht = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)
        if not ht: continue
        if k32.SuspendThread(ht) == 0xFFFFFFFF:
            k32.CloseHandle(ht); continue
        ctypes.memset(ctx_addr, 0, 1232)
        struct.pack_into('<I', (ctypes.c_char * 1232).from_address(ctx_addr), 0x30, 0x10000B)
        got = k32.GetThreadContext(ht, ctypes.c_void_p(ctx_addr))
        raw = ctypes.string_at(ctx_addr, 1232)
        rsp, rip = struct.unpack_from('<Q', raw, 0x98)[0], struct.unpack_from('<Q', raw, 0xF8)[0]
        stack = ctypes.create_string_buffer(0x8000)
        n = ctypes.c_size_t(0)
        k32.ReadProcessMemory(hp, ctypes.c_void_p(rsp), stack, len(stack), ctypes.byref(n))
        k32.ResumeThread(ht)
        k32.CloseHandle(ht)
        if not got: continue
        frames = []
        if dll[0] <= rip < dll[0] + dll[1]: frames.append(name(rip))
        elif exe_name(rip): frames.append(exe_name(rip))
        for i in range(0, n.value - 7, 8):
            v = struct.unpack_from('<Q', stack.raw, i)[0]
            nm = name(v) if dll[0] <= v < dll[0] + dll[1] else exe_name(v)
            if nm and (not frames or frames[-1].split('+')[0] != nm.split('+')[0]): frames.append(nm)
        if frames or show_all:
            print('thread %d at %s' % (tid, module_of(rip)))
            for f in frames[:depth]: print('    ' + f)
    time.sleep(0.15)
