"""The phone, through DevEco's hdc. Settings, first found wins: nx2h.local.json {"hdc": {"exe", "server", "target"}},
the TOTK backend's native-harmonyos/local.ohos.env / .ps1 (TOTK_HDC, TOTK_HDC_SERVER, TOTK_HDC_TARGET), the hdc of the
DevEco Studio in use (find_deveco). Without a target, the one connected phone is used. When the server in use sees no
phone, the other hdc servers running on this PC are asked (DevEco Studio may run its own, e.g. on port 48710); the first
that sees one is used from then on and remembered (nx2h.local.json "hdc" "server").

DevEco Studio: a PC may have several (an older beta on C:, a newer release on D:). Every one installed is looked for
(find_deveco) and the one with the newest SDK is used for everything: hdc, the SDK the doctor checks, and the backends'
scripts (TOTK_DEVECO, build_hap.ps1 -DevEcoRoot). DEVECO, as other modules import it, is that folder."""
from __future__ import annotations

import base64
import contextlib
import copy
import csv
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import NamedTuple, Optional

from . import config
from . import proc

DEVECO_DEFAULTS = (r"C:\Program Files\Huawei\DevEco Studio", r"D:\Program Files\Huawei\DevEco Studio")
_UNINSTALL = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
_UNINSTALLER = re.compile(r'\s*(?:"([^"]+)"|(\S.*?\.exe))', re.I)   # the program in an UninstallString
_PS =re.compile(r"""^\s*\$env:(\w+)\s*=\s*(['"])(.*?)\2""")
_SCREEN0 = re.compile(r"screen\[0\]:(.*?)(?=screen\[\d+\]:|$)", re.I | re.S)
_PHYSICAL = re.compile(r"physicalresolution=(\d{1,5})x(\d{1,5})", re.I)
_SIZE = re.compile(r"\s*(\d{1,5})\s*[xX\u00d7*:]\s*(\d{1,5})\s*")   # x, X, the multiplication sign, * or :
_TARGET = re.compile(r"[0-9A-Za-z][0-9A-Za-z._:-]*")   # a serial number or <ip>:<port>: one word
_OVERRIDE = re.compile(r"OverrideTimeout\s*=\s*(\d+)\s*ms", re.I)
_BUNDLE = re.compile(r"[A-Za-z0-9._-]+")
_SERVER_MODE = re.compile(r"(?:^|\s)-m(?:\s|$)")                 # hdc running as the server
_SERVER_ADDR = re.compile(r"""(?:^|\s)-s\s*["']?([^\s"']+)""")    # its address: [::ffff:]127.0.0.1:<port>
# the screen-off time keep_awake sets and sets again every few seconds: about 5 minutes, so a build that is killed
# (the app's 停止, its window closed) leaves a phone that still turns its screen off. An odd value: a later build knows a
# left-over one as its own (override_timeout), not as the phone's
AWAKE_MS = 300017
NEVER_MS = 2147483647   # what keep_awake set before 2026-11-16 (never): its own too
DEFAULT_PORT = 8710    # hdc's own server port when none is given (OHOS_HDC_SERVER_PORT changes it)
DISCOVER_EVERY = 15     # seconds before other hdc servers are asked again after none had a phone
_discovered = [0.0]     # when that last happened (one value for the process: the app polls with new Hdc objects)


def landscape(a: int, b: int):
    """A screen size as (width, height) with the long side first, or None for a zero side."""
    return (max(a, b), min(a, b)) if a and b else None


def screen_from_dump(text: str):
    """screen[0] of `hidumper -s RenderService -a screen` in landscape pixels, from its "physical resolution=AxB": the
    panel as it stands upright, so a phone's "render resolution=1320x2848, physical resolution=1320x2848" is
    (2848, 1320). None when there is no such screen or size. Whitespace goes first: a console that wrapped the long line
    may have cut a word in two ("phys" / "ical")."""
    for block in _SCREEN0.finditer(re.sub(r"\s+", "", text or "")):
        m = _PHYSICAL.search(block.group(1))
        if m:
            return landscape(int(m.group(1)), int(m.group(2)))
    return None


