"""Each game's HarmonyOS bundle name: made by this app, one per game, never shared, kept once made.

SDK rule (DevEco sdk/default/hms/toolchains/configcheck/configSchema_rich.json, app.bundleName): 7 to 127 bytes, a
reverse domain name of labels made of letters, digits and underscores, each starting with a letter (recommended:
com.<vendor>.<app>). The name made here is nx2h.<this PC>.<the game's slug in games.tsv>, e.g. nx2h.uk3f9a2.totk:
games differ by the slug, players by the PC part, so two players' signing requests never ask for the same name (a
Huawei account registers the names it signs). The PC part is "u" and 6 base-36 characters of a hash of Windows'
MachineGuid, so the app run from the checkout and the packed app (each with its own data folder) give the same names;
without one it is drawn at random once and kept. A name, once made, is kept in nx2h.local.json "bundles": the app on
the phone, its saves and the signing profile are bound to it.

A debug signing profile names one bundle (DevEco's ~/.ohos/config/default_<project>_<hash>.p7b, "bundle-info"), so a
new name needs one automatic signing in DevEco Studio with a Huawei account: signing.py."""
from __future__ import annotations

import hashlib
import re
import secrets
import string

from . import config

RULE = re.compile(r"^[A-Za-z][0-9A-Za-z_]*(\.[A-Za-z][0-9A-Za-z_]*)+$")
B36 = string.digits + string.ascii_lowercase


def valid(name: str) -> bool:
    return bool(RULE.match(name)) and 7 <= len(name.encode("utf-8")) <= 127


def _machine_guid() -> str:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography", 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            return str(winreg.QueryValueEx(k, "MachineGuid")[0])
    except (ImportError, OSError):
        return ""


def pc_part() -> str:
    guid = _machine_guid()
    if guid:
        n = int.from_bytes(hashlib.sha256(("nx2h-bundle:" + guid).encode()).digest()[:8], "big")
        return "u" + "".join(B36[(n // 36 ** i) % 36] for i in range(6))
    u = config.local().get("bundle_uid")
    if not u:
        u = "u" + "".join(secrets.choice(B36) for _ in range(6))
        config.save_local(bundle_uid=u)
    return u


def slug_of(slug: str, title_id: str) -> str:
    s = re.sub(r"[^0-9a-z_]", "", (slug or "").lower()) or "g" + title_id.lower()
    return s if s[0].isalpha() else "g" + s


def for_game(title_id: str, slug: str = "") -> str:
    """The game's bundle name: the one made for it before, else a new one (then kept)."""
    title_id = title_id.upper()
    bundles = config.local().get("bundles") or {}
    if bundles.get(title_id):
        return bundles[title_id]
    s = slug_of(slug, title_id)
    name = f"nx2h.{pc_part()}.{s}"
    taken = set(bundles.values())
    n = 2
    while name in taken:   # two games with one slug: never one name for both
        name = f"nx2h.{pc_part()}.{s}{n}"
        n += 1
    if not valid(name):
        raise ValueError(f"bundle name {name!r} breaks the HarmonyOS rule")
    config.save_local(bundles=dict(bundles, **{title_id: name}))
    return name
