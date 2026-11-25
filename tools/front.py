"""Brings the test game's window to the front as soon as it exists (the game a player starts is always the
foreground program, and Windows treats the foreground program's threads differently; a test started from a
background shell is not in front by itself).
usage: python front.py [--wait 6] [--after <old pid>]   waits for a test.pid different from --after, then its window"""
import ctypes, os, sys, time
from ctypes import wintypes as wt

rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
wait = float(sys.argv[sys.argv.index('--wait') + 1]) if '--wait' in sys.argv else 6.0
after = int(sys.argv[sys.argv.index('--after') + 1]) if '--after' in sys.argv else 0
u32 = ctypes.WinDLL('user32', use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

def pid_now():
    try: return int(open(os.path.join(rel, 'test.pid')).read().strip() or 0)
    except (OSError, ValueError): return 0

def window_of(pid):
    found = []
    def each(h, _):
        p = wt.DWORD(0)
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h): found.append(h)
        return True
    u32.EnumWindows(EnumProc(each), 0)
    return found[0] if found else None

end = time.time() + wait
pid = 0
while time.time() < end:
    pid = pid_now()
    if pid and pid != after: break
    pid = 0
    time.sleep(0.02)
if not pid: sys.exit('no new test game within %.0f s' % wait)
h = None
while time.time() < end and not h:
    h = window_of(pid)
    if not h: time.sleep(0.02)
if not h: sys.exit('test game %d showed no window within %.0f s' % (pid, wait))
# Windows lets a background program take the foreground right after a key event of its own: a tap of Alt.
u32.keybd_event(0x12, 0, 0, 0)
ok = u32.SetForegroundWindow(wt.HWND(h))
u32.keybd_event(0x12, 0, 2, 0)
time.sleep(0.2)
front = u32.GetForegroundWindow()
print('test game %d window %X: %s' % (pid, h, 'in front' if front == h else 'NOT in front (asked: %d)' % ok))
