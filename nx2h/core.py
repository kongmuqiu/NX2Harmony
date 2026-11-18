"""What the desktop front end needs, without any drawing: the setup checks (doctor.py), the games in the library folder
(scan.py, games.tsv, their icons) and the folder choices. A build runs `nx2h.py build --progress json` as a child (the
schema nxnative's front ends read). Nothing here reads keys or signing files."""
from __future__ import annotations

from pathlib import Path

from . import config, doctor, scan
from .device import Hdc
from .events import Events


def library_games(folder: str) -> list:
    """The games in the library folder with their state, buildable ones first."""
    if not folder or not Path(folder).is_dir():
        return []
    from .backends import BACKENDS, load, missing
    from . import icon
    rows = {r["title_id"]: r for r in config.games()}
    keys = keys_folder()
    out = []
    for title, its in scan.games_found(scan.find([folder])).items():
        row = rows.get(title)
        ic = icon.cached(title)
        pending = False if ic else icon.extract_later(title, its, keys)
        g = {"title_id": title, "icon": str(ic) if ic else "", "icon_pending": pending,
             "name": row["name"] if row else file_title(its), "version": row["version"] if row else "",
             "status": row["status"] if row else "未适配", "needs": row["needs"] if row else "",
             "files": [{"kind": it.kind, "name": it.path.name, "titles": [list(t) for t in it.titles]} for it in its],
             "buildable": False, "prepared": False, "problems": []}
        gone = missing(row["backend"]) if row and row["status"] in config.BUILDABLE and row["backend"] in BACKENDS else ""
        if gone:   # no backend project: nothing it has made can be seen, so the game is not buildable yet
            g["problems"] = [gone]
        elif row and row["status"] in config.BUILDABLE and row["backend"] in BACKENDS:
            be = load(row["backend"])(Events(), Hdc())
            _, problems = be.pick_inputs(its)
            prepared = not be.needs_inputs()
            g["problems"] = [] if prepared else problems
            g["bundle"] = be.bundle
            g["prepared"] = prepared
            g["buildable"] = not g["problems"]
        out.append(g)
    return sorted(out, key=lambda g: (not g["buildable"], g["status"] not in config.BUILDABLE, g["status"] != "已验证", g["name"]))


def file_title(items: list) -> str:
    """A game this project has no row for: its name as the file says it, without the [tags]."""
    import re
    from .icon import nup_name
    for it in items:   # a NUP may carry the name (what its packer typed)
        if it.kind == "nup" and it.base_title:
            n = nup_name(it.path, it.base_title)
            if n:
                return n
    base = next((it for it in items if any(r == "base" for _, _, r in it.titles)), items[0])
    name = re.sub(r"\[[^\]]*\]|\([^)]*\)", "", base.path.stem).strip(" -_.")
    return name or "未登记的游戏"


def checks_json(fix: bool = False) -> list:
    cs = doctor.checks()
    if fix and any(c.ok is False and c.fix for c in cs):
        cs = doctor.fix(cs)
    return [{"key": c.key, "label": c.label, "ok": c.ok, "detail": c.detail, "how": c.how if c.ok is False else "",
             "fixable": bool(c.fix) and c.ok is False} for c in cs]


def keys_folder() -> str:
    return config.local().get("keys") or next((str(p) for p in config.key_candidates()), "")


def set_folder(kind: str, path: str) -> str:
    """Remember the library or keys folder; '' when a keys folder holds no prod.keys (the file is not opened)."""
    if kind == "keys" and not (Path(path) / "prod.keys").is_file():
        return ""
    config.save_local(**{kind: path})
    return path


_phone_names = {}


def phone_status() -> tuple:
    """(connected, "model · system") for the top bar, polled every few seconds; the names are asked once per phone, and
    again at the next poll while it does not tell them (just plugged in: hdc's "need connect-key")."""
    h = Hdc()
    target = h.pick() if h.available() else ""
    if not target:
        return False, ""
    if target not in _phone_names:
        name = h.model()
        if not name:
            return True, "已连接（读不到型号）"
        _phone_names[target] = name
    return True, _phone_names[target]
