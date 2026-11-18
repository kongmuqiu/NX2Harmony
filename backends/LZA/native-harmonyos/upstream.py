"""The Android port checkout the HarmonyOS build is an overlay on (native-harmonyos/overlay replaces whole files of it,
build_ohos.py compiles the rest of games/totk from it): pinned to one commit, fetched when missing, checked otherwise.

    python upstream.py            ensure the checkout at TOTK_UPSTREAM (default ../totk-android next to this
                                      repository): clone at the pin when missing, resume a clone that stopped before its
                                      checkout, report HEAD against the pin otherwise
    python upstream.py --update   fetch and check out the pin (the checkout must be clean); shell scripts that a clone
                                      made with core.autocrlf=true left CRLF are written again LF
    python upstream.py --check    exit 0 when the checkout is at the pin, 2 otherwise (what the build scripts call)

The pin is UPSTREAM_PIN next to this file (one line: the commit). Move it only together with the overlay files, whose
reference versions it names. TOTK_UPSTREAM_UNPINNED=1 lets a build go on at another commit (a warning instead).

A bundled copy (NX2Harmony's package: the tree at the pin with LF line endings, no .git) holds NX2HARMONY-UPSTREAM.txt,
one line "<url> <commit>": with the pin's commit it counts as at the pin, and nothing is ever cloned into it.
Every git command runs with safe.directory set to the checkout (a checkout on a network or exFAT drive belongs to
another owner as git sees it) and with core.autocrlf=false, core.eol=lf (the scripts run in Linux containers). A
checkout made earlier with core.autocrlf=true has CRLF shell scripts: the steps that use the checkout's scripts make
their copies LF (xlate/build_xlate.sh, xlate/prepare_xlate_ns2.py), so it still counts as at the pin; --update writes
them again LF all the same."""
import hashlib, os, re, subprocess, sys
from pathlib import Path
if hasattr(sys.stdout, "reconfigure"):   # Chinese messages on a PC whose code page has no Chinese: '?', not a traceback
    sys.stdout.reconfigure(errors="replace")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ohos_env import UPSTREAM

HERE = Path(__file__).resolve().parent
URL = "https://github.com/AurelioB/totk-android.git"
PIN = (HERE / "UPSTREAM_PIN").read_text(encoding="utf-8").split()[0]
MARKER = "NX2HARMONY-UPSTREAM.txt"


def git_config():
    """-c options for every git command: the checkout trusted whoever owns it (as given and as resolved), LF kept. A
    network path (//server/share/...) is given as %(prefix)///server/share/...: Git for Windows reads a value that starts
    with / as relative to its own folder."""
    dirs = []
    for p in (Path(os.path.abspath(UPSTREAM)), UPSTREAM.resolve()):
        d = p.as_posix()
        if os.name == "nt" and d.startswith("//"):
            d = "%(prefix)/" + d
        if d not in dirs:
            dirs.append(d)
    out = []
    for d in dirs:
        out += ["-c", f"safe.directory={d}"]
    return out + ["-c", "core.autocrlf=false", "-c", "core.eol=lf"]


def git(*args, check=True):
    return subprocess.run(["git"] + git_config() + ["-C", str(UPSTREAM)] + list(args), capture_output=True, text=True,
                          errors="replace", check=check).stdout.strip()


def fail(zh, en, err=None):
    """Stop with what to do, in Chinese and English (and git's own message)."""
    detail = ""
    if isinstance(err, subprocess.CalledProcessError):
        detail = (err.stderr or err.stdout or "").strip()
    elif err is not None:
        detail = str(err)
    sys.exit(f"{zh}\n{en}" + (f"\n{detail}" if detail else ""))


def head():
    """The checkout's HEAD commit; None without one (no .git here: git would answer for a repository around it)."""
    if not (UPSTREAM / ".git").exists():
        return None
    try:
        return git("rev-parse", "HEAD")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def bundled():
    """The commit a bundled copy's marker names: "" for a marker that names none, None without a marker (or with
    .git)."""
    if (UPSTREAM / ".git").exists():
        return None
    try:
        words = (UPSTREAM / MARKER).read_text(encoding="utf-8").split()
    except OSError:
        return None
    return words[1].lower() if len(words) == 2 and re.fullmatch(r"[0-9a-fA-F]{40}", words[1]) else ""


