"""Find the player's game files and say what they are, without keys: the container's own plain headers and the file
name's [title id][vN] tags.

- .nsp / .nsz: a PFS0 whose file names are plain; a ticket's name is its rights ID, whose first 16 digits are the title
- .xci / .xcz: a card image ("HEAD" at 0x100); its titles are encrypted, so the name tags say which game and version
- .nup: a Switch 2 package ("NUP\\x01"), parts listed in its table of contents with title, version and type

The backend verifies the code modules' hashes later, so a wrong guess here stops there, before anything is built."""
from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import config

EXTS = {".nsp", ".nsz", ".xci", ".xcz", ".nup"}
TAG_TITLE = re.compile(r"\[([0-9A-Fa-f]{16})\]")
TAG_VERSION = re.compile(r"\[v(\d+)\]")


@dataclass
class Item:
    path: Path
    kind: str                 # nsp, nsz, xci, xcz, nup
    titles: list = field(default_factory=list)   # [(title id, version or None, role)], role base / update / dlc
    note: str = ""

    @property
    def base_title(self):
        return base_of(self.titles[0][0]) if self.titles else None


def base_of(title: str) -> str:
    """The base game's title id: an update is base + 0x800, add-on content base + 0x1000 + n."""
    t = int(title, 16)
    return f"{(t - 0x1000 if t & 0xFFF not in (0, 0x800) else t) & ~0xFFF:016X}"


def role(title: str) -> str:
    low = int(title, 16) & 0xFFF
    return "base" if low == 0 else "update" if low == 0x800 else "dlc"


def pfs0_names(f) -> list:
    head = f.read(16)
    if head[:4] != b"PFS0":
        return []
    n, strsize = struct.unpack_from("<II", head, 4)
    if n > 4096 or strsize > 1 << 20:
        return []
    entries = f.read(0x18 * n)
    strings = f.read(strsize)
    names = []
    for i in range(n):
        off = struct.unpack_from("<I", entries, 0x18 * i + 0x10)[0]
        end = strings.find(b"\0", off)
        names.append(strings[off:end if end >= 0 else None].decode("utf-8", "replace"))
    return names


def nup_parts(f) -> list:
    head = f.read(0x80 + 0x30)
    if head[:4] != b"NUP\x01" or head[0x80:0x84] != b"TOC\x01":
        return []
    parts = struct.unpack_from("<I", head, 0x84)[0]
    out = []
    for i in range(min(parts, 64)):
        rec = f.read(0x40)
        # u64 title, u32 root / ExeFS / RomFS dirs, u32 version, u8 kind, u8 flags (bit 0: a standalone update), ...
        title, _root, _exefs, _romfs, version, kind, _flags = struct.unpack_from("<QIIIIBB", rec, 0)
        if title == 0 or kind == 4:   # the empty base record of an update-only container; a system package
            continue
        out.append((f"{title:016X}", f"v{version}", {0: "base", 1: "update"}.get(kind, "dlc")))
    return out


def identify(path: Path) -> Item:
    kind = path.suffix.lower()[1:]
    item = Item(path, kind)
    tag_t = TAG_TITLE.findall(path.name)
    tag_v = TAG_VERSION.findall(path.name)
    try:
        with open(path, "rb") as f:
            if kind in ("nsp", "nsz"):
                names = pfs0_names(f)
                if not names:
                    item.note = "不是有效的 PFS0（NSP/NSZ）文件"
                    return item
                tiks = sorted({n[:16].upper() for n in names if n.endswith(".tik") and len(n) >= 36})
                for t in tiks or [x.upper() for x in tag_t[:1]]:
                    item.titles.append((t, f"v{tag_v[0]}" if tag_v else None, role(t)))
            elif kind in ("xci", "xcz"):
                f.seek(0x100)
                if f.read(4) != b"HEAD":
                    item.note = "不是有效的卡带镜像（XCI/XCZ）"
                    return item
                if tag_t:
                    t = tag_t[0].upper()
                    v = f"v{tag_v[0]}" if tag_v else None
                    item.titles.append((t, "v0", "base"))
                    if v and v != "v0":   # a card that also carries an update
                        item.titles.append((t[:13] + "800", v, "update"))
                else:
                    item.note = "文件名里没有 [title id] 标签，卡带内容需要密钥才能识别"
            elif kind == "nup":
                item.titles = nup_parts(f)
                if not item.titles:
                    item.note = "不是有效的 NUP 文件"
    except OSError as e:
        item.note = f"读不了：{e}"
    return item


