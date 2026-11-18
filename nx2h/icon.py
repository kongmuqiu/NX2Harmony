"""Game icons for the library, cached in work/icons/<title id>.jpg: from the player's own files first, else the eShop icon.

- .xci / .nsp / .nsz: the control NCA's icon_<language>.dat (encrypted: needs prod.keys), read in this process with
  the readers in vendor/switchfs
- .nup: the package's meta block, when the packer kept it (nxnative scripts/pack_nup.py: TLV records, 3 = icon,
  10 = localized icons); not encrypted, no keys. Some NUPs carry none (Pokémon Legends: Z-A's has only its NPDM)
- the eShop icon by title ID (fetch_online) for a game whose files carry none: on its own when the library is scanned
  (extract_later, online by default), and from the app's button"""
from __future__ import annotations

import struct
import threading
from pathlib import Path

from . import config

ICONS = config.WORK / "icons"
_busy = set()
_tried = set()   # games whose files gave no icon this run (a NUP without one): the library does not ask again
_lock = threading.Lock()
PKG = struct.Struct("<QIIIIBBHIQI20x")   # a NUP package record (0x40), as pack_nup.py reads it
TLV = struct.Struct("<HHI")
T_ICON, T_NAME, T_LNAMES, T_LICONS = 3, 5, 9, 10
LANGS = ("zh-hans", "zh-cn", "zh", "zh-hant", "zh-tw", "en-us", "en")


def _localized(payload: bytes) -> dict:
    """The localized icons record → {lowercase language tag: image bytes} (pack_nup.py localized)."""
    out, p = {}, 0
    while p < len(payload):
        ln = payload[p]
        tag = payload[p + 1:p + 1 + ln].decode(errors="replace").lower()
        p += 1 + ln
        if p + 4 > len(payload):
            break
        n = struct.unpack_from("<I", payload, p)[0]
        p += 4
        out.setdefault(tag, payload[p:p + n])
        p += n
    return out


def nup_meta(path: Path, base_title: str) -> dict:
    """The application meta records a NUP carries for this game: its base package's block (or the empty base record a
    packer leaves for an update-only container), then each update's, the newest last, a later record winning."""
    from .scan import base_of
    metas = []
    with open(path, "rb") as f:
        head = f.read(0xB0)
        if head[:4] != b"NUP":
            return {}
        for i in range(min(struct.unpack_from("<I", head, 0x84)[0], 64)):
            f.seek(0xB0 + PKG.size * i)
            title, _r, _e, _f, version, kind, _fl, _a, _b, moff, msize = PKG.unpack(f.read(PKG.size))
            if kind > 1 or not msize or msize > 16 << 20 or (title and base_of(f"{title:016X}") != base_title):
                continue
            f.seek(moff)
            b, p, meta = f.read(msize), 0, {}
            while p + TLV.size <= len(b):
                t, _, n = TLV.unpack_from(b, p)
                p += TLV.size
                meta.setdefault(t, b[p:p + n])
                p += n
            if 0 in meta and meta[0][:4] == b"META":
                # numbered from 0 (the nup tool's enum began one below SPEC §5, NPDM = 0x0001): back to the spec's
                meta = {t + 1: v for t, v in meta.items()}
            metas.append((kind, version, meta))
    merged = {}
    for _, _, meta in sorted(metas, key=lambda m: (m[0], m[1])):
        merged.update(meta)
    return merged


def nup_icon(path: Path, base_title: str):
    """The icon a NUP carries for this game (TLV 3, else the best of the localized ones, TLV 10), or None."""
    meta = nup_meta(path, base_title)
    if meta.get(T_ICON):
        return meta[T_ICON]
    icons = _localized(meta.get(T_LICONS, b""))
    return next((icons[l] for l in LANGS if l in icons), next(iter(icons.values()), None))


