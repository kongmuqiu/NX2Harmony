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
DEVECO_URL = "https://developer.huawei.com/consumer/cn/deveco-studio/"
NEEDS_JAVA = {"huawei"}   # fixes that lead to signing, which runs keytool (device.java_home): never before a Java is found
NO_JAVA_YET = "先装好 Java 再登录"
NO_DEVECO_YET = "先装好 DevEco Studio 再登录"   # no toolchain at all: DevEco Studio brings its own Java (jbr)
JAVA_PKG = "Microsoft.OpenJDK.21"   # the JDK the HarmonyOS Command Line Tools 26 are recommended with

_listener = None   # who hears the fixes' progress (listen()); None: a console shows the tools' own output
_why = ""          # why the last fix failed: the tool's exit code and last line, or what a fix said


@dataclass
class Check:
    key: str
    label: str
    ok: Optional[bool]          # None: only information
    detail: str = ""
    fix: Optional[Callable[[], bool]] = None
    how: str = ""


def _run(cmd, timeout=60) -> subprocess.CompletedProcess:
    try:
        return proc.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout)
    except (OSError, subprocess.SubprocessError) as e:
        return subprocess.CompletedProcess(cmd, 1, "", str(e))


def listen(report) -> None:
    """report(dict) hears the fixes that run next, from the thread they run on (the desktop app's 环境 page):
    {"kind": "item", "label", "n", "of"} as a fix starts; "line" / "progress" {"text"} the running tool's output (a
    progress line redraws the one before it); "ok" / "fail" / "skip" {"label", "why"} how a fix ended; "recheck" when
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


def _winget(pkg: str) -> Callable[[], bool]:
    def fix() -> bool:
        _say(f"  winget install {pkg} ...")
        return _tool(["winget", "install", "-e", "--id", pkg, "--silent", "--accept-package-agreements",
                      "--accept-source-agreements"])
    return fix


def docker_server() -> str:
    r = _run(["docker", "version", "--format", "{{.Server.Version}}"], timeout=30)
    return r.stdout.strip() if r.returncode == 0 else ""


def _start_docker() -> bool:
    if not shutil.which("docker") and not DOCKER_DESKTOP.is_file():
        return _winget("Docker.DockerDesktop")()
    if DOCKER_DESKTOP.is_file():
        _say("  启动 Docker Desktop ...")
        subprocess.Popen([str(DOCKER_DESKTOP)])
        for i in range(60):
            time.sleep(3)
            if docker_server():
                return True
            if i % 5 == 4:
                _say(f"  等 Docker Desktop 就绪（{(i + 1) * 3} 秒）")
        return bool(docker_server()) or _fail("Docker Desktop 3 分钟内没有就绪：打开它看看是否要先装 WSL 2 或重启电脑")
    return bool(docker_server()) or _fail("docker 服务没有响应：先手动打开 Docker Desktop")


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
                  _winget(JAVA_PKG), f"winget install {JAVA_PKG}")]


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
        out.insert(0, Check("git", "Git（取参考仓库用）", False, "未安装", _winget("Git.Git"), "winget install Git.Git"))
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
                     _winget("Python.Python.3.12"), "winget install Python.Python.3.12"))
    srv = docker_server()
    out.append(Check("docker", "Docker Desktop 在运行", bool(srv), f"服务端 {srv}" if srv else
                     ("已安装但没在运行" if DOCKER_DESKTOP.is_file() or shutil.which("docker") else "未安装"),
                     _start_docker, "winget install Docker.DockerDesktop，装好后启动一次（WSL 2）"))
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
                                 "winget install Meta.Zstandard"))
                break
        from .backends.common import BaseBackend
        opus = BaseBackend.PREBUILT["native-opus"]   # the package's prebuilt libopus (backends/prebuilt-ohos) needs no build
        if any(not (be.root / "artifacts" / opus).is_file() and not (be.root.parent / "prebuilt-ohos" / opus).is_file()
               for be in bes):
            pw = os.environ.get("TOTK_PWSH") or shutil.which("pwsh")
            out.append(Check("pwsh", "PowerShell 7（编译 libopus 用）", bool(pw), pw or "未安装",
                             _winget("Microsoft.PowerShell"), "winget install Microsoft.PowerShell"))
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
                         fixable=bool(c.fix) and c.ok is False)
        else:
            print(line, flush=True)


def fix(cs: list, report=None) -> list:
    """Run the automatic fixes of the failed checks; returns the checks again. report (else the listen() one) hears
    how each goes. The fixes that lead to signing wait for a Java (keytool): skipped, with a note, while the Java
    check fails."""
    global _listener, _why
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
            try:
                ok = bool(c.fix())
            except Exception as e:   # one fix failing must stop neither the others nor the check after them
                ok, _why = False, str(e) or type(e).__name__
            if ok:
                _note("ok", label=c.label)
            else:
                why = _why or "没有装好（原因不明）"
                print(f"  没装好：{why}", flush=True)
                _note("fail", label=c.label, why=why)
            config.refresh_path()   # what winget just installed (Python, zstd ...) is on PATH for the next fix and checks
        _note("recheck")
        return checks()
    finally:
        _listener = before