def partial():
    """A clone that stopped before its checkout: .git there, but no games/totk or no HEAD commit yet."""
    return (UPSTREAM / ".git").exists() and (not (UPSTREAM / "games" / "totk").is_dir() or head() is None)


def _ls_files(*args):
    """git ls-files <args> -z -- *.sh as {path: the text before the TAB}; {} when git cannot tell."""
    try:
        r = subprocess.run(["git"] + git_config() + ["-C", str(UPSTREAM), "ls-files", "-z", *args, "--", "*.sh"],
                           capture_output=True)
    except FileNotFoundError:
        return {}
    out = {}
    for entry in r.stdout.decode("utf-8", "replace").split("\0") if r.returncode == 0 else []:
        info, tab, path = entry.partition("\t")
        if tab:
            out[path] = info
    return out


def crlf_scripts():
    """The shell scripts git holds LF but the checkout has CRLF (a clone made with core.autocrlf=true):
    [(path, the blob id git holds)]."""
    eol, staged = _ls_files("--eol"), _ls_files("-s")
    out = []
    for path, info in eol.items():
        words = info.split()
        if "i/lf" in words and "w/crlf" in words and path in staged and len(staged[path].split()) >= 2:
            out.append((path, staged[path].split()[1]))
    return out


def lf_again(scripts):
    """Each CRLF script written again with LF, when that gives exactly what git holds (its blob id): a script changed
    here is left alone. Returns (written, left alone)."""
    done, kept = 0, 0
    for path, blob in scripts:
        f = UPSTREAM / path
        try:
            data = f.read_bytes().replace(b"\r\n", b"\n")
        except OSError:
            kept += 1
            continue
        if hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() != blob:
            kept += 1
            continue
        f.write_bytes(data)
        done += 1
    return done, kept


def status(verbose=True):
    """0 at the pin, 1 missing, 2 elsewhere (or not a git checkout)."""
    b = bundled()
    if b is not None:
        if b == PIN and (UPSTREAM / "games" / "totk").is_dir():
            if verbose: print(f"Android port copy {UPSTREAM} ({MARKER}) at the pin {PIN[:12]}")
            return 0
        if verbose:
            print(f"软件包自带的参考仓库 {UPSTREAM} 不完整或不是固定提交 {PIN[:12]}：请重新下载完整的 NX2Harmony。\n"
                  f"The bundled Android port copy {UPSTREAM} is incomplete or not at the pin {PIN[:12]} "
                  f"({MARKER} names {b[:12] or 'no commit'}): download the full NX2Harmony package again.")
        return 2 if b != PIN else 1
    if not (UPSTREAM / "games" / "totk").is_dir():
        if verbose:
            print(f"Android port checkout missing: {UPSTREAM} (TOTK_UPSTREAM); `python {HERE.name}/upstream.py` "
                  + ("resumes the clone" if (UPSTREAM / ".git").exists() else f"clones it at {PIN[:12]}"))
        return 1
    h = head()
    if h == PIN:
        if verbose: print(f"Android port checkout {UPSTREAM} at the pin {PIN[:12]}")
        return 0
    if verbose:
        print(f"Android port checkout {UPSTREAM} is at {h[:12] if h else 'no git HEAD'}, the overlay was made for {PIN[:12]}; "
              f"`python {HERE.name}/upstream.py --update` moves it (TOTK_UPSTREAM_UNPINNED=1 builds anyway)")
    return 2


