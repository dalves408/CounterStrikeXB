"""Capture the game window's client area as it is on screen (what the player sees).

  python scripts/winshot.py <out.png> [pid]

The renderer presents straight to the window (swap chain), so the guest-memory
dumps no longer show the final picture; this grabs the desktop where the
window's client area is. The window must be visible (not minimised or covered).
With a pid, the window of that process; else the first game window found.
"""
import ctypes
import ctypes.wintypes as wt
import sys

from PIL import ImageGrab

user32 = ctypes.WinDLL('user32', use_last_error=True)
ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)   # real pixels on scaled displays

out = sys.argv[1]
want_pid = int(sys.argv[2]) if len(sys.argv) > 2 else None
found = []

@ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def each(h, _):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, cls, 64)
    if cls.value == 'XboxRecompFramebuffer' and user32.IsWindowVisible(h):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if want_pid is None or pid.value == want_pid:
            found.append(h)
    return True

user32.EnumWindows(each, 0)
if not found:
    sys.exit('no game window')
h = found[-1]
rc = wt.RECT()
user32.GetClientRect(h, ctypes.byref(rc))
pt = wt.POINT(0, 0)
user32.ClientToScreen(h, ctypes.byref(pt))
box = (pt.x, pt.y, pt.x + rc.right, pt.y + rc.bottom)
ImageGrab.grab(bbox=box, all_screens=True).save(out)
print(out, box)