def parse_size(text: str):
    """"2848x1320" as a landscape (width, height), or None; also 1320x2848, 2848X1320, 2848*1320, 2848:1320 or with
    the multiplication sign."""
    m = _SIZE.fullmatch(text or "")
    return landscape(int(m.group(1)), int(m.group(2))) if m else None


def parse_targets(text: str) -> list:
    """The targets in `hdc list targets` output: each a serial number or <ip>:<port>, one word. hdc's own messages
    ("[Empty]", "[Fail]ExecuteCommand need connect-key?", "Connect server failed", ...) are not targets."""
    return [l.strip() for l in (text or "").splitlines() if _TARGET.fullmatch(l.strip())]


def override_timeout(text: str):
    """The screen-off time set over the user's own, from `hidumper -s PowerManagerService -a -s`
    ("OverrideTimeout=<n>ms"), or None: none set ("N/A"), or one keep_awake sets (AWAKE_MS, or NEVER_MS from an
    earlier version: left by a build that was stopped by force, so not the phone's own)."""
    m = _OVERRIDE.search(text or "")
    n = int(m.group(1)) if m else 0
    return n if 0 < n < NEVER_MS and n != AWAKE_MS else None


def hdc_said(line: str) -> bool:
    """A line hdc writes itself instead of the command's output ("[Fail]ExecuteCommand need connect-key?", "[Info]...",
    "[Empty]")."""
    return line.strip().startswith(("[Fail]", "[Info]", "[Empty]"))


def hdc_pids(tasklist_csv: str) -> set:
    """The process ids of hdc.exe in `tasklist /FI "IMAGENAME eq hdc.exe" /FO CSV /NH` output (no rows: a message)."""
    out = set()
    for row in csv.reader(l for l in (tasklist_csv or "").splitlines() if l.startswith('"')):
        if len(row) > 1 and row[0].strip().lower() == "hdc.exe" and row[1].strip().isdigit():
            out.add(int(row[1]))
    return out


def listening_ports(netstat: str, pids) -> list:
    """The TCP ports these processes listen on at 127.0.0.1 (or every address) in `netstat -ano -p TCP` output, lowest
    first. A listening row is told by its foreign address 0.0.0.0:0: the state's name is translated."""
    ports = set()
    for line in (netstat or "").splitlines():
        parts = line.split()
        if len(parts) < 4 or parts[0].upper() != "TCP" or not parts[-1].isdigit() or int(parts[-1]) not in pids:
            continue
        host, _, port = parts[1].rpartition(":")
        if host in ("127.0.0.1", "0.0.0.0") and parts[2] in ("0.0.0.0:0", "*:*") and port.isdigit():
            ports.add(int(port))
    return sorted(ports)


def server_ports(cmdlines: str) -> dict:
    """{process id: port} of the hdc processes in server mode, from "<pid>\t<command line>" lines: a server runs as
    `hdc -l 3 -s ::ffff:127.0.0.1:48710 -m` (without -s: the default port)."""
    out = {}
    for line in (cmdlines or "").splitlines():
        pid, _, cmd = line.strip().partition("\t")
        if not pid.isdigit() or not _SERVER_MODE.search(cmd):
            continue
        m = _SERVER_ADDR.search(cmd)
        port = m.group(1).rpartition(":")[2] if m else os.environ.get("OHOS_HDC_SERVER_PORT", str(DEFAULT_PORT))
        if port.isdigit():
            out[int(pid)] = int(port)
    return out


# the hdc processes' command lines, one "<pid>\t<command line>" each (tasklist does not show them)
_CMDLINES = ("Get-CimInstance Win32_Process -Filter \"Name='hdc.exe'\" | "
             "ForEach-Object { [string]$_.ProcessId + [char]9 + $_.CommandLine }")