def nup_name(path: Path, base_title: str) -> str:
    """The game's name a NUP carries (TLV 9 localized, Chinese first, else TLV 5): a label the packer typed."""
    try:
        meta = nup_meta(path, base_title)
    except (OSError, struct.error):
        return ""
    names = {k: v.decode("utf-8", "replace").strip() for k, v in _localized(meta.get(T_LNAMES, b"")).items()}
    name = next((names[l] for l in LANGS if names.get(l)), "")
    return name or meta.get(T_NAME, b"").decode("utf-8", "replace").strip()


def cached(title: str):
    """The picture to show: the game files' icon, else one fetched online."""
    for name in (f"{title.upper()}.jpg", f"{title.upper()}.online.jpg"):
        p = ICONS / name
        if p.is_file():
            return p
    return None


TINFOIL = "https://tinfoil.media/ti/{title}/256/256/"   # the eShop icon by title ID, as many Switch tools show it
TITLEDB = "https://raw.githubusercontent.com/blawar/titledb/master/{region}.json"   # the community title database
REGIONS = ("HK.zh", "US.en")   # Chinese names and icons first, then the US list (it has some titles HK lacks)
INDEX = ICONS.parent / "titledb-index.json"   # {title id: [name, icon URL]}, what the app keeps of the lists
UA = {"User-Agent": "nx2h-icon-fetch"}
# the game's square key art on Nintendo's own Asian site, for games tinfoil and titledb do not list (Switch 2 titles):
# the page code (aa9ja) is the game's product code, which no file we read carries, so it is kept here per title
OFFICIAL = {
    "0400486024DEC000": "https://www.nintendo.com/sg/games/switch2/aa9ja/img/hero_square.jpg",   # 塞尔达传说 时之笛
}
SITE_UA = {"User-Agent": "Mozilla/5.0"}   # nintendo.com turns away clients that do not look like a browser


def _get(url: str, limit: int, timeout: int = 30, headers=None) -> bytes:
    import urllib.request
    chunks, size = [], 0
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers or UA), timeout=timeout) as r:
        while size < limit:
            b = r.read(min(1 << 20, limit - size))
            if not b:
                break
            chunks.append(b)
            size += len(b)
    return b"".join(chunks)


def _is_image(data: bytes) -> bool:
    return data[:2] == b"\xff\xd8" or data[:8] == b"\x89PNG\r\n\x1a\n"


def _load_index() -> dict:
    import json
    try:
        d = json.loads(INDEX.read_text(encoding="utf-8"))
        if isinstance(d, dict) and "titles" in d:
            return d
    except (OSError, ValueError):
        pass
    return {"regions": {}, "titles": {}}


