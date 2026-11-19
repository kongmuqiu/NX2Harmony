"""Is this PC ready to build? Each check says what is missing and, where a program can be installed without an account,
installs it with winget (`setup --fix`). DevEco Studio, the signing and the phone's developer mode need the player
(a Huawei account, a dialog on the phone) and are explained instead. Keys and signing files are only looked for, never
opened for their values."""
from __future__ import annotations

import locale
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from . import config
from . import device
from . import proc
from .device import Hdc

DOCKER_DESKTOP = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Docker" / "Docker" / "Docker Desktop.exe"
DOCKER_SETTINGS = Path(os.environ.get("APPDATA", "")) / "Docker" / "settings-store.json"
DOCKER_INSTALL_SETTINGS = Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "DockerDesktop" / "install-settings.json"
DOCKER_INSTALL_LOGS = (Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "DockerDesktop" / "install-log-admin.txt",
                       Path(os.environ.get("LOCALAPPDATA", "")) / "Docker" / "install-log.txt")
WSL_EXE = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "WSL" / "wsl.exe"   # the WSL 2 package's own
WSL_INBOX = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "wsl.exe"     # Windows' own, installs it
WSL_KEY = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss\MSI"
WSL_MIN = (2, 1, 5)   # the oldest WSL Docker Desktop starts on (its system requirements)
DOCKER_WAIT = 300     # seconds the one-click fix waits for a Docker Desktop it started: a first start sets up its WSL distro
DEVECO_URL = "https://developer.huawei.com/consumer/cn/deveco-studio/"
NEEDS_JAVA = {"huawei"}   # fixes that lead to signing, which runs keytool (device.java_home): never before a Java is found
NO_JAVA_YET = "先装好 Java 再登录"
NO_DEVECO_YET = "先装好 DevEco Studio 再登录"   # no toolchain at all: DevEco Studio brings its own Java (jbr)
JAVA_PKG = "Microsoft.OpenJDK.21"   # the JDK the HarmonyOS Command Line Tools 26 are recommended with

_listener = None   # who hears the fixes' progress (listen()); None: a console shows the tools' own output
_why = ""          # why the last fix failed: the tool's exit code and last line, or what a fix said
_restart = ""      # what this run installed that works only once Windows has restarted (WSL); '' while nothing did
_waits = ""        # why the last fix stopped to wait for a restart of Windows (_wait): not a failure
ADMIN_SHELL = "右键开始菜单打开“终端(管理员)”（Windows 10 是“Windows PowerShell(管理员)”）"


@dataclass
class Check:
    key: str
    label: str
    ok: Optional[bool]          # None: only information
    detail: str = ""
    fix: Optional[Callable[[], bool]] = None
    how: str = ""
    verb: str = "安装"          # what the fix does, as the 环境 page says it: 可自动安装 / 可自动启动


def _run(cmd, timeout=60) -> subprocess.CompletedProcess:
    try:
        return proc.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout)
    except (OSError, subprocess.SubprocessError) as e:
        return subprocess.CompletedProcess(cmd, 1, "", str(e))


def listen(report) -> None:
    """report(dict) hears the fixes that run next, from the thread they run on (the desktop app's 环境 page):
    {"kind": "item", "label", "n", "of"} as a fix starts; "line" / "progress" {"text"} the running tool's output (a
    progress line redraws the one before it); "ok" / "fail" / "skip" / "wait" {"label", "why"} how a fix ended ("wait":
    it did what it could, the rest needs a restart of Windows); "recheck" when
    the checks run again. None: no one listens, and a console shows the tools' own output as before."""
    global _listener
    _listener = report


def _note(kind: str, **fields) -> None:
    if _listener:
        try:
            _listener(dict(kind=kind, **fields))
        except Exception:   # a window that went away must not stop an install
            pass


def _say(text: str) -> None:
    """A fix's own message: printed for a console, and passed to the listener."""
    print(text, flush=True)
    _note("line", text=text.strip())


def _fail(why: str) -> bool:
    global _why
    _why = why
    return False


def _wait(why: str) -> bool:
    """A fix that did what it could and must wait for a restart of Windows for the rest (Docker Desktop after WSL):
    reported as waiting, not as failed."""
    global _waits
    _waits = why
    return _fail(why)


_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def _clean(raw: bytes) -> str:
    """One line of a tool's output as a console would leave it: decoded (UTF-8, else the system code page), colour
    codes dropped, backspaces applied (winget's spinner erases itself); '' for nothing but a spinner."""
    try:
        s = raw.decode("utf-8")
    except UnicodeDecodeError:
        s = raw.decode(locale.getpreferredencoding(False) or "mbcs", "replace")
    out = []
    for ch in _ANSI.sub("", s):
        if ch == "\b":
            if out:
                out.pop()
        elif ch == "\t":
            out.append(" ")
        elif ch >= " ":
            out.append(ch)
    s = "".join(out).strip()
    return s if len(s.strip("-\\|/ ")) > 1 else ""


