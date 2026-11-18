#!/usr/bin/env python3
"""A self-contained NX2Harmony folder for a clean PC (or a VM): the packed app with an empty data folder and the game
backends' sources beside it in backends/ (config.py looks there first), so nothing but the player's game files, keys,
DevEco Studio and a phone is needed.

    python packaging/full_bundle.py --out <empty folder> [--zip]

What goes in: dist/NX2Harmony (the app; run packaging/build.py first) without its data/; the backends as this
repository's commit holds them (git archive HEAD of backends/: each game backend's folder, prebuilt-ohos, SOURCES.tsv,
README.md; the package must match a commit, so uncommitted changes under backends/ are refused) with
NX2HARMONY-COMMIT.txt naming that commit; the Android port every backend builds on, totk-android (MIT; its
vendor/ryujinx MIT too, each with its licence), at the backends' native-harmonyos/UPSTREAM_PIN, as committed (git
archive without line-ending conversion: LF) in backends/totk-android with NX2HARMONY-UPSTREAM.txt "<url> <commit>",
which upstream.py takes for the checkout at the pin (no git, no clone on the new PC); the part of nxnative (MIT) that
the Ocarina of Time backend runs, at the commit backends/SOURCES.tsv pins, in backends/nxnative; the prebuilt
third-party libraries (libopus, libvpx, with their licences) in backends/prebuilt-ohos, so no PC builds them. Every
*.sh in backends/ goes in with LF line ends (they run in Linux containers). What never goes in (_allowed, checked on
the committed files too): anything under prepared/ or artifacts/ (game material, built packages), build outputs and
caches, the signing configuration (build-profile.json5 at the project root), machine settings (local.ohos.env / .ps1
and their .usb / .wifi / backup copies, not the tracked local.ohos.example.ps1; local.properties), keys, certificates
and profiles, pictures outside an app's resources (screenshots of a game), backup copies (name.c.a121pre,
name.py.a108, *.bak).
Changed on the way: a bundle name made on some PC (nx2h.u<6>.<slug>, nx2h/bundle.py) that a backend keeps as a default
becomes nx2h.u000000.<slug>; every build gets the player's own name anyway (signing.py, TOTK_OHOS_BUNDLE).
Refused before anything is copied: uncommitted changes under backends/, a backend folder without its marker file
(nx2h/backends MARKERS) or holding a file the package never carries, backends that pin different totk-android
commits, no totk-android git checkout holding the pin (backends/totk-android, the one a backend's own lookup finds,
else ../totk-android), backends/prebuilt-ohos without libopus.a or libvpx.a; a backend file that would tell who packed
it or from where (PRIVATE: the packing user's folder and git e-mail, a LAN address with a port as hdc over Wi-Fi uses,
a cloud-drive download folder); a picture in a backend's app that is not in packaging/bundle_pictures.tsv. Paths of
the packing PC's own folders in a backend file are only listed (packaging/backend_import.py refuses them before they
reach the repository). Refused while packing (the output folder is then incomplete: delete it): a backend without
build-profile.template.json5, no nxnative checkout holding the commit backends/SOURCES.tsv pins.
_files / SOURCE_DIRS / SOURCE_FILES (which files of a backend project count as its sources) are also what
packaging/backend_import.py takes from a project's working folder."""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

NEVER_DIRS = {"prepared", "artifacts", "build", ".hvigor", "oh_modules", ".idea", "__pycache__", ".git", "libs", ".preview",
              ".cxx", "signing", "node_modules", "logs", "runs"}
# a port's machine settings and their copies (local.ohos.ps1.usb / .wifi, local.ohos.env.bak-wifi-...), not the
# template the install guides tell a player to copy (native-harmonyos/local.ohos.example.ps1, tracked)
NEVER_FILES = ["local.ohos.env", "local.ohos.env.*", "local.ohos.ps1", "local.ohos.ps1.*", "local.properties", "*.p12", "*.cer", "*.csr", "*.p7b", "*.keys", "*.hap",
               "*.log", "*.pyc", ".clangd", "*.nsp", "*.nsz", "*.xci", "*.xcz", "*.nup", "*.bak", "*.bak?", "*.orig",
               "*.rej", "*.pre", "*.post"]
