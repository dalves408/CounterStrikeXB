"""Drive config.exe like a player, for testing (scratch LOCALAPPDATA).

  python scripts/config_check.py <appdata dir> <out dir>

Opens build/RelWithDebInfo/config.exe with LOCALAPPDATA=<appdata dir>,
captures the Video tab, switches to Controls, gives "A" the key Space,
captures, sets Render scale to 2x, saves, and closes. The files it wrote are
then in <appdata dir>/CounterStrikeXB.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

user32 = ctypes.WinDLL('user32', use_last_error=True)
ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetDlgItem.restype = wt.HWND

appdata, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
env = dict(os.environ, LOCALAPPDATA=appdata)
proc = subprocess.Popen([os.path.join(root, 'build', 'RelWithDebInfo', 'config.exe')], env=env)
main = None
for _ in range(50):
    time.sleep(0.1)
    main = user32.FindWindowW('CounterStrikeXBConfig', None)
    if main:
        break
if not main:
    sys.exit('config.exe window not found')
user32.SetForegroundWindow(main)
time.sleep(0.5)


def shot(name):
    r = wt.RECT()
    user32.GetWindowRect(main, ctypes.byref(r))
    if r.right <= r.left or r.bottom <= r.top:
        return
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(os.path.join(out, name))


def click(ctrl_id):
    user32.SendMessageW(user32.GetDlgItem(main, ctrl_id), 0x00F5, 0, 0)   # BM_CLICK


shot('config_video.png')
# Render scale (combo 200) -> 2x: CB_SETCURSEL, then CBN_SELCHANGE to the window
combo = user32.GetDlgItem(main, 200)
user32.SendMessageW(combo, 0x014E, 1, 0)
user32.SendMessageW(main, 0x0111, (1 << 16) | 200, combo)
# Controls tab: a click on its tab (about 85,12 in the tab control)
tab = user32.GetDlgItem(main, 100)
user32.PostMessageW(tab, 0x0201, 1, (12 << 16) | 85)                 # WM_LBUTTONDOWN
user32.PostMessageW(tab, 0x0202, 0, (12 << 16) | 85)                 # WM_LBUTTONUP
time.sleep(0.4)
# select row 8 ("A") in the list (101): LVM_SETITEMSTATE
lst = user32.GetDlgItem(main, 101)


for _ in range(8):                                                   # row 8 ("A"): arrow keys, as a player would
    user32.SendMessageW(lst, 0x0100, 0x28, 0)                        # WM_KEYDOWN Down
    user32.SendMessageW(lst, 0x0101, 0x28, 0)
click(102)                                                           # Change key
time.sleep(0.2)
user32.PostMessageW(main, 0x0100, 0x20, 0x00390001)                  # WM_KEYDOWN Space
time.sleep(0.3)
shot('config_controls.png')
click(106)                                                           # Save
time.sleep(0.3)
user32.PostMessageW(main, 0x0010, 0, 0)                              # WM_CLOSE
proc.wait(5)
cfg = os.path.join(appdata, 'CounterStrikeXB')
for name in ('settings.ini', 'keymap.ini'):
    p = os.path.join(cfg, name)
    print('----', name)
    print(open(p).read() if os.path.exists(p) else '(missing)')
