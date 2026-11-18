"""Signing: which bundle names this PC can sign, with which DevEco configuration, and the swap during one HAP build.

DevEco Studio's automatic signing (File > Project Structure > Signing Configs, logged in with a Huawei account) writes,
for the project that is open, a debug certificate and profile to ~/.ohos/config/default_<project>_<hash>.{cer,csr,p12,
p7b} and a "signingConfigs" entry (those paths, the key alias, passwords encrypted with ~/.ohos/config/material) into
that project's build-profile.json5. The profile (.p7b) names the one bundle it is for (bundle-info.bundle-name) and the
phones it installs on. So a bundle name is signable here when a project's build-profile.json5 holds a configuration
whose profile names it: the backends' own projects, and the small signing projects this app makes for a name no
project has signed yet (one per game, in %USERPROFILE%\\.nx2h\\signing: signing belongs to the Windows user, like
~/.ohos, so the app run from the checkout and every copy of the packed app find the same ones).

Nothing of these files is printed, copied off this PC or kept in the app's repository: a configuration is read in this
process and written into the backend's build-profile.json5 (a backend's git must ignore it) for one build, then the original is put
back (also at the next build, after one that was stopped)."""
from __future__ import annotations

import json
import re
import shutil
import time
from pathlib import Path

from . import config

OHOS = Path.home() / ".ohos" / "config"
SIGNING = Path.home() / ".nx2h" / "signing"


def _json5(text: str):
    """The JSON5 that DevEco writes (comments, trailing commas, quoted keys) as Python objects."""
    out, i, n = [], 0, len(text)
    while i < n:   # drop comments outside strings
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            i = text.find("\n", i) if text.find("\n", i) >= 0 else n
        elif text.startswith("/*", i):
            i = text.find("*/", i) + 2 if text.find("*/", i) >= 0 else n
        else:
            out.append(c)
            i += 1
    s = re.sub(r",(\s*[}\]])", r"\1", "".join(out))
    s = re.sub(r'([{,]\s*)([A-Za-z_][A-Za-z0-9_]*)\s*:', r'\1"\2":', s)   # bare keys (none in DevEco's files)
    return json.loads(s)