# a port's working copies: vk_draw.c.a121pre, gmemfast.py.a108, nvn_vk.c.a121b.post
BACKUP = re.compile(r"\.a\d{2,3}[a-z]*(pre|post)?$")
PICTURES = ("*.jpg", "*.jpeg", "*.png", "*.webp", "*.bmp", "*.gif", "*.svg", "*.ico")
# a backend that is not a git checkout: these folders and root files are the project; anything else is the porter's
# (docs/ too: a working folder's notes are the porter's log; a git checkout's docs/ is part of what git lists)
SOURCE_DIRS = {"AppScope", "entry", "hvigor", "native-harmonyos", "assets", "tools", "tests"}
# a bundle name nx2h/bundle.py made on some PC, left in a backend's sources as a default; NEUTRAL_BUNDLE in the package
MACHINE_BUNDLE = re.compile(rb"\bnx2h\.u[0-9a-z]{6}\.")
NEUTRAL_BUNDLE = b"nx2h.u000000."
PICTURES_OK = REPO / "packaging" / "bundle_pictures.tsv"
SOURCE_FILES = {"LICENSE", "THIRD_PARTY_NOTICES.md", ".gitignore", "hvigorfile.ts", "build-profile.template.json5",
                "code-linter.json5", "oh-package.json5", "oh-package-lock.json5"}
# nxnative (seedds/nxnative, MIT): what the Ocarina of Time backend runs (shader_split.py and what it imports, the mesh
# stage, the Ampere Ryujinx the translator is built from, the game's profile)
NXNATIVE_PARTS = ["LICENSE", "README.md", "dm/gfx/tools", "dm/tools", "dm/games/game.py", "dm/games/profile_guess.py",
                  "dm/games/settings.py", "dm/games/0400486024DEC000", "dm/third_party/ryujinx",
                  "dm/gfx/nvn/dmsh_abi.h"]   # dmsh_abi.py (imported by dmsh_meshstage.py) reads it
# nxnative's prebuilt translator and SPIRV-Cross (.NET and native binaries without licence texts for what they bundle):
# the Ocarina of Time backend builds its own (xlate/prepare_xlate_ns2.py, SPIRV-Cross in a container) and never runs them
NXNATIVE_LEFT_OUT = ["dm/gfx/tools/dmsh_xlate_bin", "dm/gfx/tools/spirv_cross_bin"]


def _allowed(rel: Path, root_profile: bool = True) -> bool:
    if any(part in NEVER_DIRS for part in rel.parts[:-1]):
        return False
    if root_profile and rel.as_posix() == "build-profile.json5":   # the signing configuration (the template stays)
        return False
    if BACKUP.search(rel.name):
        return False
    if any(fnmatch.fnmatch(rel.name.lower(), pat) for pat in PICTURES) and "resources" not in rel.parts:
        return False
    return not any(fnmatch.fnmatch(rel.name, pat) for pat in NEVER_FILES)


def _git(root: Path, cmd: str, *args) -> list:
    r = subprocess.run(["git", "-C", str(root), cmd, "-z", *args], capture_output=True, check=True)   # -z before any "--"
    return [Path(p) for p in r.stdout.decode("utf-8").split("\0") if p]


def _files(root: Path) -> list:
    if (root / ".git").exists():
        rels = _git(root, "ls-files", "--cached", "--others", "--exclude-standard")
    else:
        rels = [p.relative_to(root) for p in root.rglob("*") if p.is_file()]
        rels = [r for r in rels if (r.parts[0] in SOURCE_DIRS if len(r.parts) > 1 else r.name in SOURCE_FILES)]
    return [rel for rel in rels if (root / rel).is_file() and _allowed(rel)]


