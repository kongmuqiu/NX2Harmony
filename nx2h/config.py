"""Paths and local settings. Machine-specific values live in nx2h.local.json (ignored by git):

    {"backends": {"totk_v143": "C:/src/NativeGames-harmonyos"}, "keys": "C:/keys", "python": "C:/.../python.exe",
     "deveco": "D:/Program Files/Huawei/DevEco Studio"}

"deveco" is the DevEco Studio every build uses. The app writes the best one installed there itself, with the same value in
"deveco_picked" (device.find_deveco); a "deveco" set by hand to another folder is kept.

From a checkout everything sits in the repository (nx2h.local.json, work/, logs/). The packed app (PyInstaller,
dist/NX2Harmony) keeps them in data\\ beside NX2Harmony.exe (%LOCALAPPDATA%\\NX2Harmony when that folder is read-only) and
finds its bundled files (games.tsv) in its own folder.
Without a backends entry each backend is looked for in backends/ (this repository's own copy, which the full package
carries beside the exe too)."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

FROZEN = bool(getattr(sys, "frozen", False))
REPO = Path(__file__).resolve().parents[1]   # the bundled files' folder when packed (games.tsv, THIRD_PARTY_NOTICES.md)
APP_DIR = Path(sys.executable).resolve().parent if FROZEN else REPO   # where nx2h.exe / nx2h.py is
OLD_DATA = Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "NX2Harmony"   # the first packed app's place


def _data_dir() -> Path:
    """The packed app is portable: its settings, icon cache and logs live in data/ beside NX2Harmony.exe, so a copied
    folder keeps them (a read-only place, e.g. Program Files, falls back to %LOCALAPPDATA%\\NX2Harmony). The first
    start copies what the earlier %LOCALAPPDATA% place held."""
    if not FROZEN:
        return REPO
    d = APP_DIR / "data"
    try:
        d.mkdir(exist_ok=True)
        probe = d / ".write-test"
        probe.write_text("")
        probe.unlink()
    except OSError:
        return OLD_DATA
    if not (d / "nx2h.local.json").exists() and (OLD_DATA / "nx2h.local.json").is_file():
        try:
            shutil.copy2(OLD_DATA / "nx2h.local.json", d / "nx2h.local.json")
            if (OLD_DATA / "work" / "icons").is_dir():
                shutil.copytree(OLD_DATA / "work" / "icons", d / "work" / "icons", dirs_exist_ok=True)
        except OSError:
            pass
    return d


DATA = _data_dir()
LOCAL = DATA / "nx2h.local.json"
WORK = DATA / "work"
LOGS = DATA / "logs"


def _default_backend(folder: str) -> Path:
    # backends/: this checkout's own copy, or beside the packed app (the full package carries them); then the older
    # places (the checkout's neighbour, near the packed app)
    here = [APP_DIR / "backends", REPO.parent] + list(APP_DIR.parents)[:4]
    return next((d / folder for d in here if (d / folder).is_dir()), APP_DIR / "backends" / folder)


DEFAULT_BACKENDS = {"totk_v143": "NativeGames-harmonyos", "za_202": "LZA", "oot_v131072": "OOT"}


def backend_dirs() -> list:
    """Every backend folder this app may use: the configured ones and where each would be looked for, without
    remembering anything (scan.py keeps out of their prepared/ and artifacts/)."""
    conf = local().get("backends") or {}
    out = []
    for name, folder in DEFAULT_BACKENDS.items():
        for p in (conf.get(name), _default_backend(folder)):
            if p and Path(p) not in out:
                out.append(Path(p))
    return out


def refresh_path() -> None:
    """PATH as a newly started program would get it: the machine's and the user's PATH from the registry (expanded),
    then whatever this process had on top. An installer (winget) changes the registry only, so without this a tool
    installed while the app runs (Python, zstd) is not found until the app is opened again."""
    if sys.platform != "win32":
        return
    import winreg
    parts = []
    for root, key in ((winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
                      (winreg.HKEY_CURRENT_USER, "Environment")):
        try:
            with winreg.OpenKey(root, key) as k:
                value, _ = winreg.QueryValueEx(k, "Path")
        except OSError:
            continue
        parts += [os.path.expandvars(p) for p in str(value).split(os.pathsep) if p.strip()]
    if not parts:
        return
    seen, out = set(), []
    for p in parts + os.environ.get("PATH", "").split(os.pathsep):
        n = os.path.normcase(os.path.normpath(p.strip().strip('"'))) if p.strip() else ""
        if n and n not in seen:
            seen.add(n)
            out.append(p.strip())
    os.environ["PATH"] = os.pathsep.join(out)


def python() -> str:
    """The Python that runs the backends' own scripts (prepare_v143.py, the icon reader): this one from a checkout; for
    the packed app, the configured one or the one on PATH (not the Microsoft Store stub)."""
    if not FROZEN:
        return sys.executable
    p = local().get("python")
    if p and Path(p).is_file():
        return p
    for name in ("python", "python3"):
        w = shutil.which(name)
        if w and "WindowsApps" not in w:
            return w
    if shutil.which("py"):
        from . import proc   # its path may hold Chinese: decoded as UTF-8, else the console code page
        try:
            r = proc.run(["py", "-3", "-c", "import sys; print(sys.executable)"], capture_output=True, text=True,
                         timeout=30)
        except (OSError, subprocess.SubprocessError):
            return ""
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip().splitlines()[-1].strip()
    return ""


def cli() -> list:
    """The command that runs nx2h's command line: nx2h.exe next to the packed app, else this Python with nx2h.py."""
    return [str(APP_DIR / "nx2h.exe")] if FROZEN else [sys.executable, str(REPO / "nx2h.py")]


