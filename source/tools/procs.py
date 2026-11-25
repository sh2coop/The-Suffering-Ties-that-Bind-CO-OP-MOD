"""The game's processes.
  python procs.py              lists them (and build tools) with thread counts and parents, from a Toolhelp snapshot
  python procs.py --stop PID   ends that process if it is Suffering2.exe. It opens the process by its id and never
                               walks the system's process list, which a game stuck in the kernel can make hang.
"""
import ctypes, sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE

if '--stop' in sys.argv:
    pid = int(sys.argv[sys.argv.index('--stop') + 1])
    h = k32.OpenProcess(0x0001 | 0x1000 | 0x00100000, False, pid)  # terminate, query limited, synchronize
    if not h:
        print('test game %d is not running' % pid)
        sys.exit(0)
    name = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, ctypes.c_wchar_p, ctypes.POINTER(wt.DWORD)]
    k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    if not k32.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(size)) or not name.value.lower().endswith('suffering2.exe'):
        print('process %d is not the game (%s): left alone' % (pid, name.value))
        sys.exit(0)
    if k32.WaitForSingleObject(h, 0) == 0:
        print('test game %d has already ended' % pid)
        sys.exit(0)
    k32.TerminateProcess(h, 1)
    gone = k32.WaitForSingleObject(h, 8000) == 0
    print('stopped test game %d' % pid if gone else 'test game %d was told to end but is still there after 8 s' % pid)
    sys.exit(0 if gone else 4)

class PE(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_char * 260)]

snap = k32.CreateToolhelp32Snapshot(2, 0)
e = PE()
e.dwSize = ctypes.sizeof(e)
ok = k32.Process32First(snap, ctypes.byref(e))
names = ('suffering2.exe', 'ninja.exe', 'cmake.exe', 'clang++.exe', 'clang.exe', 'xbe_recomp.exe')
while ok:
    n = e.szExeFile.decode(errors='replace').lower()
    if n in names:
        print('%-16s pid %6d threads %3d parent %6d' % (n, e.th32ProcessID, e.cntThreads, e.th32ParentProcessID))
    ok = k32.Process32Next(snap, ctypes.byref(e))
k32.CloseHandle(snap)
print('done')