def _text(data: bytes) -> bool:
    return b"\0" not in data[:8192]


def _private() -> dict:
    """What in a backend file would tell who packed the package, or from where: what it is -> pattern."""
    marks = {
        "the packing user's folder": re.compile(rb"(?i)users[\\/]+" + re.escape(Path.home().name.encode("utf-8"))
                                                + rb"(?![\w.-])"),
        "a LAN address with a port (hdc over Wi-Fi)": re.compile(
            rb"(?<![\d.])(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}:\d{2,5}(?!\d)"),
        "a cloud-drive download folder": re.compile(rb"(?i)" + b"baidu" + b"net" + b"disk"),
    }
    email = subprocess.run(["git", "config", "--global", "user.email"], capture_output=True, text=True).stdout.strip()
    if "@" in email and not email.lower().endswith("users.noreply.github.com"):
        marks["the packing user's git e-mail"] = re.compile(re.escape(email.encode("utf-8")), re.I)
    # this PC's name and the packing user's git name, as whole words of 4 or more characters (a short one would match
    # ordinary words in sources)
    import socket
    name = subprocess.run(["git", "config", "--global", "user.name"], capture_output=True, text=True).stdout.strip()
    for what, word in (("this PC's name", socket.gethostname()), ("the packing user's git name", name)):
        if len(word) >= 4 and "@" not in word:
            marks[what] = re.compile(rb"(?i)(?<![\w.-])" + re.escape(word.encode("utf-8")) + rb"(?![\w-])")
    return marks


def _local_folders(roots: list):
    """The packing PC's own top folders (for example X:\\Projects) of this repository and the backends: listed when a
    backend file names them, not refused (a porter's comment says where something came from)."""
    tops = {(r.resolve().drive, r.resolve().parts[1]) for r in roots if len(r.resolve().parts) > 1}
    alts = [re.escape(d.encode("utf-8")) + rb"[\\/]+" + re.escape(top.encode("utf-8")) + rb"(?![\w-])" for d, top in tops]
    return re.compile(rb"(?i)" + b"|".join(alts)) if alts else None


def _pictures_ok() -> set:
    try:
        lines = PICTURES_OK.read_text(encoding="utf-8").splitlines()
    except OSError:
        sys.exit(f"refusing: {PICTURES_OK} is missing (the pictures a backend's app may carry)")
    return {ln.split("\t", 1)[0].strip().lower() for ln in lines if ln.strip() and not ln.startswith("#")}


def _vet(packed: list, local_roots: list = None) -> list:
    """Every backend file, before anything is copied: refused when one would tell who packed the package or from
    where (_private, after the bundle names are made neutral), or is a picture nobody has looked at (bundle_pictures.tsv).
    packed: (name, folder, src) with the files _files lists, or (name, folder, src, files). local_roots: the folders
    whose top folders are this PC's own (default: this repository and each src). Returns [(name, folder, src, files)]."""
    private, ok = _private(), _pictures_ok()
    local = _local_folders(local_roots if local_roots is not None else [REPO] + [p[2] for p in packed])
    vetted, refused, unseen, listed = [], [], [], []
    for entry in packed:
        name, folder, src = entry[:3]
        files = [f for f in entry[3] if _allowed(f)] if len(entry) > 3 else _files(src)
        for rel in files:
            where = f"{folder}/{rel.as_posix()}"
            data = (src / rel).read_bytes()
            if any(fnmatch.fnmatch(rel.name.lower(), pat) for pat in PICTURES):
                if hashlib.sha256(data).hexdigest() not in ok:
                    unseen.append(where)
                continue
            if not _text(data):
                continue
            data = MACHINE_BUNDLE.sub(NEUTRAL_BUNDLE, data)
            for what, rx in private.items():
                for m in rx.finditer(data):
                    line = data.count(b"\n", 0, m.start()) + 1
                    refused.append(f"  {where}:{line}: {what}")
            if local:
                for m in local.finditer(data):
                    line = data.count(b"\n", 0, m.start()) + 1
                    listed.append(f"  {where}:{line}: {m.group(0).decode('utf-8', 'replace')}")
        vetted.append((name, folder, src, files))
    if listed:
        print(f"note: {len(listed)} places name this PC's own folders (packed as they are):")
        print("\n".join(listed[:20] + ([f"  ... {len(listed) - 20} more"] if len(listed) > 20 else [])))
    problems = []
    if refused:
        problems.append("these backend files would tell who packed the package or from where (clean them in the "
                        "backend first):\n" + "\n".join(refused))
    if unseen:
        problems.append(f"these pictures are not in {PICTURES_OK.name} (look at each: no game's or company's art; then "
                        "add its sha256 there):\n" + "\n".join(f"  {u}" for u in unseen))
    if problems:
        sys.exit("refusing: " + "\n".join(problems))
    return vetted