def _relay(stream, tail: list) -> None:
    """A tool's output to the listener: whole lines as "line", lines redrawn in place with a carriage return
    (winget's and git's progress bars) as "progress", about four a second. `tail` keeps the last whole lines."""
    buf, shown = b"", 0.0
    for chunk in iter(lambda: stream.read1(65536), b""):
        data = buf + chunk
        hold = b"\r" if data.endswith(b"\r") else b""   # perhaps the first half of a \r\n
        parts = re.split(rb"(\r\n|\n|\r)", data[:len(data) - len(hold)])
        buf = parts.pop()[-65536:] + hold
        for raw, sep in zip(parts[0::2], parts[1::2]):
            line = _clean(raw)
            if not line:
                continue
            if sep == b"\r":
                now = time.monotonic()
                if now - shown >= 0.25:
                    shown = now
                    _note("progress", text=line)
            else:
                tail.append(line)
                del tail[:-5]
                _note("line", text=line)
    line = _clean(buf)
    if line:
        tail.append(line)
        _note("line", text=line)


def _tool(cmd) -> bool:
    """Run an install tool (winget, upstream.py). With no listener it runs as before (its output in the console when
    there is one); while a window listens its output is read here and passed on. When it fails, _why says why: the
    exit code and the tool's last line."""
    global _why
    _why = ""
    last = ""
    try:
        if not _listener:
            rc = proc.run_visible(cmd).returncode
        else:
            p = proc.popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            tail = []
            reader = threading.Thread(target=_relay, args=(p.stdout, tail), daemon=True)
            reader.start()
            rc = p.wait()
            reader.join(5)   # a program it started may hold the pipe open: do not wait for that one
            last = tail[-1] if tail else ""
    except OSError as e:
        return _fail(f"启动不了 {Path(str(cmd[0])).name}：{e.strerror or e}")
    if rc != 0:
        code = str(rc) if -256 < rc < 256 else f"0x{rc & 0xFFFFFFFF:08X}"   # winget's are HRESULTs
        return _fail(f"退出码 {code}" + (f"：{last}" if last else ""))
    return True


def winget_cmd(pkg: str) -> str:
    """The install command for a package, as the app runs it and as a player types it: from the winget source only.
    Without --source winget the Microsoft Store source is searched too; it pins its server certificate, so anything
    on the PC that replaces HTTPS certificates (an accelerator, an antivirus's HTTPS scanning, a proxy) makes the
    whole install fail with 0x8A15005E, although the package comes from the winget source."""
    return f"winget install -e --id {pkg} --source winget"


def _winget(pkg: str) -> Callable[[], bool]:
    def fix() -> bool:
        _say(f"  {winget_cmd(pkg)} ...")
        return _tool(winget_cmd(pkg).split() + ["--silent", "--accept-package-agreements",
                                                "--accept-source-agreements"])
    return fix


def docker_server() -> str:
    r = _run(["docker", "version", "--format", "{{.Server.Version}}"], timeout=30)
    return r.stdout.strip() if r.returncode == 0 else ""


def _uninstall_places(name: str) -> list:
    """InstallLocation of the uninstall entry `name` (all users, then this user)."""
    if sys.platform != "win32":
        return []
    import winreg
    out = []
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            with winreg.OpenKey(hive, rf"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{name}", 0,
                                winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
                v = winreg.QueryValueEx(k, "InstallLocation")[0]
        except OSError:
            continue
        if isinstance(v, str) and v.strip(' "'):
            out.append(v.strip(' "'))
    return out


def docker_desktop_exe() -> Optional[Path]:
    """Docker Desktop's program: for all users (winget's install) in Program Files, for this user only (what its own
    installer picks since 4.83) in %LOCALAPPDATA%\\Programs\\DockerDesktop, else where its uninstall entry says."""
    places = [DOCKER_DESKTOP, Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "DockerDesktop" / "Docker Desktop.exe"]
    places += [Path(p) / "Docker Desktop.exe" for p in _uninstall_places("Docker Desktop")]
    cli = shutil.which("docker")   # <folder>\resources\bin\docker.exe
    if cli:
        places.append(Path(cli).resolve().parent.parent.parent / "Docker Desktop.exe")
    return next((p for p in places if p.is_file()), None)


def docker_installed() -> bool:
    return docker_desktop_exe() is not None or bool(shutil.which("docker"))


def _json_file(path: Path) -> dict:
    import json
    try:
        d = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return {}
    return d if isinstance(d, dict) else {}


def _docker_settings() -> dict:
    """Docker Desktop's settings, keys in lower case: what its installer was told (install-settings.json, for all users;
    imported at the first start), overridden by the player's own (%APPDATA%\\Docker\\settings-store.json)."""
    out = {}
    for path in (DOCKER_INSTALL_SETTINGS, DOCKER_SETTINGS):
        out.update({str(k).lower(): v for k, v in _json_file(path).items()})
    return out


def docker_terms_accepted() -> bool:
    """Docker Desktop's service agreement is accepted: at install time (--accept-license, which winget's package
    passes since its 4.74 manifest) or by the player at the first start. Without it the engine does not start."""
    return bool(_docker_settings().get("licensetermsversion"))


def docker_uses_wsl() -> bool:
    """Docker Desktop runs its engine in WSL 2 (the default) unless its settings chose Hyper-V."""
    s = _docker_settings()
    engine = str(s.get("enginetypewindows") or "").lower()
    return s.get("wslengineenabled", True) is not False and (not engine or "wsl" in engine)


def wsl_version() -> Optional[tuple]:
    """The version of the WSL 2 package (what Docker Desktop needs), None when it is not installed. Read from the
    registry, never by running wsl.exe: without the package, Windows' own wsl.exe asks for a key press and then
    downloads and installs it."""
    if sys.platform != "win32" or not WSL_EXE.is_file():
        return None
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, WSL_KEY, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            v = str(winreg.QueryValueEx(k, "Version")[0])
    except OSError:
        return WSL_MIN   # wsl.exe without its record: the package layout of WSL 2.0 and later, its version unknown
    nums = re.findall(r"\d+", v)
    return tuple(int(x) for x in nums[:3]) if nums else WSL_MIN


def _wsl_ok() -> bool:
    v = wsl_version()
    return v is not None and v >= WSL_MIN


_plat = (0.0, {})   # platform_state's last reading and when


def platform_state(fresh: bool = False) -> dict:
    """What WSL 2 runs on, read through CIM, which needs no administrator rights (Get-WindowsOptionalFeature and DISM
    do): hypervisor (one runs: virtualization is on), firmware (the CPU's virtualization switched on in the firmware;
    Windows reports it only while no hypervisor runs), vmp and wsl_oc (InstallState of the Virtual Machine Platform and
    of the WSL optional component, which Windows 10 needs too: 1 on, 2 off, None unknown). {} when it cannot be read
    (a broken WMI prints empty fields). A reading is reused for 15 seconds unless `fresh` (after an install)."""
    global _plat
    if sys.platform != "win32":
        return {}
    if not fresh and time.monotonic() - _plat[0] < 15:
        return dict(_plat[1])
    import base64
    script = ("$c = Get-CimInstance Win32_ComputerSystem; $p = Get-CimInstance Win32_Processor | Select-Object -First 1; "
              "$f = @{}; Get-CimInstance Win32_OptionalFeature -Filter \"Name='VirtualMachinePlatform' OR "
              "Name='Microsoft-Windows-Subsystem-Linux'\" | ForEach-Object { $f[$_.Name] = $_.InstallState }; "
              "'{0}|{1}|{2}|{3}' -f $c.HypervisorPresent, $p.VirtualizationFirmwareEnabled, "
              "$f['VirtualMachinePlatform'], $f['Microsoft-Windows-Subsystem-Linux']")
    r = _run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand",
              base64.b64encode(script.encode("utf-16-le")).decode("ascii")], timeout=60)
    parts = [p.strip() for p in r.stdout.strip().splitlines()[-1].split("|")] if r.returncode == 0 and r.stdout.strip() else []
    out = {}
    if len(parts) == 4 and all(p in ("True", "False") for p in parts[:2]):
        out = {"hypervisor": parts[0] == "True", "firmware": parts[1] == "True",
               "vmp": int(parts[2]) if parts[2].isdigit() else None,
               "wsl_oc": int(parts[3]) if parts[3].isdigit() else None}
    _plat = (time.monotonic(), out)
    return dict(out)


