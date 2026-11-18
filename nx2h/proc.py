"""Child processes without console windows. The packed desktop app has no console, so every console tool it starts
(hdc, docker, python, winget) would flash a window of its own; started with CREATE_NO_WINDOW they get a hidden console,
which their own children share. From a console (nx2h.exe, python nx2h.py) the tools whose output the player should
see (winget, the upstream clone) keep that console instead.

Their output is text in two encodings: UTF-8 (docker, git, Python children with PYTHONIOENCODING) and the console code
page (Windows PowerShell 5, cmd, tasklist, netstat, a path hdc echoes back; 936 on a Chinese system). Read as one of
them only, the other's Chinese turns into mojibake, so each line is decoded by itself (decode): UTF-8 when it is valid
UTF-8, else the console code page. run() does that for every captured text output (text=True without an encoding);
lines() for a pipe read as it comes."""
from __future__ import annotations

import codecs
import locale
import re
import subprocess
import sys

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def _console_codec() -> str:
    """The code page console tools write to a pipe when not UTF-8: Windows' OEM code page (the console's default), else
    the locale's; 'mbcs' (the ANSI code page) when neither names a codec."""
    names = []
    if sys.platform == "win32":
        try:
            import ctypes
            names.append(f"cp{int(ctypes.windll.kernel32.GetOEMCP())}")
        except (AttributeError, OSError, ValueError):
            pass
    names.append(locale.getpreferredencoding(False) or "")
    for name in names:
        try:
            codec = codecs.lookup(name).name
        except LookupError:
            continue
        if codec != "utf-8":
            return codec
    return "mbcs" if sys.platform == "win32" else "latin-1"


CONSOLE = _console_codec()


def _line(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(CONSOLE, "replace")


def decode(data) -> str:
    """A child's output as text: each line UTF-8 when it is valid UTF-8, else the console code page."""
    if not data:
        return ""
    if isinstance(data, str):
        return data
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return "".join(_line(l) for l in data.splitlines(keepends=True))


_EOL = re.compile(rb"\r\n|\r|\n")


def lines(stream):
    """The lines of a child's binary output pipe as text, as they come, without their line ends: ended by \\n, \\r\\n or
    a lone \\r (a progress line rewritten in place), each decoded by itself (decode)."""
    buf = b""
    read1 = getattr(stream, "read1", None)   # what the pipe has now (Popen's buffered reader), not a full buffer
    while True:
        chunk = read1(1 << 16) if read1 else stream.readline()
        if not chunk:
            break
        buf += chunk
        end = len(buf) - 1 if buf.endswith(b"\r") else len(buf)   # a \r at the end may be half of \r\n
        start = 0
        for m in _EOL.finditer(buf, 0, end):
            yield _line(buf[start:m.start()])
            start = m.end()
        buf = buf[start:]
    if buf.rstrip(b"\r"):
        yield _line(buf.rstrip(b"\r"))


def text_out(data):
    """Captured output as text mode gives it: decoded (decode), every line end as \\n; None stays None."""
    return None if data is None else decode(data).replace("\r\n", "\n").replace("\r", "\n")


def _run(cmd, kw: dict) -> subprocess.CompletedProcess:
    """subprocess.run; captured text output (text=True without an encoding) decoded line by line (decode)."""
    captured = kw.get("capture_output") or subprocess.PIPE in (kw.get("stdout"), kw.get("stderr"))
    if not (kw.get("text") or kw.get("universal_newlines")) or not captured or kw.get("encoding"):
        return subprocess.run(cmd, **kw)
    check = kw.get("check", False)
    kw = {k: v for k, v in kw.items() if k not in ("text", "universal_newlines", "errors", "check")}
    if isinstance(kw.get("input"), str):   # what text mode would have sent
        kw["input"] = kw["input"].encode(locale.getpreferredencoding(False), "replace")
    r = subprocess.run(cmd, **kw)
    r.stdout, r.stderr = text_out(r.stdout), text_out(r.stderr)
    if check:
        r.check_returncode()
    return r


def has_console() -> bool:
    if sys.platform != "win32":
        return True
    import ctypes
    return bool(ctypes.windll.kernel32.GetConsoleWindow())


def run(cmd, **kw) -> subprocess.CompletedProcess:
    """A tool whose output is captured or not wanted: never a window."""
    kw.setdefault("creationflags", NO_WINDOW)
    return _run(cmd, kw)


def run_visible(cmd, **kw) -> subprocess.CompletedProcess:
    """A tool the player may watch (installs, clones): in the console when there is one, else hidden."""
    if not has_console():
        kw.setdefault("creationflags", NO_WINDOW)
    return _run(cmd, kw)


def popen(cmd, **kw) -> subprocess.Popen:
    """A tool read as it runs: give it binary pipes and read them with lines() / decode()."""
    kw.setdefault("creationflags", NO_WINDOW)
    return subprocess.Popen(cmd, **kw)