def _copy(src: Path, dst: Path) -> bool:
    """One backend file; in a text file a PC's bundle name becomes NEUTRAL_BUNDLE (True: it had one)."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    data = src.read_bytes()
    if _text(data) and MACHINE_BUNDLE.search(data):
        dst.write_bytes(MACHINE_BUNDLE.sub(NEUTRAL_BUNDLE, data))
        shutil.copystat(src, dst)
        return True
    shutil.copy2(src, dst)
    return False


# the third-party libraries every backend links (common.BaseBackend.PREBUILT), committed in backends/prebuilt-ohos
PREBUILT = "prebuilt-ohos"
SOURCES_TSV = REPO / "backends" / "SOURCES.tsv"


def _repo_backends(dst: Path) -> str:
    """backends/ as HEAD holds it (git archive: the committed bytes, LF but where .gitattributes says otherwise) into
    dst/backends; returns HEAD's commit. Refused when backends/ has changes that are not committed."""
    dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "--untracked-files=all", "--", "backends"],
                           capture_output=True, text=True, check=True).stdout.strip()
    if dirty:
        sys.exit("refusing: backends/ has changes that are not committed (the package must match a commit):\n"
                 + "\n".join("  " + line for line in dirty.splitlines()[:20]))
    head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()
    tar = subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", str(REPO), "archive",
                          "--format=tar", head, "--", "backends"], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        for m in tf:
            parts = _safe_parts(m.name)
            to = dst.joinpath(*parts)
            if m.isdir():
                to.mkdir(parents=True, exist_ok=True)
                continue
            if not m.isfile():
                sys.exit(f"refusing: {m.name} in backends/ at {head[:12]} is not a plain file")
            to.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(m) as f, open(to, "wb") as g:
                shutil.copyfileobj(f, g)
    return head


def _sources_pin(folder: str) -> tuple:
    """(origin, revision) of a row of backends/SOURCES.tsv."""
    try:
        lines = SOURCES_TSV.read_text(encoding="utf-8").splitlines()
    except OSError:
        sys.exit(f"refusing: {SOURCES_TSV} is missing")
    for line in lines:
        cols = line.split("\t")
        if len(cols) >= 4 and cols[0] == folder and not line.startswith("#"):
            return cols[2], cols[3]
    sys.exit(f"refusing: backends/SOURCES.tsv has no row for {folder}")


def _prebuilt_ok(root: Path) -> int:
    """The committed prebuilt libraries are there (every new PC would need PowerShell 7, a container and the network
    to build libopus / libvpx otherwise); returns their bytes."""
    from nx2h.backends.common import BaseBackend
    missing = [lib for lib in BaseBackend.PREBUILT.values() if not (root / lib).is_file()]
    if missing:
        sys.exit(f"refusing: backends/{PREBUILT} lacks {', '.join(missing)}")
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file())


