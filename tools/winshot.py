"""A picture of the game's own window as the player sees it (the frame as shown, with what the runtime draws over
it), not the 640 x 480 frame a CAPTURE writes. Waits, finds the window of the test game (test.pid in the release
folder, or the process given) and saves its client area. The window is asked to draw itself into a bitmap
(PrintWindow with the full-content flag), so it need not be in front and nothing else on the screen is taken;
only when it draws itself black and is the window in front, its place on the screen is taken instead.
usage: python winshot.py OUT.png [seconds to wait first=0] [process id (default: the one in test.pid)]"""
import ctypes, os, sys, time
from ctypes import wintypes
from PIL import Image

out = sys.argv[1]
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 0
rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
user32.SetProcessDPIAware()
time.sleep(wait)
try:
    pid = int(sys.argv[3]) if len(sys.argv) > 3 else int(open(os.path.join(rel, 'test.pid')).read().strip())
except Exception:
    print('no test game (test.pid)')
    sys.exit(2)

found = []
@ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def each(hwnd, _):
    p = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    if p.value == pid and user32.IsWindowVisible(hwnd):
        r = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(r))
        if r.right >= 320 and r.bottom >= 240: found.append((r.right * r.bottom, hwnd, r.right, r.bottom))
    return True
user32.EnumWindows(each, 0)
if not found:
    print('no window of process', pid)
    sys.exit(2)
_, hwnd, w, h = max(found)

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG), ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD), ('biSizeImage', wintypes.DWORD),
                ('biXPelsPerMeter', wintypes.LONG), ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]

user32.GetDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
screen = user32.GetDC(None)
dc = gdi32.CreateCompatibleDC(screen)
bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
gdi32.SelectObject(dc, bmp)
kClientOnly, kFullContent = 1, 2
ok = user32.PrintWindow(hwnd, dc, kClientOnly | kFullContent)
info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
pixels = ctypes.create_string_buffer(w * h * 4)
got = gdi32.GetDIBits(dc, bmp, 0, h, pixels, ctypes.byref(info), 0)
gdi32.DeleteObject(bmp)
gdi32.DeleteDC(dc)
user32.ReleaseDC(None, screen)
if not ok or got != h:
    print('the window did not draw itself (PrintWindow %d, rows %d)' % (ok, got))
    sys.exit(3)
image = Image.frombuffer('RGBA', (w, h), pixels, 'raw', 'BGRA', 0, 1).convert('RGB')
if image.getbbox() is None:
    # A window in front may be shown by the display directly, past the desktop's compositor, and then draws itself
    # black here. In front, nothing covers it: its place on the screen is taken instead.
    if user32.GetForegroundWindow() != hwnd:
        print('the window drew itself black: no picture')
        sys.exit(3)
    from PIL import ImageGrab
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    image = ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + w, pt.y + h), all_screens=True)
image.save(out)
print(out, w, 'x', h)