def _lookup(title: str):
    """[name, icon URL] for a title from blawar/titledb (as NX2Apple looks key art up). A regional list (50-90 MB) is
    read only when the regions read so far lack the title, reduced into the small index in the app's data folder and
    not kept; a region read over a week ago is read again for a title it lacked (a newer game)."""
    import json
    import time
    idx = _load_index()
    if title in idx["titles"]:
        return idx["titles"][title]
    for region in REGIONS:
        if time.time() - idx["regions"].get(region, 0) < 7 * 86400:
            continue
        try:
            entries = json.loads(_get(TITLEDB.format(region=region), 400 << 20, timeout=60).decode("utf-8"))
        except (OSError, ValueError):
            continue   # tried again the next time a title needs it
        for e in entries.values():
            tid, url = (e.get("id") or "").upper(), e.get("iconUrl") or ""
            if len(tid) == 16 and url.startswith("https://") and tid not in idx["titles"]:
                idx["titles"][tid] = [e.get("name") or "", url]
        del entries
        idx["regions"][region] = time.time()
        INDEX.parent.mkdir(parents=True, exist_ok=True)
        INDEX.write_text(json.dumps(idx, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        if title in idx["titles"]:
            return idx["titles"][title]
    return None


def fetch_online(title: str) -> bool:
    """A game whose files carry no icon: its eShop icon, looked up by title ID (tinfoil first, then titledb's link to
    Nintendo's own CDN, then the key art on Nintendo's Asian site, OFFICIAL). Saved as <title id>.online.jpg in the
    app's data folder; nothing of it is in the project."""
    title = title.upper()
    data = b""
    try:
        data = _get(TINFOIL.format(title=title), 2 << 20, timeout=15)
    except OSError:
        pass
    if not _is_image(data):
        hit = _lookup(title)
        if hit:
            try:
                data = _get(hit[1], 8 << 20)
            except OSError:
                data = b""
    if not _is_image(data) and title in OFFICIAL:
        try:
            data = _get(OFFICIAL[title], 8 << 20, headers=SITE_UA)
        except OSError:
            data = b""
    if not _is_image(data):
        return False
    ICONS.mkdir(parents=True, exist_ok=True)
    tmp = ICONS / f"{title}.online.tmp"
    tmp.write_bytes(data)
    tmp.replace(ICONS / f"{title}.online.jpg")
    return True


def sources(items: list) -> list:
    """The files that can hold this game's control data, best first: card images, then NSP, then NSZ (a base game
    before an update)."""
    rank = {"xci": 0, "nsp": 1, "nsz": 2}
    keyed = (it for it in items if it.kind in rank)
    return sorted(keyed, key=lambda it: (rank[it.kind], not any(r == "base" for _, _, r in it.titles)))


def extract(title: str, items: list, keys_dir, online: bool = True) -> bool:
    """The game's icon into the app's data folder: from its own files (a NUP's meta block; a card's or NSP / NSZ's
    control data, with prod.keys), else, online, its eShop icon by title ID. True when there is one afterwards."""
    if cached(title):
        return True
    if from_files(title, items, keys_dir):
        return True
    return online and fetch_online(title)


def from_files(title: str, items: list, keys_dir) -> bool:
    """The icon the game's own files carry (no network)."""
    out = ICONS / f"{title.upper()}.jpg"
    for it in items:   # a NUP's meta block needs no keys
        if it.kind == "nup":
            try:
                data = nup_icon(it.path, title.upper())
            except (OSError, struct.error):
                data = None
            if data:
                ICONS.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)
                return True
    keys = Path(keys_dir) / "prod.keys" if keys_dir else None
    if not keys or not keys.is_file():
        return False
    for it in sources(items):
        data = b""
        for t in dict.fromkeys([title.upper()] + [t for t, _, _ in it.titles]):   # an update's control data is its own
            try:
                data = control_icon(it.path, keys, t)
                break
            except Exception:   # not this title's, a damaged file, keys that do not open it: the next one
                continue
        if data[:2] == b"\xff\xd8" or data[:8] == b"\x89PNG\r\n\x1a\n":
            ICONS.mkdir(parents=True, exist_ok=True)
            tmp = out.with_suffix(".tmp")
            tmp.write_bytes(data)
            tmp.replace(out)
            return True
    return False


def control_icon(game: Path, keys: Path, title: str) -> bytes:
    """The icon in a card image's or NSP / NSZ's control data (vendor/switchfs, the Android port's readers, in this
    process: the packed app carries them and cryptography). Chinese icons first."""
    from .vendor.switchfs import extract_app_icon as e
    from .vendor.switchfs.extract_shared_fonts import load_keys
    langs = ["SimplifiedChinese", "TraditionalChinese"] + [l for l in e.LANGUAGES if not l.endswith("Chinese")]
    if e.LANGUAGES != langs:
        e.LANGUAGES = langs
    tmp = config.WORK / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    data, _label = e.read_control_data(Path(game), load_keys(Path(keys)), title.lower(), temp_dir=tmp)
    return data


def forget_tried() -> None:
    """The library's 刷新: games whose files gave no icon are tried again (new files may have one)."""
    with _lock:
        _tried.clear()


def extract_later(title: str, items: list, keys_dir) -> bool:
    """Start extracting in the background (once at a time per game); True while it runs."""
    if cached(title):
        return False
    with _lock:   # a game whose files were tried already this run and gave nothing: not again
        if title in _tried:
            return False
    with _lock:
        if title in _busy:
            return True
        _busy.add(title)

    def run():
        try:
            extract(title, items, keys_dir)
        finally:
            with _lock:
                _busy.discard(title)
                _tried.add(title)
    threading.Thread(target=run, daemon=True).start()
    return True