def _nxnative(dst: Path) -> int:
    """Copy NXNATIVE_PARTS of nxnative at the commit backends/SOURCES.tsv pins, from a checkout that holds it
    (backends/nxnative, the one beside this repository, or the OoT backend's own lookup); returns the bytes copied."""
    from nx2h.backends import load
    url, pin = _sources_pin("nxnative")
    if not re.fullmatch(r"[0-9a-f]{40}", pin):
        sys.exit(f"refusing: backends/SOURCES.tsv pins nxnative to {pin!r}, not a full commit")
    cls = load("oot_v131072")
    probe = cls.__new__(cls)   # the backend's own lookup, without a phone or a bundle name
    probe.root, probe._nxnative = REPO / "backends" / "OOT", None
    places = [REPO / "backends" / "nxnative", REPO.parent / "nxnative", probe.nxnative()]
    src = next((p for p in places if p and (p / ".git").exists() and subprocess.run(
        ["git", "-C", str(p), "cat-file", "-e", pin + "^{commit}"], capture_output=True).returncode == 0), None)
    if not src:
        sys.exit(f"refusing: no nxnative git checkout holds the pinned commit {pin[:12]} (looked in "
                 + ", ".join(str(p) for p in places if p) + f"); clone {url} beside this repository")
    head = pin
    # the committed bytes (git archive), not the working tree: a checkout made with core.autocrlf=true has CRLF files
    data = subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", str(src), "archive", "--format=tar",
                           head, "--", *NXNATIVE_PARTS, *(":(exclude)" + x for x in NXNATIVE_LEFT_OUT)],
                          capture_output=True, check=True).stdout
    dst.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        members = [m for m in tf.getmembers() if m.isfile() and not m.name.startswith("/") and ".." not in Path(m.name).parts]
        tf.extractall(dst, members=members)
    files = [Path(m.name) for m in members]
    (dst / "NX2HARMONY-COMMIT.txt").write_bytes(f"{url} {head}\n".encode("utf-8"))   # LF
    size = sum((dst / rel).stat().st_size for rel in files)
    print(f"nxnative: {len(files)} files, {size / 2**20:.1f} MB at {head[:12]} from {src}")
    return size


# totk-android, as upstream.py knows a bundled copy: its MARKER file and URL
UPSTREAM_MARKER = "NX2HARMONY-UPSTREAM.txt"
UPSTREAM_URL = re.compile(r'^URL\s*=\s*"([^"]+)"', re.M)


def _upstream_pin(roots: list) -> tuple:
    """(pin, url): the commit every backend's native-harmonyos/UPSTREAM_PIN names (they must agree) and upstream.py's
    URL."""
    pins, url = {}, ""
    for root in roots:
        na = root / "native-harmonyos"
        try:
            words = (na / "UPSTREAM_PIN").read_text(encoding="utf-8").split()
        except OSError:
            continue
        pins.setdefault(words[0].lower() if words else "", []).append(str(root))
        if not url:
            try:
                m = UPSTREAM_URL.search((na / "upstream.py").read_text(encoding="utf-8"))
            except OSError:
                m = None
            url = m.group(1) if m else ""
    if len(pins) != 1 or not re.fullmatch(r"[0-9a-f]{40}", next(iter(pins))):
        sys.exit(f"refusing: the backends' native-harmonyos/UPSTREAM_PIN do not name one commit: {pins or 'none found'}")
    if not url:
        sys.exit("refusing: no URL = \"...\" in the backends' native-harmonyos/upstream.py")
    return next(iter(pins)), url


