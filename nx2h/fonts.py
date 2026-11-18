"""The console's shared fonts (BFTTF), which every game asks the system for. A game card carries the system update it
needs, and with it the fonts; a NUP or an NSP does not. So they are read once from one of the player's own card images in the library (any game) (or
an unpacked firmware folder, nx2h.local.json "firmware") with the player's prod.keys, verified (NCA FS header, the six
IVFC levels, each BFTTF header: vendor/switchfs/extract_shared_fonts.py, in this process), and kept in the app's data
folder for every game: work/shared-fonts/{manifest.json, <n>-<name>.bfttf}. Nothing of them is in the project."""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

from . import config

FONTS = config.WORK / "shared-fonts"


def ready():
    return FONTS if (FONTS / "manifest.json").is_file() else None


def _run(args: list) -> None:
    import contextlib
    import io
    from .vendor.switchfs import extract_shared_fonts as esf
    saved = sys.argv
    sys.argv = ["extract_shared_fonts"] + args
    try:
        with contextlib.redirect_stdout(io.StringIO()):   # its JSON summary would break the --progress json stream
            esf.main()
    finally:
        sys.argv = saved


def firmware_folders(near: Path) -> list:
    """Unpacked firmware folders (many .nca files) near the keys: the folder above the keys' and two levels below it
    (a firmware the player dumped from their own console, e.g. "firmware/")."""
    out = []
    for top in [near] + list(near.parents)[:2]:
        for d in [top] + [c for c in _subdirs(top)] + [g for c in _subdirs(top) for g in _subdirs(c)]:
            try:
                if d not in out and sum(1 for _ in d.glob("*.nca")) >= 50:
                    out.append(d)
            except OSError:
                continue
    return out


def _subdirs(d: Path) -> list:
    try:
        return [c for c in d.iterdir() if c.is_dir() and not c.name.startswith((".", "$"))]
    except OSError:
        return []


def extract(items: list, keys_dir) -> tuple:
    """(folder, message): the fonts, read now when they are not there yet."""
    if ready():
        return FONTS, "已有"
    keys = Path(keys_dir) / "prod.keys" if keys_dir else None
    if not keys or not keys.is_file():
        return None, "取系统字体需要 prod.keys"
    fw = config.local().get("firmware")
    sources = [("--firmware", Path(fw))] if fw and Path(fw).is_dir() else []
    sources += [("--firmware", d) for d in firmware_folders(keys.parent) if not fw or d != Path(fw)]
    sources += [("--xci", it.path) for it in items if it.kind == "xci"]   # a trimmed card image may have dropped them
    if not sources:
        return None, "系统字体要从你自己的一张游戏卡带镜像（.xci）或你主机导出的固件里取：游戏文件夹里放一张卡带镜像即可"
    tmp = config.WORK / "shared-fonts.tmp"
    for flag, src in sources:
        if tmp.exists():
            shutil.rmtree(tmp)
        try:
            _run(["--keys", str(keys), flag, str(src), "--output", str(tmp)])
        except BaseException as e:   # the readers stop with SystemExit/ValueError on a source without them
            if isinstance(e, KeyboardInterrupt):
                raise
            continue
        if (tmp / "manifest.json").is_file():
            if FONTS.exists():
                shutil.rmtree(FONTS)
            tmp.rename(FONTS)
            return FONTS, f"从 {src.name} 取得"
    if tmp.exists():
        shutil.rmtree(tmp)
    return None, "这些卡带 / 固件里没取到系统字体（密钥太旧，或文件损坏）"