def hdc_servers() -> list:
    """The ports of the hdc servers running on this PC, lowest first: hdc.exe processes in server mode that listen on
    their port. Only those: a server also listens for each `hdc fport` forward, and asking such a port for targets
    would send hdc's greeting into a connection something else uses (a performance panel, say). Without the command
    lines, only hdc's usual ports (DEFAULT_PORT, DevEco Studio's 48710) are taken."""
    if sys.platform != "win32":
        return []
    try:
        r = proc.run(["tasklist", "/FI", "IMAGENAME eq hdc.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                     timeout=10)
        pids = hdc_pids(r.stdout)
        if not pids:
            return []
        netstat = proc.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True, timeout=15).stdout
        encoded = base64.b64encode(_CMDLINES.encode("utf-16-le")).decode("ascii")
        r = proc.run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], capture_output=True,
                     text=True, timeout=30)
        servers = server_ports(r.stdout) if r.returncode == 0 else {}
    except (OSError, subprocess.SubprocessError):
        return []
    if servers:
        return sorted({port for pid, port in servers.items() if pid in pids and port in listening_ports(netstat, {pid})})
    usual = {DEFAULT_PORT, 48710}
    return [p for p in listening_ports(netstat, pids) if p in usual]


def _read_bytes(p: subprocess.Popen) -> int:
    """How much a running process has read so far (Windows I/O counters; 0 elsewhere or when unknown). The hdc client
    reads the local file itself during `file send`, so this follows the transfer of one big file."""
    if sys.platform != "win32":
        return 0
    import ctypes

    class IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes", "OtherBytes")]

    io = IoCounters()
    ok = ctypes.windll.kernel32.GetProcessIoCounters(ctypes.c_void_p(int(p._handle)), ctypes.byref(io))
    return int(io.ReadBytes) if ok else 0


def _backend_env() -> dict:
    out = {}
    root = config.backend_root("totk_v143") / "native-harmonyos"
    for name in ("local.ohos.ps1", "local.ohos.env"):
        p = root / name
        if not p.is_file():
            continue
        for line in p.read_text(encoding="utf-8-sig").splitlines():
            m = _PS.match(line) if name.endswith(".ps1") else None
            if m:
                out.setdefault(m.group(1), m.group(3))
            elif not name.endswith(".ps1") and "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                out.setdefault(k.strip(), v.strip())
    return out


# ---------- DevEco Studio: which one ----------
def _numbers(version: str) -> tuple:
    return tuple(int(n) for n in re.findall(r"\d+", version or ""))


@dataclasses.dataclass(frozen=True)
class DevEco:
    """One DevEco Studio folder and the HarmonyOS SDK inside it (sdk/default)."""
    root: Path
    version: str = ""    # the SDK's, e.g. "26.0.0.105" ('' when it is not downloaded yet)
    release: str = ""    # its release type: "Release", "Beta2", ...
    platform: str = ""   # its API level as a version, e.g. "26.0.0"

    def is_release(self) -> bool:
        return self.release.strip().lower() == "release"

    def rank(self) -> tuple:
        """Higher is better: the SDK's API level, at the same level a release before a beta, then the full version; a
        folder without an SDK last."""
        return (_numbers(self.platform) or _numbers(self.version)[:3], self.is_release(), _numbers(self.version))

    def label(self) -> str:
        """'D:\\Program Files\\Huawei\\DevEco Studio（SDK 26.0.0.105）', the release type named unless a release."""
        if not self.version:
            return f"{self.root}（SDK 还没下载）"
        return f"{self.root}（SDK {self.version}{'' if self.is_release() else ' ' + (self.release or '预览版')}）"


class Found(NamedTuple):
    use: Optional[DevEco]   # the one this app uses (None: none on this PC)
    installs: list          # every DevEco Studio found, the best first
    ignored: str            # nx2h.local.json "deveco" set by the user but not a DevEco Studio folder ('' otherwise)