def _features_off(plat: dict) -> str:
    """The Windows feature WSL 2 needs that is known to be off, by its name in Windows' feature list: the Virtual
    Machine Platform, and on Windows 10 the WSL optional component too; '' when none is known to be."""
    if not plat:
        return ""
    if plat.get("vmp") not in (1, None):
        return "“虚拟机平台”"
    win10 = sys.getwindowsversion().build < 22000 if sys.platform == "win32" else False
    return "“适用于 Linux 的 Windows 子系统”" if win10 and plat.get("wsl_oc") not in (1, None) else ""


def _boot_time() -> float:
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.GetTickCount64.restype = ctypes.c_ulonglong
    return time.time() - kernel32.GetTickCount64() / 1000


def _docker_install_wants_restart() -> bool:
    """Docker Desktop's installer turned on Windows features since Windows last started: they work after a restart.
    Its exit code does not say so (winget reports success), its log does."""
    if sys.platform != "win32":
        return False
    for log in DOCKER_INSTALL_LOGS:
        try:
            if log.stat().st_mtime < _boot_time():
                continue
            text = re.split(r"-{4,}>8", log.read_text(encoding="utf-8", errors="replace"))[-1]
        except OSError:
            continue
        if re.search(r"require[sd]? (a )?(computer )?restart|must restart", text, re.I):
            return True
    return False


def _wsl_restart_due() -> bool:
    """WSL was installed (by this run, or by an earlier one: nx2h.local.json's stamp) and Windows has not restarted since."""
    if _restart:
        return True
    try:
        stamp = float(config.local().get("wsl_installed_at") or 0)
    except (TypeError, ValueError):
        stamp = 0
    return bool(stamp) and sys.platform == "win32" and stamp > _boot_time()


def restart_due() -> str:
    """Why Windows must restart before WSL 2 and Docker Desktop can run, '' when nothing says so: WSL was installed,
    or Docker Desktop's installer turned on the virtualization features since Windows last started."""
    if _wsl_restart_due():
        return "WSL 刚装好"
    if _docker_install_wants_restart():
        return "Docker Desktop 安装时打开了 Windows 的虚拟化组件"
    return ""


def reboot_pending() -> bool:
    """Windows' servicing waits for a restart (features turned on or off, updates). Only a hint: an update waiting for
    a restart sets it too."""
    if sys.platform != "win32":
        return False
    import winreg
    try:
        winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                       r"SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending", 0,
                       winreg.KEY_READ | winreg.KEY_WOW64_64KEY).Close()
        return True
    except OSError:
        return False


def server_service_off() -> bool:
    """The Server service (LanmanServer) is disabled, which Docker Desktop needs; tools that close port 445 do it."""
    if sys.platform != "win32":
        return False
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Services\LanmanServer", 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            return winreg.QueryValueEx(k, "Start")[0] == 4   # SERVICE_DISABLED
    except OSError:
        return False


