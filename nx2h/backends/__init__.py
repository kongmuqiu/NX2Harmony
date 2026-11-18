"""Game backends: one per adapted (game, version) of games.tsv. A backend turns the player's files into a signed HAP on
the phone with the game data next to it. Stage A wraps the pipelines the two HarmonyOS ports already have."""

BACKENDS = {"totk_v143": "nx2h.backends.totk_v143", "za_202": "nx2h.backends.za_202",
            "oot_v131072": "nx2h.backends.oot_v131072"}
# what doctor.py checks to know a backend's project is there, and where to get it
MARKERS = {"totk_v143": ("native-harmonyos/prepare_v143.py", "NativeGames-harmonyos"),
           "za_202": ("native-harmonyos/za/rewrite_za.py", "LZA"),
           "oot_v131072": ("native-harmonyos/oot/rewrite_oot.py", "OOT")}   # as the OOT backend's docs/DESIGN-OOT.md names it


def load(name: str):
    """The Backend class of a games.tsv backend name. Each module is imported by name here, not through importlib, so
    PyInstaller finds every backend (an importlib string is invisible to it: the packed app lacked oot_v131072)."""
    if name == "totk_v143":
        from . import totk_v143 as m
    elif name == "za_202":
        from . import za_202 as m
    elif name == "oot_v131072":
        from . import oot_v131072 as m
    else:
        raise KeyError(name)
    return m.Backend


def missing(name: str) -> str:
    """'' when the backend's project is where config.backend_root finds it (its MARKERS file), else what to do."""
    from .. import config
    if name not in MARKERS:
        return f"没有 {name} 这个后端"
    marker, folder = MARKERS[name]
    if (config.backend_root(name) / marker).is_file():
        return ""
    game = next((r["name"] for r in config.games() if r.get("backend") == name), name)
    place = config.APP_DIR / "backends" / folder
    how = ("把你手上那份完整的 NX2Harmony 重新完整解压到原处" if config.FROZEN
           else "它是本仓库的一部分，用 git 恢复（git checkout -- backends）")
    return f"找不到{game}的后端 {folder}（应在 {place}）：{how}，或在 {config.LOCAL} 的 backends.{name} 写它的位置"