# Folders the walk does not enter: hidden and system ones, version control, and the hard-linked copies the backends
# stage (prepared/nx2h-stage, .nx2h-stage), so a game is not listed twice. This app's own folders: _excluded().
SKIP = {"$recycle.bin", "system volume information", "windows", "program files", "program files (x86)", "programdata",
        "appdata", "node_modules", ".git", ".svn", "__pycache__", "nx2h-stage", ".nx2h-stage", "_internal", "site-packages"}
MIN_SIZE = 1 << 20   # smaller files with these extensions are not games (tickets, test stubs)


def _norm(p) -> str:
    return os.path.normcase(os.path.abspath(str(p)))


def _excluded() -> tuple:
    """The folders the walk never enters, wherever they are found below a chosen folder: this app's own folder (and its
    data folder when elsewhere), backends/ beside the app, and every backend's prepared/ and artifacts/, where the
    backends unpack and build from the game files (an update unpacked there is not a second update). As (normalized
    paths, (device, file id) pairs, lowercase names): a folder is looked at only when its name is one of them."""
    dirs = [config.APP_DIR, config.APP_DIR / "backends", config.DATA, config.WORK]
    for root in config.backend_dirs():
        dirs += [root / "prepared", root / "artifacts"]
    paths, ids, names = set(), set(), set()
    for d in dirs:
        names.add(d.name.lower())
        paths.add(_norm(d))
        try:
            paths.add(_norm(d.resolve()))
            st = os.stat(d)
        except (OSError, RuntimeError):
            continue
        if st.st_ino:
            ids.add((st.st_dev, st.st_ino))
    return paths, ids, names


def find(folders) -> list:
    """The game files in the chosen folders and all their subfolders, in path order. A file reached by two paths (a
    hard link) counts once. A chosen folder that holds this app is walked, but not the app's own folders in it
    (_excluded)."""
    found, seen = [], set()
    ex_paths, ex_ids, ex_names = _excluded()

    def excluded(path: str, name: str) -> bool:
        if name not in ex_names:
            return False
        if _norm(path) in ex_paths:
            return True
        try:
            st = os.stat(path)
        except OSError:
            return False
        return bool(st.st_ino) and (st.st_dev, st.st_ino) in ex_ids

    def add(path: Path, st) -> None:
        key = (st.st_dev, st.st_ino) if st.st_ino else str(path).lower()
        if key not in seen:
            seen.add(key)
            found.append(path)

    def walk(d: Path) -> None:
        try:
            with os.scandir(d) as it:
                entries = sorted(it, key=lambda e: e.name.lower())
        except OSError:
            return
        for e in entries:
            try:
                if e.is_file(follow_symlinks=False):
                    if os.path.splitext(e.name)[1].lower() in EXTS:
                        st = e.stat(follow_symlinks=False)
                        if st.st_size >= MIN_SIZE:
                            add(Path(e.path), st)
                elif e.is_dir(follow_symlinks=False):
                    n = e.name.lower()
                    if n not in SKIP and not n.startswith(("$", ".")) and not excluded(e.path, n):
                        walk(Path(e.path))
            except OSError:
                continue

    for d in folders:
        d = Path(d)
        if d.is_file() and d.suffix.lower() in EXTS:
            add(d, d.stat())
        elif d.is_dir():
            walk(d)
    return [identify(p) for p in found]


def games_found(items: list) -> dict:
    """base title id -> [items]."""
    by = {}
    for it in items:
        if it.base_title:
            by.setdefault(it.base_title, []).append(it)
    return by