def _elevated(exe, args: str, verb: str = "runas") -> int:
    """`exe args` as administrator, in a console window of its own (Windows asks the player first, a UAC prompt);
    waits for it. Its exit code, or -1 when the player said no or it could not start. (verb "open": the same without
    administrator rights, for a test.)"""
    if not Path(exe).is_file():   # ShellExecuteEx would show its own "cannot find" message box and wait on it
        return -1
    import ctypes
    from ctypes import wintypes

    class Info(ctypes.Structure):   # SHELLEXECUTEINFOW
        _fields_ = [("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
                    ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                    ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                    ("dwHotKey", wintypes.DWORD), ("hIconOrMonitor", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]
    shell32, kernel32 = ctypes.WinDLL("shell32", use_last_error=True), ctypes.WinDLL("kernel32", use_last_error=True)
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(Info)]
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    info = Info(cbSize=ctypes.sizeof(Info), fMask=0x40 | 0x100,   # SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC
                lpVerb=verb, lpFile=str(exe), lpParameters=args, nShow=1)
    if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
        return -1   # 1223 ERROR_CANCELLED: the player said no
    try:
        kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
        code = wintypes.DWORD()
        kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
        return code.value
    finally:
        kernel32.CloseHandle(info.hProcess)


def _code(rc: int) -> str:
    return str(rc) if -256 < rc < 256 else f"0x{rc & 0xFFFFFFFF:08X}"


def _install_wsl() -> bool:
    """WSL 2 for Docker Desktop, as administrator (a UAC prompt): `wsl --install --no-distribution` (no Linux
    distribution: Docker Desktop brings its own) installs the WSL package, from the Microsoft Store (else, with
    --web-download, from GitHub), and turns on the Virtual Machine Platform; it works once Windows has restarted.
    Windows' own wsl.exe runs it while there is no package, the package's when only the features are off. One too old
    gets `wsl --update`, which needs no restart."""
    global _restart
    v = wsl_version()
    had = v is not None
    win_build = sys.getwindowsversion().build if sys.platform == "win32" else 0
    if v is not None and v < WSL_MIN:
        exe, args, update = WSL_EXE, "--update", True
    else:
        exe, args, update = (WSL_EXE if v is not None else WSL_INBOX), "--install --no-distribution", False
    _say("  安装 WSL：Windows 会问是否允许更改，点“是”。新窗口里显示下载进度，装完自己关上；"
         "如果它说“按任意键安装”，按一下任意键")
    declined = "没有得到管理员权限（UAC 里点了“否”？），WSL 没有装"

    def done(code: int) -> bool:   # a package that was there already says nothing about this run
        return code in (0, 3010) or ((update or not had) and _wsl_ok())
    rc = _elevated(exe, args)
    if rc == -1:
        return _fail(declined)
    web = exe == WSL_EXE or win_build < 26100
    if not done(rc) and web:   # the Microsoft Store unreachable: the same from GitHub
        _say(f"  没装上（退出码 {_code(rc)}），改从 GitHub 下载 ...")
        rc = _elevated(exe, args + " --web-download")
        if rc == -1:
            return _fail(declined)
    if not done(rc):
        return _fail(f"WSL 没装好（退出码 {_code(rc)}）：{ADMIN_SHELL}，运行 {'wsl --update' if update else 'wsl --install --no-distribution'}，"
                     "装完重启电脑")
    if update:
        _say("  WSL 更新好了")
        return True
    plat = platform_state(fresh=True)   # 3010, a package not there yet (the Store's), features off: Windows must restart
    if rc == 3010 or not _wsl_ok() or (plat and (not plat["hypervisor"] or _features_off(plat))):
        _restart = "WSL"
        try:
            config.save_local(wsl_installed_at=time.time())
        except OSError:
            pass
        _say("  WSL 装好了：重启电脑以后才能用")
    else:
        _say("  WSL 装好了")
    return True


def _docker_blocked() -> tuple:
    """What keeps Docker Desktop from starting, found before it is started: (why, a restart is all it needs), ('', False)
    when nothing is known to."""
    due = restart_due()
    if due:
        return f"{due}，要重启电脑后才能用：重启以后打开 NX2Harmony，再点「一键安装缺少的」启动 Docker Desktop", True
    if docker_uses_wsl() and not _wsl_ok():
        return "没有 WSL 2：先装上面的 WSL，重启电脑后再启动 Docker Desktop", False
    plat = platform_state()
    off = _features_off(plat) if docker_uses_wsl() else ""
    if off:
        return f"Windows 的{off}没打开：先装上面的 WSL（会打开它），然后重启电脑", False
    if plat and not plat["hypervisor"] and not plat["firmware"]:
        return "电脑的 BIOS/UEFI 里没开虚拟化（Intel VT-x 或 AMD SVM），或者 CPU 不支持：进 BIOS 打开后再启动 Docker Desktop", False
    if plat and not plat["hypervisor"]:
        if reboot_pending():
            return "Windows 在等重启：重启电脑后再点「一键安装缺少的」", True
        return ("Windows 的虚拟机监控程序没在运行：先重启电脑；还不行的话，可能是安卓模拟器或某些游戏把它关了，"
                f"{ADMIN_SHELL}，运行 bcdedit /set hypervisorlaunchtype auto，再重启电脑"), False
    return "", False


def _docker_wait_why(started: Optional[subprocess.Popen]) -> str:
    """Why a Docker Desktop that was started does not answer, as far as can be told without it, the likeliest first."""
    why, _ = _docker_blocked()
    if why:
        return why
    if server_service_off():
        return "Windows 的 Server 服务（LanmanServer）被禁用了，Docker Desktop 要用它（关 445 端口的工具常会禁用它）：" \
               "在 services.msc 里把它设成自动并启动，再打开 Docker Desktop"
    if not docker_terms_accepted():
        return "Docker Desktop 的服务协议还没接受：在它的窗口里点 Accept（接受），登录可以跳过，然后点「重新检查」"
    if started is not None and started.poll() is not None and not _running("Docker Desktop.exe"):
        return "Docker Desktop 打开后又退出了：看它弹出的错误窗口，按提示处理后点「重新检查」"
    return f"Docker Desktop {DOCKER_WAIT // 60} 分钟内没有就绪：看它窗口里的提示，左下角显示 Engine running 后点「重新检查」"


def _running(image: str) -> bool:
    r = _run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"], timeout=30)
    return image.lower() in r.stdout.lower()


def _start_docker() -> bool:
    """Docker Desktop installed when it is missing, then started, and waited for until its engine answers: one click
    from nothing to a running Docker. It cannot start without WSL 2 (the WSL row's fix runs before this one) or before
    Windows has restarted after WSL or Docker Desktop's installer turned the virtualization features on."""
    if not docker_installed():
        if not _winget("Docker.DockerDesktop")():
            return False
        config.refresh_path()
    if docker_server():
        return True
    why, restart = _docker_blocked()
    if why:
        return _wait(why) if restart else _fail(why)
    exe = docker_desktop_exe()
    if exe is None:
        return _fail("docker 服务没有响应：先手动打开 Docker Desktop")
    _say("  启动 Docker Desktop：如果弹出服务协议，点 Accept（接受）；登录可以跳过")
    try:
        started = subprocess.Popen([str(exe)])
    except OSError as e:
        return _fail(f"打不开 Docker Desktop：{e.strerror or e}")
    for i in range(DOCKER_WAIT // 3):
        time.sleep(3)
        if docker_server():
            return True
        if i % 5 == 4:
            _say(f"  等 Docker Desktop 就绪（{(i + 1) * 3} 秒）：它的窗口里有要点的（Accept、跳过登录）就点一下")
    return bool(docker_server()) or _fail(_docker_wait_why(started))


def network_place(path) -> bool:
    """True when `path` is on a network share (\\\\server\\share) or a mapped network drive (DRIVE_REMOTE)."""
    s = str(path).replace("/", "\\")
    if s.upper().startswith("\\\\?\\UNC\\"):
        return True
    if s.startswith(("\\\\?\\", "\\\\.\\")):
        s = s[4:]
    elif s.startswith("\\\\"):
        return True
    if sys.platform != "win32" or len(s) < 2 or s[1] != ":":
        return False
    import ctypes
    return ctypes.windll.kernel32.GetDriveTypeW(s[:2] + "\\") == 4   # DRIVE_REMOTE


def sdk_dir() -> Path:
    """The HarmonyOS native SDK the backend builds with (its TOTK_OHOS_SDK setting, else the one inside the DevEco
    Studio in use: device.find_deveco)."""
    p = os.environ.get("TOTK_OHOS_SDK") or device._backend_env().get("TOTK_OHOS_SDK")
    return Path(p) if p else device.deveco_root() / "sdk/default/openharmony/native"


def _inside(path: Path, folder: Path) -> bool:
    try:
        Path(os.path.abspath(path)).relative_to(os.path.abspath(folder))
        return True
    except ValueError:
        return False


def deveco_detail(found, hdc: Hdc, sdk: Path, ok: bool) -> str:
    """The DevEco row: which DevEco Studio the builds use and its SDK version
    (D:\\Program Files\\Huawei\\DevEco Studio（SDK 26.0.0.105）), what is missing, and the other installs found."""
    use, parts = found.use, []
    if found.ignored:
        parts.append(f"nx2h.local.json 的 deveco 不是 DevEco Studio 文件夹（{found.ignored}），改用找到的")
    if use:
        parts.append(use.label())
    if not ok:
        if not use and not hdc.available():
            parts.append("未找到")
        elif not hdc.available():
            parts.append(f"缺 hdc：{hdc.exe}")
        elif not sdk.is_dir() and (not use or use.version):
            parts.append(f"缺 SDK：{sdk}")
    if ok and (not use or not _inside(sdk, use.root)):
        parts.append(f"原生 SDK：{sdk}")   # TOTK_OHOS_SDK names another one
    others = [d.label() for d in found.installs if d != use]
    if others:
        parts.append("另有（未用）：" + "、".join(others))
    return "；".join(parts)


def signing_checks(ready: bool = True, needs_java: bool = False) -> list:
    """The Huawei account the background signing uses (agc.py). Each game's own signing (its bundle name, bundle.py)
    is not a row here: a build asks for it in the background when it is missing (cli.py), and says so if that fails.
    Until `ready` the login is not offered: the signing after it runs keytool. What is missing then (checks()): with
    DevEco Studio's jbr or no toolchain at all, DevEco Studio and its SDK, as before; with a toolchain that has no jbr
    (`needs_java`), a Java (device.java_home)."""
    from . import agc
    acct = agc.account()

    def do_login() -> bool:
        try:
            agc.login(say=_say)
            return True
        except agc.SigningError as e:
            _say("  " + str(e))
            return _fail(str(e))
    logged_in = bool(acct.get("jwt"))
    if logged_in or ready:
        login, how = (None if logged_in else do_login), "在浏览器里登录华为账号（开发者账号，需实名认证）"
    elif needs_java:
        login, how = None, NO_JAVA_YET + "（签名要用 Java 的 keytool），然后在这里登录华为账号"
    else:
        login, how = None, NO_DEVECO_YET + "（签名要用 DevEco Studio 自带的 keytool），然后在这里登录华为账号"
    out = [Check("huawei", "华为账号（签名用）", logged_in,
                 (acct.get("userName") or acct.get("userId", "")) + (f" · 团队 {acct.get('teamName')}" if acct.get("teamName") else "")
                 if logged_in else "未登录：第一次构建时会在浏览器里打开登录页", login, how)]
    return out


def java_checks(found, java_ok: bool) -> list:
    """The Java row, only when the toolchain in use (found.use) is there but has no Java of its own (jbr) and none is
    found elsewhere (device.java_home, JAVA_MIN or newer). With DevEco Studio's jbr, or with no toolchain at all,
    there is no such row (java_home is the jbr, or the way on is DevEco Studio)."""
    if java_ok or not found.use:
        return []
    return [Check("java", "Java（签名和打包用）", False,
                  f"未找到：{found.use.root} 里没有 jbr，PATH、JAVA_HOME 和常见安装位置也没有 Java {device.JAVA_MIN} 或更新的版本",
                  _winget(JAVA_PKG), winget_cmd(JAVA_PKG))]


def gb_text(free: float, need: float) -> str:
    """Free space in GB for a row or a message: rounded down, with one decimal within 1 GB of what is needed (79.6 GB
    free is not "80" next to "at least 80")."""
    return f"{int(free * 10) / 10:.1f}" if abs(free - need) < 1 else str(int(free))


def disk_short(root, need: float, what: str) -> str:
    """'' when the disk `root` is on has `need` GB free, else what to tell the player (cli.cmd_build, before the first
    step)."""
    drive = Path(root).drive or str(root)
    try:
        free = shutil.disk_usage(drive + "\\").free / 2**30
    except OSError:
        return ""
    if free >= need:
        return ""
    name = f"{drive[0].upper()} 盘" if re.fullmatch(r"[A-Za-z]:", drive) else drive
    return f"{name}剩余 {gb_text(free, need)} GB，准备{what}约要 {need} GB：先清理空间，或把软件放到空间更大的盘"


def wsl_checks() -> list:
    """The WSL 2 row, while Docker Desktop does not answer and runs its engine in WSL (the default): Docker Desktop
    cannot start without it ("WSL not installed") and Windows' own wsl.exe only offers to install it on a key press.
    Once this run installed it, the row says to restart Windows (it works only after that)."""
    if not docker_uses_wsl():
        return []
    v = wsl_version()
    label = "WSL 2（Docker Desktop 要用）"
    how = f"{ADMIN_SHELL}，运行 wsl --install --no-distribution，装完重启电脑"
    if v is None:   # installed from the Microsoft Store, the package may show only after the restart
        if _wsl_restart_due():
            return [Check("wsl", label, False, "已安装，重启电脑后才能用", None,
                          "重启电脑，再打开 NX2Harmony 点「一键安装缺少的」启动 Docker Desktop")]
        return [Check("wsl", label, False, "未安装", _install_wsl, how)]
    ver = ".".join(map(str, v))
    if v < WSL_MIN:
        return [Check("wsl", label, False, f"{ver} 太旧（要 {'.'.join(map(str, WSL_MIN))} 或更新）",
                      _install_wsl, f"{ADMIN_SHELL}，运行 wsl --update")]
    due = restart_due()
    if due:
        return [Check("wsl", label, False, f"{ver}；{due}，重启电脑后才能用", None,
                      "重启电脑，再打开 NX2Harmony 点「一键安装缺少的」启动 Docker Desktop")]
    off = _features_off(platform_state())
    if off:
        return [Check("wsl", label, False, f"{ver}，但 Windows 的{off}没打开", _install_wsl, how)]
    return [Check("wsl", label, True, ver)]


def docker_check(srv: str) -> Check:
    """The Docker row. Installed but not answering, its fix starts it (可自动启动) and the row says how to open it by
    hand; missing, the fix installs it and then starts it."""
    label = "Docker Desktop 在运行"
    if srv:
        return Check("docker", label, True, f"服务端 {srv}")
    if docker_installed():
        blocked, _ = _docker_blocked()
        if blocked:   # nothing to click: a restart, WSL (its own row), the firmware
            return Check("docker", label, False, "已安装但没在运行", None, blocked, "启动")
        why = "" if docker_terms_accepted() else "（还没接受它的服务协议）"
        return Check("docker", label, False, "已安装但没在运行" + why, _start_docker,
                     "从开始菜单打开 Docker Desktop：弹出服务协议就点 Accept，登录可以跳过；左下角显示 Engine running "
                     "后点「重新检查」", "启动")
    return Check("docker", label, False, "未安装", _start_docker,
                 winget_cmd("Docker.DockerDesktop") + "，装好后打开一次（要先有 WSL 2）")


UPSTREAM_STATES = {1: "没有参考仓库", 2: "不在固定提交"}


def upstream_checks(backend: str, root: Path, code: int) -> list:
    """The 参考仓库 row when upstream.py --check did not return 0 (1 missing, 2 another commit), in Chinese (its own
    output is English). The package's own copy (NX2HARMONY-UPSTREAM.txt, no .git) cannot be repaired here: download the
    package again. A git checkout (a developer's) is repaired by upstream.py: cloned when missing (that needs Git: a Git
    row then, only when git is missing), moved to the pin with --update (refused with local changes)."""
    from .backends import load
    from .events import Events
    from .backends.za_202 import UPSTREAM_MARKER
    try:
        up = load(backend)(Events(), Hdc()).upstream()
    except Exception:   # the backend's own lookup failed: where ohos_env.py looks without settings
        up = root.parent / "totk-android"
    state = UPSTREAM_STATES.get(code, f"检查失败（退出码 {code}）")
    label = "参考仓库 totk-android 在固定提交"
    packaged = config.FROZEN or ((up / UPSTREAM_MARKER).is_file() and not (up / ".git").exists())
    if packaged:
        return [Check("upstream", label, False, f"{state}：{up}", None,
                      "软件包里的参考仓库 totk-android 缺失或不完整：把你手上那份完整的 NX2Harmony 重新完整解压到原处")]
    script = root / "native-harmonyos" / "upstream.py"
    args = ["--update"] if code == 2 else []

    def ensure_upstream() -> bool:
        py_now = config.python()
        if not py_now:
            return _fail("先装好 Python，再重新打开 NX2Harmony")
        return _tool([py_now, "-u", "-I", str(script)] + args)   # -u: lines as they come (git's output in English)
    out = [Check("upstream", label, False, f"{state}：{up}", ensure_upstream,
                 " ".join(["python native-harmonyos/upstream.py"] + args))]
    if code == 1 and not shutil.which("git"):   # the clone needs Git; nothing else this app runs does
        out.insert(0, Check("git", "Git（取参考仓库用）", False, "未安装", _winget("Git.Git"), winget_cmd("Git.Git")))
    return out


def checks() -> list:
    config.refresh_path()   # "重新检查" also sees a tool installed outside the app since it started
    out = []
    is_win = platform.system() == "Windows" and platform.machine().upper() in ("AMD64", "X86_64")
    out.append(Check("os", "Windows 10/11 x64", is_win, platform.platform()))
    if network_place(config.APP_DIR):   # cmd.exe (CMake's link step) and ohpm's directory junctions fail there
        out.append(Check("location", "程序位置", False, f"{config.APP_DIR}（网络位置）", None,
                         "把整个 NX2Harmony 文件夹复制到本地磁盘（如 C:\\NX2Harmony）后从那里打开：在网络位置上 cmd.exe、CMake 和 ohpm 会失败或卡住"))
    py = config.python()
    pyver = sys.version.split()[0] if not config.FROZEN else (_run([py, "-c", "import sys; print(sys.version.split()[0])"]).stdout.strip() if py else "")
    py_ok = bool(pyver) and tuple(int(x) for x in pyver.split(".")[:2]) >= (3, 9)
    out.append(Check("python", "Python 3.9 或更新（运行构建脚本）", py_ok, f"{pyver} · {py}" if py_ok else "未找到",
                     _winget("Python.Python.3.12"), winget_cmd("Python.Python.3.12")))
    srv = docker_server()
    if not srv and is_win:
        out += wsl_checks()
    out.append(docker_check(srv))
    found = device.find_deveco(refresh=True)   # looked for again: DevEco Studio may have been installed meanwhile
    hdc = Hdc()
    sdk = sdk_dir()
    deveco_ok = hdc.available() and sdk.is_dir()
    out.append(Check("deveco", "DevEco Studio 和 HarmonyOS SDK", deveco_ok, deveco_detail(found, hdc, sdk, deveco_ok),
                     None, "打开一次 DevEco Studio，等 HarmonyOS SDK 下载完" if found.use else
                     f"从 {DEVECO_URL} 下载安装，登录华为账号，打开一次让 SDK 下载完"))
    java_ok = device.java_home() is not None
    out += java_checks(found, java_ok)
    # the login waits for keytool: with DevEco Studio's jbr or no toolchain at all, for DevEco Studio and its SDK as
    # before; with a toolchain without a jbr, for a Java
    no_jbr = found.use is not None and not device.is_jdk(found.use.root / "jbr", 0)
    sign_ok = java_ok if no_jbr else deveco_ok
    from .backends import MARKERS, load, missing
    from .events import Events
    ready = []
    # what the package carries (the game backends, nxnative, the prebuilt libraries) is a row only when it is missing
    again = "软件包里缺这个文件夹：把你手上那份完整的 NX2Harmony 重新完整解压到原处；" if config.FROZEN else ""
    for r in config.games():
        if r.get("status") not in config.BUILDABLE or r.get("backend") not in MARKERS:
            continue
        root = config.backend_root(r["backend"])
        marker, folder = MARKERS[r["backend"]]
        if (root / marker).is_file():
            ready.append((r, root))
        else:
            out.append(Check("backend", f"{r['name']} 后端（{folder}）", False, str(root), None, again + missing(r["backend"])))
    if ready:
        root = ready[0][1]   # every backend pins the same totk-android commit (native-harmonyos/UPSTREAM_PIN)
        # upstream.py needs a real Python: never this app's own exe (sys.executable when packed), which would open
        # another NX2Harmony window that checks again, and so on
        if py:
            up_code = _run([py, "-I", str(root / "native-harmonyos/upstream.py"), "--check"], timeout=60).returncode
        else:
            up_code = None
        if up_code:
            out += upstream_checks(ready[0][0]["backend"], root, up_code)
        elif up_code is None:
            out.append(Check("upstream", "参考仓库 totk-android 在固定提交", False, "先装好上面的 Python 再检查"))
        out += signing_checks(sign_ok, no_jbr)
        bes = [load(row["backend"])(Events(), hdc) for row, _ in ready]
        for be in bes:   # a backend whose steps still to run use seedds/nxnative (Ocarina of Time's shader tools)
            if getattr(be, "needs_nxnative", None) and be.needs_nxnative() and not be.nxnative():
                out.append(Check("nxnative", f"nxnative（{be.name}的着色器工具）", False, "未找到", None,
                                 again + be.nxnative_how()))
        for be in bes:   # a backend whose steps still to run need the zstd program (Ocarina of Time's shader list)
            if getattr(be, "needs_zstd", None) and be.needs_zstd() and not be.zstd():
                out.append(Check("zstd", f"zstd（{be.name}的着色器清单用）", False, "未安装", _winget("Meta.Zstandard"),
                                 winget_cmd("Meta.Zstandard")))
                break
        from .backends.common import BaseBackend
        opus = BaseBackend.PREBUILT["native-opus"]   # the package's prebuilt libopus (backends/prebuilt-ohos) needs no build
        if any(not (be.root / "artifacts" / opus).is_file() and not (be.root.parent / "prebuilt-ohos" / opus).is_file()
               for be in bes):
            pw = os.environ.get("TOTK_PWSH") or shutil.which("pwsh")
            out.append(Check("pwsh", "PowerShell 7（编译 libopus 用）", bool(pw), pw or "未安装",
                             _winget("Microsoft.PowerShell"), winget_cmd("Microsoft.PowerShell")))
        need_keys = any(be.needs_keys() for be in bes)
        keys = config.key_candidates()
        keys_ok = bool(keys) if need_keys else (True if keys else None)   # not needed once the game data is prepared
        out.append(Check("keys", "prod.keys（你自己的 Switch 导出）", keys_ok,
                         str(keys[0]) if keys else ("游戏数据已准备好，暂时用不到" if not need_keys else "未找到"),
                         None, "放到 %USERPROFILE%\\.switch\\prod.keys，或构建时用 -k 指定所在文件夹"))
        need = {}
        for be in bes:   # room for the biggest game still to prepare (one is built at a time; cli.cmd_build checks again)
            drive = be.root.drive or str(be.root)
            need[drive] = max(need.get(drive, 5), getattr(be, "disk_gb", 80) if be.needs_inputs() else 5)
        for drive, gb in need.items():
            try:
                free = shutil.disk_usage(drive + "\\").free / 2**30
            except OSError:
                continue
            out.append(Check("disk", f"磁盘空间（{drive} 至少 {gb} GB）", free >= gb, f"{drive} 剩余 {gb_text(free, gb)} GB"))
    tgt = hdc.pick() if hdc.available() else ""
    detail = "没有连接的手机" if not tgt else (hdc.model() or "已连接（读不到型号）")
    out.append(Check("phone", "鸿蒙手机已连接（开发者模式 + USB 调试）", bool(tgt), detail, None,
                     "手机：设置 → 关于手机 → 连点版本号打开开发者模式；系统 → 开发者选项 → 打开 USB 调试；用数据线连电脑并允许调试"))
    return out


def show(cs: list, events=None) -> None:
    for c in cs:
        mark = "✓" if c.ok else ("·" if c.ok is None else "✗")
        line = f" {mark} {c.label}：{c.detail}"
        if c.ok is False and c.how:
            line += f"\n     → {c.how}"
        if events and events.json:
            events._emit("check", key=c.key, label=c.label, ok=c.ok, detail=c.detail, how=c.how if c.ok is False else "",
                         fixable=bool(c.fix) and c.ok is False, verb=c.verb)
        else:
            print(line, flush=True)


def fix(cs: list, report=None) -> list:
    """Run the automatic fixes of the failed checks; returns the checks again. report (else the listen() one) hears
    how each goes. The fixes that lead to signing wait for a Java (keytool): skipped, with a note, while the Java
    check fails."""
    global _listener, _why, _waits
    before = _listener
    if report:
        _listener = report
    try:
        no_java = any(c.key == "java" and c.ok is False for c in cs)
        todo = []
        for c in cs:
            if c.ok is not False or not c.fix:
                continue
            if no_java and c.key in NEEDS_JAVA:
                print(f"跳过：{c.label}（{NO_JAVA_YET}）", flush=True)
                _note("skip", label=c.label, why=NO_JAVA_YET)
            else:
                todo.append(c)
        for i, c in enumerate(todo, 1):
            print(f"修复：{c.label}", flush=True)
            _note("item", label=c.label, n=i, of=len(todo))
            _why = ""
            _waits = ""
            try:
                ok = bool(c.fix())
            except Exception as e:   # one fix failing must stop neither the others nor the check after them
                ok, _why = False, str(e) or type(e).__name__
            if ok:
                _note("ok", label=c.label)
            elif _waits:
                print(f"  等重启：{_waits}", flush=True)
                _note("wait", label=c.label, why=_waits)
            else:
                why = _why or "没有装好（原因不明）"
                print(f"  没装好：{why}", flush=True)
                _note("fail", label=c.label, why=why)
            config.refresh_path()   # what winget just installed (Python, zstd ...) is on PATH for the next fix and checks
        _note("recheck")
        return checks()
    finally:
        _listener = before