def resume():
    """Finish a clone that stopped before its checkout (the folder is there, so git clone would refuse): fetch the pin
    when the clone lacks it, then check it out over whatever the stopped checkout left."""
    print(f"resuming the clone in {UPSTREAM} ...")
    try:
        if subprocess.run(["git"] + git_config() + ["-C", str(UPSTREAM), "cat-file", "-e", PIN + "^{commit}"],
                          capture_output=True).returncode:
            if not git("config", "--get", "remote.origin.url", check=False):
                git("remote", "add", "origin", URL)
            git("fetch", "origin", PIN)
        git("checkout", "--force", "--detach", PIN)
    except FileNotFoundError as e:
        fail("没有找到 git：安装 Git，或使用自带参考仓库的完整 NX2Harmony 软件包。",
             "git was not found: install Git, or use the full NX2Harmony package, which brings this checkout.", e)
    except subprocess.CalledProcessError as e:
        fail(f"续上参考仓库 {UPSTREAM} 的克隆失败：检查网络后再运行一次；还不行就删掉这个文件夹重新克隆。",
             f"Resuming the clone in {UPSTREAM} failed: check the network and run this again; if it still fails, delete "
             "the folder and run again to clone it anew.", e)


def ensure(update=False):
    if bundled() is not None:
        if update:
            print(f"{UPSTREAM} is a bundled copy ({MARKER}), not a git checkout: not moving it")
        return status()
    if partial():
        resume()
        return status()
    if not (UPSTREAM / "games" / "totk").is_dir():
        if UPSTREAM.is_dir() and any(UPSTREAM.iterdir()):
            fail(f"参考仓库的位置 {UPSTREAM} 已有别的文件，不是 totk-android 的检出：把这个文件夹移走或删掉后再运行。",
                 f"{UPSTREAM} holds other files and is not a totk-android checkout: move or delete the folder, then run "
                 "this again.")
        UPSTREAM.parent.mkdir(parents=True, exist_ok=True)
        print(f"cloning {URL} into {UPSTREAM} ...")
        try:
            # --config: the clone keeps LF for whoever checks out in it later without these -c options
            subprocess.run(["git"] + git_config() + ["clone", "--no-checkout", "--config", "core.autocrlf=false",
                                                     "--config", "core.eol=lf", URL, str(UPSTREAM)], check=True)
            git("checkout", "--detach", PIN)
        except FileNotFoundError as e:
            fail("没有找到 git：安装 Git，或使用自带参考仓库的完整 NX2Harmony 软件包。",
                 "git was not found: install Git, or use the full NX2Harmony package, which brings this checkout.", e)
        except subprocess.CalledProcessError as e:
            fail(f"克隆参考仓库 {URL} 到 {UPSTREAM} 失败：检查网络（能否访问 GitHub）后再运行一次。",
                 f"Cloning {URL} into {UPSTREAM} failed: check the network (access to GitHub) and run this again (a "
                 "clone that stopped after its download is resumed).", e)
        return status()
    if update and head() != PIN:
        if git("status", "--porcelain"):
            print(f"{UPSTREAM} has local changes; not moving it"); return 2
        try:
            git("fetch", "origin", PIN)
            git("checkout", "--detach", PIN)
        except subprocess.CalledProcessError as e:
            fail(f"把参考仓库 {UPSTREAM} 移到固定提交 {PIN[:12]} 失败：检查网络后再运行一次。",
                 f"Moving {UPSTREAM} to the pin {PIN[:12]} failed: check the network and run this again.", e)
    if update and head() == PIN:
        crlf = crlf_scripts()
        if crlf:
            done, kept = lf_again(crlf)
            print(f"{done} shell scripts in {UPSTREAM} written again with LF line ends"
                  + (f"; {kept} left alone (changed here, or unreadable): fix them by hand" if kept else ""))
    return status()


def require():
    """For the build scripts: at the pin, or TOTK_UPSTREAM_UNPINNED=1; otherwise stop with the message above."""
    s = status()
    if s == 0 or os.environ.get("TOTK_UPSTREAM_UNPINNED") == "1":
        return
    sys.exit(f"the Android port checkout is not at the pinned commit (status {s}); see native-harmonyos/upstream.py")


if __name__ == "__main__":
    if "--check" in sys.argv: sys.exit(status())
    sys.exit(ensure(update="--update" in sys.argv))