def _upstream_source(names: list, pin: str) -> Path:
    """The totk-android git checkout that holds the pin: backends/totk-android (where the app's environment page clones
    it), the one a backend uses (its ohos_env.UPSTREAM, through the backend's own upstream()), else the one beside this
    repository."""
    from nx2h.backends import MARKERS, load
    places = [REPO / "backends" / "totk-android"]
    for name in names:
        cls = load(name)
        probe = cls.__new__(cls)   # the backend's own lookup, without a phone or a bundle name
        probe.root, probe._upstream = REPO / "backends" / MARKERS[name][1], None
        if hasattr(probe, "upstream"):
            places.append(probe.upstream())
    places.append(REPO.parent / "totk-android")
    seen = []
    for p in places:
        if p in seen:
            continue
        seen.append(p)
        if (p / ".git").exists() and subprocess.run(["git", "-C", str(p), "cat-file", "-e", pin + "^{commit}"],
                                                    capture_output=True).returncode == 0:
            return p
    sys.exit(f"refusing: no totk-android git checkout holds the pinned commit {pin[:12]} (looked in "
             + ", ".join(str(p) for p in seen) + f"); fetch it there: git fetch origin {pin}")


def _safe_parts(name: str) -> tuple:
    """A tar member's path as parts below the destination; refused when it would leave it."""
    p = PurePosixPath(name)
    parts = tuple(x for x in p.parts if x not in ("", "."))
    if p.is_absolute() or not parts or ".." in parts or any(":" in x or "\\" in x for x in parts):
        sys.exit(f"refusing: archive member {name!r} would land outside the folder")
    return parts


def _upstream(dst: Path, src: Path, pin: str, url: str) -> int:
    """src's tree at pin as committed into dst (git archive with no line-ending conversion: totk-android has no
    .gitattributes, so its files come out byte for byte, LF), then NX2HARMONY-UPSTREAM.txt "<url> <pin>"; returns the
    bytes written."""
    with tempfile.TemporaryDirectory() as t:
        tar = Path(t) / "totk-android.tar"
        r = subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", str(src), "archive",
                            "--format=tar", "-o", str(tar), pin], capture_output=True)
        if r.returncode:
            sys.exit(f"refusing: git archive {pin[:12]} in {src} failed: {r.stderr.decode('utf-8', 'replace').strip()}")
        files, size = 0, 0
        with tarfile.open(str(tar), "r") as tf:
            for m in tf:
                parts = _safe_parts(m.name)
                to = dst.joinpath(*parts)
                if m.isdir():
                    to.mkdir(parents=True, exist_ok=True)
                    continue
                if not m.isfile():   # a symlink or the like: none at the pin; a new one needs a decision here
                    sys.exit(f"refusing: {m.name} in totk-android at {pin[:12]} is not a plain file")
                to.parent.mkdir(parents=True, exist_ok=True)
                with tf.extractfile(m) as f, open(to, "wb") as g:
                    shutil.copyfileobj(f, g)
                os.utime(to, (m.mtime, m.mtime))
                files += 1
                size += m.size
    if not (dst / "LICENSE").is_file():
        sys.exit(f"refusing: totk-android at {pin[:12]} has no LICENSE")
    (dst / UPSTREAM_MARKER).write_bytes(f"{url} {pin}\n".encode("utf-8"))
    print(f"totk-android: {files} files, {size / 2**20:.1f} MB at {pin[:12]} from {src}")
    return size