def sdk_info(root) -> dict:
    """The SDK's {"version", "releaseType", "platformVersion", "apiVersion", ...} inside a DevEco Studio folder, from
    sdk/default/sdk-pkg.json ("data"), else the oh-uni-package.json of its openharmony toolchains or native part; {}
    without an SDK."""
    default = Path(root) / "sdk" / "default"
    for f, part in ((default / "sdk-pkg.json", "data"),
                    (default / "openharmony" / "toolchains" / "oh-uni-package.json", ""),
                    (default / "openharmony" / "native" / "oh-uni-package.json", "")):
        try:
            d = json.loads(f.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            continue
        d = d.get(part) if part and isinstance(d, dict) else d
        if isinstance(d, dict) and d.get("version"):
            return d
    return {}


def is_deveco(root) -> bool:
    """A DevEco Studio folder: its program, or an SDK where DevEco Studio keeps it."""
    r = Path(root)
    return (r / "bin" / "devecostudio64.exe").is_file() or (r / "sdk" / "default" / "openharmony").is_dir()


def _reg_value(key, name: str) -> str:
    import winreg
    try:
        v = winreg.QueryValueEx(key, name)[0]
    except OSError:
        return ""
    return v.strip() if isinstance(v, str) else ""


def registry_places() -> list:
    """The folders of the DevEco Studio installs Windows knows: the uninstall entries (all users in both registry
    views, this user) whose DisplayName starts with "DevEco Studio", their InstallLocation, else the folder above
    bin\\Uninstall.exe in UninstallString."""
    if sys.platform != "win32":
        return []
    import winreg
    out = []
    for hive, view in ((winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_64KEY), (winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_32KEY),
                       (winreg.HKEY_CURRENT_USER, 0)):
        try:
            top = winreg.OpenKey(hive, _UNINSTALL, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        with top:
            for i in range(100000):
                try:
                    name = winreg.EnumKey(top, i)
                except OSError:   # no more entries
                    break
                try:
                    with winreg.OpenKey(top, name, 0, winreg.KEY_READ | view) as k:
                        if not _reg_value(k, "DisplayName").startswith("DevEco Studio"):
                            continue
                        place = _reg_value(k, "InstallLocation")
                        m = None if place else _UNINSTALLER.match(_reg_value(k, "UninstallString"))
                        if m:   # "<folder>\bin\Uninstall.exe" [arguments]
                            exe = Path(m.group(1) or m.group(2))
                            place = str(exe.parent.parent) if exe.parent.name.lower() == "bin" else ""
                except OSError:
                    continue
                if place:
                    out.append(place.strip('"'))
    return out


def _same(a, b) -> bool:
    return bool(a) and bool(b) and os.path.normcase(os.path.abspath(str(a))) == os.path.normcase(os.path.abspath(str(b)))


def deveco_installs(extra=()) -> list:
    """Every DevEco Studio on this PC, the best first (DevEco.rank; equal ones in the order found): the places in
    `extra`, TOTK_DEVECO (this process's environment, then the TOTK backend's local.ohos.env / .ps1), the uninstall
    entries, the default places on C: and D:."""
    places = list(extra) + [os.environ.get("TOTK_DEVECO"), _backend_env().get("TOTK_DEVECO")] + registry_places()
    places += [str(Path(os.environ.get("ProgramFiles") or r"C:\Program Files") / "Huawei" / "DevEco Studio")]
    places += list(DEVECO_DEFAULTS)
    found = []
    for p in places:
        if not p or any(_same(p, d.root) for d in found):
            continue
        try:
            if not is_deveco(p):
                continue
        except OSError:   # a drive that is not there
            continue
        info = sdk_info(p)
        found.append(DevEco(Path(p), str(info.get("version") or ""), str(info.get("releaseType") or info.get("stage") or ""),
                            str(info.get("platformVersion") or "")))
    return sorted(found, key=DevEco.rank, reverse=True)   # sorted() is stable: ties keep their order


_deveco_lock = threading.Lock()
_deveco = []   # [Found] once looked for (the app makes a new Hdc every few seconds)


def find_deveco(refresh: bool = False) -> Found:
    """Which DevEco Studio this app uses: nx2h.local.json "deveco" when the user set it there (and it is a DevEco
    Studio folder), else the best one installed (deveco_installs). The best one is remembered in nx2h.local.json as
    "deveco", with the same value in "deveco_picked": while the two agree the choice is the app's own and a better
    install found later replaces it; a "deveco" the user writes by hand differs from "deveco_picked" and is kept.
    Looked for once per process; again with refresh (the 环境 page's checks, after an install)."""
    with _deveco_lock:
        if _deveco and not refresh:
            return _deveco[0]
        conf = config.local()
        mine, picked = str(conf.get("deveco") or ""), str(conf.get("deveco_picked") or "")
        own = bool(mine) and not _same(mine, picked)
        installs = deveco_installs([mine] if mine else [])
        use = next((d for d in installs if _same(d.root, mine)), None) if own else None
        ignored = mine if own and use is None else ""
        if use is None and installs:
            use = installs[0]
            if not own and not (_same(mine, use.root) and _same(picked, use.root)):
                try:
                    config.save_local(deveco=str(use.root), deveco_picked=str(use.root))
                except OSError:
                    pass
        _deveco[:] = [Found(use, installs, ignored)]
        return _deveco[0]


def deveco_root() -> Path:
    """The DevEco Studio folder in use (find_deveco); without one, where TOTK_DEVECO or the default place says."""
    use = find_deveco().use
    return use.root if use else Path(os.environ.get("TOTK_DEVECO") or DEVECO_DEFAULTS[0])


def __getattr__(name: str):
    # DEVECO, as agc, signing and the backends import it: looked for when first asked, not when this module loads
    if name == "DEVECO":
        return deveco_root()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


# ---------- Java: keytool for the signing (agc.py), java for hvigor (build_hap.ps1) ----------
JAVA_PLACES = ("Microsoft/jdk-*", "Eclipse Adoptium/*")   # below Program Files, where the JDK installers put it
JAVA_MIN = 17   # the oldest Java taken from outside the toolchain: hvigor wants 17+, and keytool -groupname (agc.py)
#                 is not in Java 8
_JAVA_HOME = re.compile(r"^\s*java\.home\s*=\s*(.+?)\s*$", re.M)
_JAVA_SPEC = re.compile(r"^\s*java\.specification\.version\s*=\s*(\S+)", re.M)
_RELEASE = re.compile(r'^JAVA_VERSION\s*=\s*"?([^"\s]+)', re.M)
_java_homes = {}   # {the java on PATH: (its java.home or '', its version or 0)}: asking java takes a moment, the 环境
#                    page asks often


def _major(version: str) -> int:
    """17 for "17.0.14" or "17", 8 for "1.8.0_301" or "1.8"; 0 for nothing."""
    n = re.findall(r"\d+", version or "")
    return (int(n[1]) if n[0] == "1" and len(n) > 1 else int(n[0])) if n else 0


def java_version(home) -> int:
    """The major version of a Java folder: from its release file (a JDK 8's java.home is its jre folder, the file one
    above), else what the java on PATH said of itself when this is its folder; 0 when unknown."""
    h = Path(home)
    for f in [h / "release"] + ([h.parent / "release"] if h.name.lower() == "jre" else []):
        try:
            m = _RELEASE.search(f.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        if m:
            return _major(m.group(1))
    return next((v for where, v in _java_homes.values() if v and _same(where, h)), 0)


def is_jdk(home, floor: int = JAVA_MIN) -> bool:
    r"""A Java folder that signs: bin\java.exe and bin\keytool.exe both there, and not older than `floor` (0: any;
    a version that cannot be read counts)."""
    b = Path(home) / "bin"
    ext = ".exe" if sys.platform == "win32" else ""
    if not ((b / f"java{ext}").is_file() and (b / f"keytool{ext}").is_file()):
        return False
    v = java_version(home) if floor else 0
    return not v or v >= floor


def which_java(path: Optional[str] = None, exe_only: bool = True) -> Optional[str]:
    """The java on `path` (else this process's PATH). exe_only: java.exe as hvigor's node starts it (not a java.cmd or
    .bat); else any PATHEXT one, as build_hap.ps1's Get-Command finds it. Quoted entries count (Python 3.9's which
    does not unquote them, Windows does)."""
    path = os.environ.get("PATH", "") if path is None else path
    dirs = os.pathsep.join(p.strip().strip('"') for p in path.split(os.pathsep) if p.strip().strip('"'))
    return shutil.which("java.exe" if exe_only and sys.platform == "win32" else "java", path=dirs)


def path_java_home(path: Optional[str] = None) -> Optional[Path]:
    r"""The real java.home of the java on PATH (the one hvigor runs), as that java reports it: PATH may name a launcher
    that forwards to it (Oracle's ...\Common Files\Oracle\Java\javapath, no keytool beside it). Without an answer,
    the folder above the bin it is in. None when PATH has no java."""
    java = which_java(path)
    if not java:
        return None
    if java not in _java_homes:
        home, spec = "", 0
        try:
            r = proc.run([java, "-XshowSettings:properties", "-version"], capture_output=True, text=True, timeout=60)
            out = (r.stderr or "") + "\n" + (r.stdout or "")
            m, s = _JAVA_HOME.search(out), _JAVA_SPEC.search(out)
            home, spec = (m.group(1) if m else ""), (_major(s.group(1)) if s else 0)
        except (OSError, subprocess.SubprocessError):
            pass
        if not home and Path(java).parent.name.lower() == "bin":
            home = str(Path(java).resolve().parent.parent)
        _java_homes[java] = (home, spec)
    home = _java_homes[java][0]
    return Path(home) if home else None


def java_places() -> list:
    """The JDKs installed where their installers put them (JAVA_PLACES below Program Files), the newest first: one
    installed after this app started is not on its PATH yet."""
    base = Path(os.environ.get("ProgramW6432") or os.environ.get("ProgramFiles") or r"C:\Program Files")
    out = []
    for pattern in JAVA_PLACES:
        try:
            out += sorted(base.glob(pattern), key=lambda p: _numbers(p.name), reverse=True)
        except OSError:
            continue
    return out


def java_home() -> Optional[Path]:
    r"""The Java that signs (agc.py runs its keytool) and builds (hvigor runs `java` from PATH), a folder with
    bin\java.exe and bin\keytool.exe; None when there is none. First found wins: the toolchain's own (the jbr of the
    DevEco Studio in use, as before, whatever its version), then Java JAVA_MIN or newer: the java on PATH (its real
    java.home: keytool and hvigor then run the same one), JAVA_HOME, a JDK where its installer puts it (java_places).
    backends/common.py puts one that PATH does not start first on the scripts' PATH (java_for_scripts)."""
    jbr = deveco_root() / "jbr"
    if is_jdk(jbr, 0):
        return jbr
    for more in (lambda: [path_java_home()], lambda: [(os.environ.get("JAVA_HOME") or "").strip().strip('"')],
                 java_places):
        for home in more():
            if home and is_jdk(home):
                return Path(home)
    return None


def java_for_scripts(path: str) -> Optional[Path]:
    """The Java (java_home) a backend script's PATH (`path`) gets first, with JAVA_HOME, so hvigor runs the one keytool
    runs; None: PATH stays. DevEco Studio's jbr only when PATH has no java at all, as build_hap.ps1 does itself (as
    before). Any other one also when the java.exe on PATH is not it (no keytool beside it, older than JAVA_MIN)."""
    jdk = java_home()
    if jdk is None:
        return None
    if _same(jdk, deveco_root() / "jbr"):
        return None if which_java(path, exe_only=False) else jdk
    return None if which_java(path) and _same(jdk, path_java_home(path)) else jdk


class Hdc:
    def __init__(self):
        conf = config.local().get("hdc") or {}
        env = _backend_env()
        self.exe = (conf.get("exe") or os.environ.get("TOTK_HDC") or env.get("TOTK_HDC")
                    or str(deveco_root() / "sdk/default/openharmony/toolchains/hdc.exe"))
        self.server = conf.get("server") or os.environ.get("TOTK_HDC_SERVER") or env.get("TOTK_HDC_SERVER") or ""
        self.target = conf.get("target") or os.environ.get("TOTK_HDC_TARGET") or env.get("TOTK_HDC_TARGET") or ""

    def available(self) -> bool:
        return Path(self.exe).is_file()

    def _base(self, with_target=True) -> list:
        args = [self.exe]
        if self.server:
            args += ["-s", self.server]
        if with_target and self.target:
            args += ["-t", self.target]
        return args

    def run(self, *args, timeout=None, with_target=True) -> str:
        """hdc's output and errors as text ('' when it does not run or answer); proc.run decodes them line by line."""
        try:
            r = proc.run(self._base(with_target) + list(args), capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.SubprocessError):   # no hdc, or a server that does not answer: no phone
            return ""
        return ((r.stdout or "") + (r.stderr or "")).strip()

    def targets(self, timeout=20) -> list:
        return parse_targets(self.run("list", "targets", timeout=timeout, with_target=False))

    def pick(self) -> str:
        """The target to use: the configured one when connected, else the only one connected ('' when none or several).
        With none on the server in use, another hdc server on this PC that has one (discover)."""
        ts = self.targets()
        if not ts and self.discover():
            ts = self.targets()
        if self.target and self.target in ts:
            return self.target
        if len(ts) == 1:
            self.target = ts[0]
            return self.target
        return ""

    def discover(self) -> str:
        """Another hdc server running on this PC that sees a phone: used from now on and remembered in nx2h.local.json
        ("hdc": {"server": "127.0.0.1:<port>"}, beside the other hdc settings). '' when none does; then not asked again
        for DISCOVER_EVERY seconds (the app polls the phone every few seconds)."""
        if not self.available() or time.time() - _discovered[0] < DISCOVER_EVERY:
            return ""
        host, _, port = (self.server or "").rpartition(":")
        port = port or os.environ.get("OHOS_HDC_SERVER_PORT", "")
        here = int(port) if port.isdigit() and host in ("", "127.0.0.1", "localhost") else DEFAULT_PORT
        deadline = time.time() + 30
        for p in hdc_servers():
            if p == here:
                continue   # the server in use: it has just said it sees no phone
            if time.time() > deadline:
                break
            other = copy.copy(self)
            other.server = f"127.0.0.1:{p}"
            if other.targets(timeout=6):
                self.server = other.server
                config.save_local(hdc=dict(config.local().get("hdc") or {}, server=self.server))
                _discovered[0] = 0.0
                return self.server
        _discovered[0] = time.time()
        return ""

    def shell(self, cmd: str, timeout=60) -> str:
        return self.run("shell", cmd, timeout=timeout)

    def param(self, name: str) -> str:
        """A system parameter of the phone; '' without a target, or when hdc answered with its own message (a phone
        just plugged in: "[Fail]ExecuteCommand need connect-key?")."""
        if not self.target:
            return ""
        last = (self.shell(f"param get {name}", timeout=20).splitlines() or [""])[-1].strip()
        return "" if hdc_said(last) else last

    def model(self) -> str:
        """"<model> · <system>" for the phone row and the top bar; '' when the phone did not tell them."""
        a, b = self.param("const.product.model"), self.param("const.ohos.fullname")
        return f"{a} · {b}" if a and b else ""

    def screen(self):
        """The phone's screen as a landscape (width, height) in pixels, or None (no target, or no answer)."""
        return screen_from_dump(self.shell("hidumper -s RenderService -a screen", timeout=30)) if self.target else None

    def size_of(self, path: str):
        """The size of a file on the phone, or None."""
        out = self.shell(f"stat -c %s '{path}' 2>/dev/null", timeout=20).strip()
        return int(out) if out.isdigit() else None

    def send(self, local: Path, remote: str, bundle: str, progress=None) -> str:
        """hdc file send; progress(bytes sent so far) about once a second while it runs, from what hdc has read of the
        file (hdc itself only reports at the end, and one RomFS file is 16 GB)."""
        args = self._base() + ["file", "send", "-b", bundle, str(local), remote]
        if progress is None:
            return self.run(*args[len(self._base()):])
        try:
            p = proc.popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError:
            return ""
        out = []
        reader = threading.Thread(target=lambda: out.append(proc.text_out(p.stdout.read())), daemon=True)
        reader.start()
        size = local.stat().st_size
        while True:
            try:
                p.wait(timeout=1)
                break
            except subprocess.TimeoutExpired:
                n = _read_bytes(p)
                if n:
                    progress(min(n, size))
        reader.join(timeout=10)
        return "".join(out).strip()

    def install(self, hap: Path) -> str:
        return self.run("install", "-r", str(hap), timeout=600)

    def installed(self, bundle: str) -> bool:
        return "bundleName" in self.shell(f"bm dump -n {bundle}", timeout=30)

    @staticmethod
    def restore_command(old) -> str:
        """The phone's own screen-off time back: the override it had before ("power-shell timeout -o <ms>"), else none."""
        return f"power-shell timeout -o {old}" if old else "power-shell timeout -r"

    @contextlib.contextmanager
    def keep_awake(self, every: float = 20, report=None):
        """The phone's screen kept on while the block runs: every `every` seconds (a daemon thread) the phone is woken
        and its screen-off time overridden to AWAKE_MS (minutes, not never: a build killed meanwhile leaves a phone
        that still turns its screen off); afterwards, however the block ends, the screen-off time it had is put back
        (restore_command). report(on, restore command), if given, hears when this starts and ends, so whoever may kill
        the build (the app's 停止) can put it back too. Only with a picked target, and always that phone, whatever this
        object is pointed at meanwhile."""
        if not self.target:
            yield
            return
        phone = copy.copy(self)
        restore = self.restore_command(override_timeout(phone.shell("hidumper -s PowerManagerService -a -s", timeout=20)))
        stop = threading.Event()

        def wake() -> None:
            while not stop.is_set():
                phone.shell(f"power-shell wakeup; power-shell timeout -o {AWAKE_MS}", timeout=15)
                stop.wait(every)

        t = threading.Thread(target=wake, name="nx2h-keep-awake", daemon=True)
        t.start()
        if report:
            report(True, restore)
        try:
            yield
        finally:
            stop.set()
            t.join(timeout=20)
            phone.shell(restore, timeout=20)
            if report:
                report(False, restore)

    def mark_game(self, bundle: str, on: bool = True) -> tuple:
        """The system's game mark for an app, as native-harmonyos/oot/game_mode.ps1 (in the Ocarina of Time backend)
        sets it: the game service (system ability 66006) keeps a list of the apps it treats as games, and gives them its
        game policy (the GPU held at a high frequency, among others). The system keeps the mark until it is taken off
        (on=False). Returns (whether the phone confirmed it, what it said)."""
        if not _BUNDLE.fullmatch(bundle or ""):
            return False, f"bundle name {bundle!r}"
        self.shell("cem publish -e game.assistant.custom.deviceTemplate", timeout=20)
        time.sleep(0.5)
        if on:
            out = self.shell(f"hidumper -s 66006 -a '-se {bundle}'", timeout=30)
            return f"set bundleName={bundle} isGame=1 success." in out, out
        r1 = self.shell(f"hidumper -s 66006 -a '-sd {bundle}'", timeout=30)
        r2 = self.shell(f"hidumper -s 66006 -a '-sc {bundle}'", timeout=30)
        return "isGame=0 success" in r1 and "cancel game recognition" in r2, f"{r1} | {r2}"
