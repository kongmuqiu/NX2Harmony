"""A check of the packed app: start NX2Harmony.exe (it saves a snapshot and quits after 6 s, NX2H_SNAPSHOT) and record
every console window that appears meanwhile (the classic console and Windows Terminal's host). The desktop app should
open none: its tools run with CREATE_NO_WINDOW (nx2h/proc.py).
    python packaging/check_no_windows.py [path to NX2Harmony.exe]"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "dist" / "NX2Harmony" / "NX2Harmony.exe"
CLASSES = {"ConsoleWindowClass", "CASCADIA_HOSTING_WINDOW_CLASS", "PseudoConsoleWindow"}
user32 = ctypes.windll.user32


def consoles() -> dict:
    found = {}

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value in CLASSES and user32.IsWindowVisible(hwnd):
            title = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, title, 512)
            found[hwnd] = (buf.value, title.value)
        return True
    user32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb), 0)
    return found


def main() -> int:
    before = consoles()
    shot = Path(tempfile.gettempdir()) / "nx2h-nowindow.png"
    p = subprocess.Popen([str(EXE)], env=dict(os.environ, NX2H_SNAPSHOT=str(shot)))
    seen = {}
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < 40:
        for h, v in consoles().items():
            if h not in before and h not in seen:
                seen[h] = (round(time.time() - t0, 2),) + v
        time.sleep(0.02)
    print(f"app exit {p.poll()}, console windows opened: {len(seen)}")
    for v in seen.values():
        print("  ", v)
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