def _lf_sh(root: Path) -> int:
    """Every *.sh under root with LF line ends (dash in the Linux containers reads a CR as part of each command);
    returns how many were changed."""
    n = 0
    for p in root.rglob("*.sh"):
        if p.is_file():
            b = p.read_bytes()
            if b"\r\n" in b:
                p.write_bytes(b.replace(b"\r\n", b"\n"))
                n += 1
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--zip", action="store_true", help="also write <out>/NX2Harmony-<version>-windows-x64-full.zip")
    a = ap.parse_args()
    from nx2h import __version__
    from nx2h.backends import MARKERS
    app = REPO / "dist" / "NX2Harmony"
    if not (app / "NX2Harmony.exe").is_file():
        sys.exit("dist/NX2Harmony is missing: python packaging/build.py first")
    out = Path(a.out) / "NX2Harmony"
    if out.exists():
        sys.exit(f"{out} exists: choose another --out (nothing is overwritten)")
    with tempfile.TemporaryDirectory() as t:
        head = _repo_backends(Path(t))
        staged = Path(t) / "backends"
        packed = []
        for name, (marker, folder) in MARKERS.items():
            src = staged / folder
            if not (src / marker).is_file():
                sys.exit(f"refusing: backends/{folder} at {head[:12]} has no {marker}")
            files = [p.relative_to(src) for p in sorted(src.rglob("*")) if p.is_file()]
            dropped = [f.as_posix() for f in files if not _allowed(f)]
            if dropped:   # backend_import.py never takes these in: the commit holds what the package must not
                sys.exit(f"refusing: backends/{folder} holds files the package never carries: " + ", ".join(dropped[:10]))
            packed.append((name, folder, src, files))
        # refused here, before anything is copied
        pin, url = _upstream_pin([src for _, _, src, _ in packed])
        upstream = (_upstream_source([name for name, _, _, _ in packed], pin), pin, url)
        vetted = _vet(packed, local_roots=[REPO])
        prebuilt = _prebuilt_ok(staged / PREBUILT)
        shutil.copytree(app, out, ignore=lambda d, names: ["data"] if Path(d) == app else [])
        (out / "data").mkdir()
        # the notices beside the exe too (the packed app keeps a copy in _internal): what backends/ carries, and its licences
        shutil.copy2(REPO / "THIRD_PARTY_NOTICES.md", out / "THIRD_PARTY_NOTICES.md")
        total = prebuilt
        for name, folder, src, files in vetted:
            dst = out / "backends" / folder
            neutral = sum(_copy(src / rel, dst / rel) for rel in files)
            size = sum((dst / rel).stat().st_size for rel in files)
            total += size
            if not (dst / "build-profile.template.json5").is_file():
                # build-profile.json5 stays behind (it holds a PC's signing); without the template a new PC cannot sign
                sys.exit(f"refusing: backends/{folder} has no build-profile.template.json5 (build-profile.json5 without "
                         "signingConfigs)")
            print(f"{name}: {len(files)} files, {size / 2**20:.1f} MB"
                  + (f"; neutral bundle name in {neutral}" if neutral else ""))
        shutil.copytree(staged / PREBUILT, out / "backends" / PREBUILT)
        for extra in ("README.md", "SOURCES.tsv"):   # what backends/ is and where each folder comes from
            if (staged / extra).is_dir():
                shutil.copytree(staged / extra, out / "backends" / extra)
            elif (staged / extra).is_file():
                shutil.copy2(staged / extra, out / "backends" / extra)
    (out / "backends" / "NX2HARMONY-COMMIT.txt").write_bytes(f"NX2Harmony {head}\n".encode("utf-8"))
    print(f"backends: from commit {head[:12]}; prebuilt libraries {prebuilt / 2**20:.1f} MB")
    # beside the backends: ohos_env.UPSTREAM's default (../totk-android)
    total += _upstream(out / "backends" / "totk-android", *upstream)
    total += _nxnative(out / "backends" / "nxnative")
    print(f"line ends: {_lf_sh(out / 'backends')} .sh files made LF")
    bad = [p for p in out.rglob("*") if p.is_file() and any(d in ("prepared", "artifacts") for d in p.relative_to(out).parts)]
    if bad:
        sys.exit(f"refusing: game material would be packed ({bad[0]})")
    print(f"bundle: {out} (backends {total / 2**20:.1f} MB)")
    if a.zip:
        z = Path(a.out) / f"NX2Harmony-{__version__}-windows-x64-full.zip"
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            for p in sorted(out.rglob("*")):
                if p.is_file():
                    zf.write(p, Path("NX2Harmony") / p.relative_to(out))
        print(f"zip: {z} ({z.stat().st_size / 2**20:.0f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