def local() -> dict:
    try:
        return json.loads(LOCAL.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


_save_lock = threading.Lock()   # this process's threads (the phone poll, the checks, the window) save in turn


def save_local(**values) -> None:
    """values merged into nx2h.local.json. Written to a temporary file that then replaces the old one, so a reader never
    sees half a file. A file that is there but cannot be read (another process writing it just then: read again once)
    is never replaced by one holding only these values."""
    with _save_lock:
        data = None
        for attempt in range(2):
            try:
                data = json.loads(LOCAL.read_text(encoding="utf-8"))
                break
            except FileNotFoundError:
                data = {}
                break
            except (OSError, ValueError):
                if attempt == 0:
                    time.sleep(0.2)
        if not isinstance(data, dict):
            raise OSError(f"{LOCAL} 读不出来，没有改动它")
        data.update(values)
        LOCAL.parent.mkdir(parents=True, exist_ok=True)
        tmp = LOCAL.with_name(f"{LOCAL.name}.{os.getpid()}.tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        for attempt in range(5):
            try:
                os.replace(str(tmp), str(LOCAL))
                return
            except PermissionError:   # Windows: another process has the file open just then
                if attempt == 4:
                    tmp.unlink(missing_ok=True)
                    raise
                time.sleep(0.1)


def backend_root(name: str) -> Path:
    backends = local().get("backends") or {}
    p = backends.get(name)
    if p and Path(p).is_dir():   # a remembered place that went away (the app copied elsewhere) is looked for again
        return Path(p)
    d = _default_backend(DEFAULT_BACKENDS[name])
    if d.is_dir():   # remembered, so a copy of the packed app (its data/ goes with it) still finds it
        save_local(backends=dict(backends, **{name: str(d)}))
    return d


def games() -> list:
    """games.tsv rows as dicts (title_id, name, version, update, backend, status, needs)."""
    header, rows = None, []
    for line in (REPO / "games.tsv").read_text(encoding="utf-8").splitlines():
        if line.startswith("# title_id"):
            header = line[2:].split("\t")
        elif line.strip() and not line.startswith("#") and header:
            r = line.split("\t")
            if len(r) == len(header):
                rows.append(dict(zip(header, r)))
    return rows


# games.tsv statuses that build in one click: 已验证 (played through on the phone) and 优化中 (playable, frame rate and a few
# pictures still being tuned); 适配中 is shown but not built
BUILDABLE = ("已验证", "优化中")


def key_candidates() -> list:
    """Folders that hold a prod.keys file, the configured one first (names only: the file is never opened here)."""
    out = []
    conf = local().get("keys")
    for d in ([Path(conf)] if conf else []) + [Path(os.environ.get("USERPROFILE", "~")).expanduser() / ".switch"]:
        if (d / "prod.keys").is_file() and d not in out:
            out.append(d)
    return out