def profile_info(p7b: Path):
    """{bundle, type, not_after (epoch seconds), devices (count)} of a signing profile, or None. Only these fields are
    read; the device ids themselves are not kept."""
    try:
        data = p7b.read_bytes()
    except OSError:
        return None
    m = re.search(rb'"bundle-name"\s*:\s*"([^"]+)"', data)
    if not m:
        return None
    t = re.search(rb'"type"\s*:\s*"(debug|release)"', data)
    na = re.search(rb'"not-after"\s*:\s*(\d+)', data)
    dev = re.search(rb'"device-ids"\s*:\s*\[([^\]]*)\]', data)
    return {"bundle": m.group(1).decode("utf-8", "replace"), "type": t.group(1).decode() if t else "",
            "not_after": int(na.group(1)) if na else 0, "devices": dev.group(1).count(b'"') // 2 if dev else 0}


def projects() -> list:
    """Project folders whose build-profile.json5 may hold a configuration: the backends and the signing projects."""
    out = []
    for name, p in (config.local().get("backends") or {}).items():
        out.append(Path(p))
    if _signing_dir().is_dir():
        out += sorted(d for d in _signing_dir().iterdir() if d.is_dir())
    return [d for i, d in enumerate(out) if d not in out[:i] and (d / "build-profile.json5").is_file()]


def _configs(project: Path) -> list:
    try:
        bp = _json5((project / "build-profile.json5").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return [c for c in (bp.get("app") or {}).get("signingConfigs") or [] if isinstance(c, dict) and c.get("material")]


def material_for(bundle: str):
    """(signing configuration, project) that signs this bundle name and has not expired, else (None, None): the
    profile this app requested in the background (agc.py) first, then the DevEco projects'."""
    from . import agc
    now = time.time()
    cfg = agc.config_for(bundle)
    if cfg:
        info = profile_info(Path(cfg["material"]["profile"]))
        if info and info["bundle"] == bundle and (not info["not_after"] or info["not_after"] > now + 86400):
            return cfg, agc.KEYS
    for project in projects():
        for c in _configs(project):
            m = c["material"]
            files = [Path(m.get(k) or "") for k in ("certpath", "profile", "storeFile")]
            if not all(f.is_file() for f in files):
                continue
            info = profile_info(Path(m["profile"]))
            if info and info["bundle"] == bundle and (not info["not_after"] or info["not_after"] > now):
                return c, project
    return None, None


def signable(bundle: str) -> bool:
    return material_for(bundle)[0] is not None


def covers(bundle: str, udid: str) -> bool:
    """Whether the signing found for this bundle name lets it install on the phone with this UDID."""
    from . import agc
    cfg, _ = material_for(bundle)
    return bool(cfg) and udid.upper() in agc.device_ids(Path(cfg["material"]["profile"]))


def current_bundle(root: Path) -> str:
    try:
        s = (root / "AppScope" / "app.json5").read_text(encoding="utf-8")
    except OSError:
        return ""
    m = re.search(r'"bundleName"\s*:\s*"([^"]+)"', s)
    return m.group(1) if m else ""


# ---------- one build with another bundle name ----------
def _kept(root: Path) -> Path:
    return root / "prepared" / "nx2h-stage" / "signing"


def restore(root: Path) -> None:
    """Put back the backend's own app.json5 and build-profile.json5 (moved aside by apply())."""
    kept = _kept(root)
    if not kept.is_dir():
        return
    for name, dst in (("app.json5", root / "AppScope" / "app.json5"), ("build-profile.json5", root / "build-profile.json5")):
        src = kept / name
        if src.is_file():
            dst.unlink(missing_ok=True)
            shutil.move(str(src), str(dst))
    absent = kept / "build-profile.absent"
    if absent.is_file():
        (root / "build-profile.json5").unlink(missing_ok=True)
        absent.unlink()


def apply(root: Path, bundle: str) -> str:
    """Make the backend build this bundle name with its signing: "" when it already does, "applied" after the swap,
    or an error message (no configuration on this PC signs the name)."""
    restore(root)
    cfg, _ = material_for(bundle)
    if current_bundle(root) == bundle and any(_signs(c, bundle) for c in _configs(root)):
        return ""
    if cfg is None:
        return f"这台电脑还没有为 {bundle} 签过名"
    kept = _kept(root)
    kept.mkdir(parents=True, exist_ok=True)
    app = root / "AppScope" / "app.json5"
    text = app.read_text(encoding="utf-8")
    shutil.move(str(app), str(kept / "app.json5"))
    app.write_text(re.sub(r'("bundleName"\s*:\s*")[^"]+(")', lambda m: m.group(1) + bundle + m.group(2), text, count=1),
                   encoding="utf-8")
    bp_path = root / "build-profile.json5"
    if bp_path.is_file():
        bp = _json5(bp_path.read_text(encoding="utf-8"))
        shutil.move(str(bp_path), str(kept / "build-profile.json5"))
    else:
        bp = _json5((root / "build-profile.template.json5").read_text(encoding="utf-8"))
        (kept / "build-profile.absent").write_text("", encoding="utf-8")
    cfg = dict(cfg, name="default")
    bp.setdefault("app", {})["signingConfigs"] = [cfg]
    for p in bp["app"].get("products") or []:
        p["signingConfig"] = "default"
    bp_path.write_text(json.dumps(bp, ensure_ascii=False, indent=2), encoding="utf-8")
    return "applied"


def _signs(c: dict, bundle: str) -> bool:
    info = profile_info(Path(c["material"].get("profile") or ""))
    return bool(info) and info["bundle"] == bundle


# ---------- a project for DevEco to sign a new name ----------
SKELETON = ["AppScope", "entry/src/main", "entry/build-profile.json5", "entry/hvigorfile.ts", "entry/oh-package.json5",
            "entry/obfuscation-rules.txt", "entry/consumer-rules.txt", "hvigor", "hvigorfile.ts", "oh-package.json5",
            "code-linter.json5"]


def _signing_dir() -> Path:
    """~/.nx2h/signing, unless the path has characters DevEco projects do not take (then <system drive>/nx2h-signing)."""
    s = str(SIGNING)
    if s.isascii() and " " not in s:
        return SIGNING
    import os
    return Path(os.environ.get("SystemDrive", "C:") + "\\") / "nx2h-signing"


def project_for(bundle: str, slug: str, backend_root: Path) -> Path:
    """A project to open in DevEco Studio once, so its automatic signing issues a profile for this bundle name: the
    backend's app configuration (the same module.json5, so the profile carries the permissions the game asks for)
    without its code and data. Kept in ~/.nx2h/signing/NX2H_<slug>; DevEco writes the configuration into it."""
    d = _signing_dir() / ("NX2H_" + re.sub(r"[^0-9A-Za-z_]", "_", slug or bundle.split(".")[-1]))
    for rel in SKELETON:
        src, dst = backend_root / rel, d / rel
        if not src.exists() or dst.exists():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_dir():
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns("build", ".preview", "oh_modules", ".cxx"))
        else:
            shutil.copy2(src, dst)
    bp = d / "build-profile.json5"
    if not bp.exists():
        shutil.copy2(backend_root / "build-profile.template.json5", bp)
    app = d / "AppScope" / "app.json5"
    text = app.read_text(encoding="utf-8")
    app.write_text(re.sub(r'("bundleName"\s*:\s*")[^"]+(")', lambda m: m.group(1) + bundle + m.group(2), text, count=1),
                   encoding="utf-8")
    return d


def deveco_exe() -> Path:
    from .device import DEVECO
    return DEVECO / "bin" / "devecostudio64.exe"


def open_in_deveco(project: Path) -> bool:
    from . import proc
    exe = deveco_exe()
    if not exe.is_file():
        return False
    proc.popen([str(exe), str(project)])
    return True


HOWTO = ("用 DevEco Studio 打开它（软件会自动打开），登录华为账号，"
         "File → Project Structure → Signing Configs → 勾选 Automatically generate signature → OK；"
         "手机连着电脑时签，签名里会带上这台手机。完成后回到这里重新构建。")
