"""Tears of the Kingdom 1.4.3: the NativeGames-harmonyos pipeline (0.5.0), driven step by step.

  inputs   the base game (the card .xci: 1.0.0, or a card that already carries 1.2.1; or the eShop 1.0.0 .nsp / .nsz, or a .xcz:
           the backend decompresses NSZ / XCZ itself, v143/merge_v143.sh) and the 1.4.3 update (.nsz), prod.keys
  check    first the totk-android checkout (native-harmonyos/upstream.py --check, za_202.upstream_ready): no step runs
           against a missing or wrong reference
  prepare  native-harmonyos/prepare_v143.py --only <step>, one call per step so each gets its own progress line; a
           step whose output exists says "done before" and is reported as reused (the script decides, not this file).
           The card and the update go in as folders holding only them (_stage: a hard link, else a copy, removed once
           read); the shared fonts come from nx2h/fonts.py (firmware beside the keys, else a card image), not from
           prepare_v143.py's own fonts step; a guest step the script takes for done while its last part is missing
           runs prepare_guest.py v143 again
  screen   the phone's screen (hidumper's screen[0], or nx2h build --screen): wider than 16:9, the patches step also
           makes romfs-patches-<W>x<H> (--aspect), the UI laid out for it with the HUD at the screen's edges; the app
           picks that overlay by its own screen (totk_napi.c) and turns its UI squeeze off
  libs     prepare_v143.py --only libs (build_ohos.py is incremental)
  hap      native-harmonyos/build_hap.ps1 -Tag <tag> with the game's bundle name, signing and icon (common.py)
  install  hdc install -r, then one start so libtotk_ohos.so creates the game folders in the sandbox; with no env.txt
           yet that start first writes the settings page's one (its stored or default settings, and the screen's size:
           entryability/EntryAbility.ets), so the runtime also makes the screen's overlay folder
  push     the files v143/push_v143.ps1 sends, to the same places; a file the phone already has is skipped (same size and
           16 sampled MiB with the same SHA-256); the screen's overlay beside romfs-patches, when the app has made its
           folder (its first start does, and every 开始游戏 with 画面比例 全面屏), else left for a later build"""
from __future__ import annotations

import os
import re
import shutil
import time
from pathlib import Path

from .. import config
from .. import mirrors
from .. import proc
from . import za_202
from .common import BaseBackend, have_docker

STEPS = [("extract", "解开 1.4.3 更新"), ("merge", "合并本体和更新的 RomFS"), ("patches", "RomFS 补丁文件"),
         ("fonts", "系统字体"), ("xlate", "着色器翻译器"), ("rewrite", "改写游戏代码"), ("guest", "客体数据"),
         ("seed", "着色器种子（首次约 3 分钟）"), ("deps", "第三方静态库")]


def guest_files(root: Path) -> list:
    """What the guest step (prepare_guest.py v143) writes, its last files last: the runtime tables (in the container,
    after the export), gen_embed_ohos.py's sources (guest-manifest.json last), then game_data_ohos.py's
    runtime-index.bin (when missing), romfs-config.txt and shared-fonts.txt."""
    o = root / "prepared" / "harmonyos-v143"
    return [o / "game-data/runtime-tables.bin", o / "embed/guest_modules.c", o / "embed/guest-manifest.json",
            root / "prepared/totk-v143/extracted/v143/romfs/runtime-index.bin", o / "game-data/romfs-config.txt",
            o / "game-data/shared-fonts.txt"]


def state(root: Path = None) -> dict:
    """What the backend has prepared already (decides whether keys, game files and disk space are still needed): extract,
    merge and seed by prepare_v143.py's own "done before" checks, guest by every file that step writes (guest_files)."""
    root = root or config.backend_root("totk_v143")
    w, o = root / "prepared" / "totk-v143", root / "prepared" / "harmonyos-v143"
    v = w / "extracted" / "v143"
    return {
        "extract": (v / "exefs/main").is_file() and (v / "elf").is_dir(),
        "merge": (v / "romfs/romfs.bin").is_file() and (v / "romfs/index.json").is_file(),
        "fonts": (w / "shared-fonts/manifest.json").is_file(),
        "guest": all(f.is_file() for f in guest_files(root)),
        "seed": (o / "seed-build-sb256/shaders/v143/seed.vkpack").is_file(),
        "opus": (root / "artifacts/native-opus/build-ohos/opus/libopus.a").is_file(),
    }


class Backend(BaseBackend):
    key = "totk_v143"
    title_id = "0100F2C0115B6000"
    name = "塞尔达传说 王国之泪"
    version = "1.4.3"
    update = "v720896"
    slug = "totk"
    log_name = "totk"
    screen = None   # (width, height) from nx2h build --screen; without it the phone is asked
    _screen_skip = False   # totk-ohos: push() found no folder on the phone for the screen's overlay

    upstream = za_202.Backend.upstream               # the totk-android checkout (ohos_env.UPSTREAM)
    upstream_ready = za_202.Backend.upstream_ready   # upstream.py --check, before the first step
    _fonts = za_202.Backend._fonts                   # nx2h/fonts.py's shared fonts into self.fonts

    def __init__(self, ev, hdc):
        super().__init__(ev, hdc)
        self.fonts = self.prep / "totk-v143" / "shared-fonts"   # prepare_v143.py's place; its guest step reads them there
        self._upstream = None

    # ---------- the screen ----------
    def wide_screen(self):
        """The screen the UI layouts are made for, (width, height) in landscape pixels, when it is wider than 16:9 by
        more than 1% (the rule totk_napi.c picks the overlay by), else None. Decided once: --screen, else the phone's."""
        if hasattr(self, "_wide"):
            return self._wide
        self._wide, size, where = None, self.screen, "--screen"
        if not size:
            target = self.hdc.pick() if self.hdc.available() else ""
            size, where = (self.hdc.screen() if target else None), "手机"
            if not size:
                self.ev.say(("    还不知道手机屏幕（没有连接手机）" if not target else "    读不出手机屏幕的分辨率（hidumper）")
                            + "：这次不按屏幕生成界面布局，游戏里 HUD 先在中间 16:9。连上手机再构建一次就会补上，"
                              "也可以用 --screen 宽x高 指定")
                return None
        w, h = size
        if w * 9 <= h * 16 * 1.01:
            self.ev.say(f"    屏幕 {w}×{h}（{where}）没有比 16:9 宽出 1% 以上，用游戏自己的界面布局")
        else:
            self._wide = (w, h)
            self.ev.say(f"    屏幕 {w}×{h}（{where}）比 16:9 宽：按它生成界面布局，HUD 贴到屏幕两边（设置“画面”里可改成居中 16:9）")
        return self._wide

    # ---------- inputs ----------
    # the base game's file kinds, the first found used: the card image (verified first, and it carries the system fonts),
    # then the eShop package; the compressed ones are decompressed by the merge step (NSZ's hash-verified conversion)
    BASE_KINDS = ("xci", "nsp", "xcz", "nsz")

    def pick_inputs(self, items) -> tuple:
        base = sorted((it for it in items if it.kind in self.BASE_KINDS and any(r == "base" for _, _, r in it.titles)),
                      key=lambda it: self.BASE_KINDS.index(it.kind))
        base = [it.path for it in base]
        upd = [it.path for it in items if it.kind == "nsz" and any(r == "update" and v == self.update for _, v, r in it.titles)]
        problems = []
        if not base:
            problems.append("缺本体（卡带 .xci，或 eShop 版 .nsp / .nsz）")
        if not upd:
            problems.append(f"缺 1.4.3 更新 .nsz（[{self.title_id[:13]}800][{self.update}]）")
        return {"base": base[0] if base else None, "update": upd[0] if upd else None}, problems

    def needs_inputs(self) -> bool:
        st = state(self.root)
        return not (st["extract"] and st["merge"])   # the fonts come from firmware or one of the player's own card images (needs_keys)

    def needs_keys(self) -> bool:
        from .. import fonts
        st = state(self.root)
        return self.needs_inputs() or not (st["fonts"] or fonts.ready())   # push() sends the font files themselves

    # ---------- the card and the update as folders ----------
    def _stage_dirs(self, kind: str, src: Path) -> list:
        return [self.prep / "nx2h-stage" / kind, config.WORK / "stage" / "totk_v143" / kind, src.parent / ".nx2h-stage" / kind]

    @staticmethod
    def _same(src: Path, dst: Path) -> bool:
        """dst is src: a hard link of it, or a whole copy (same size and time: copystat)."""
        try:
            if os.path.samefile(src, dst):
                return True
            a, b = src.stat(), dst.stat()
        except OSError:
            return False
        return a.st_size == b.st_size and abs(a.st_mtime - b.st_mtime) < 2

    @staticmethod
    def _clear(d: Path, keep: str) -> None:
        for old in d.iterdir():
            if old.name != keep and old.is_file():
                old.unlink()

    def _copy(self, src: Path, dst: Path) -> None:
        """src to dst through dst.part, with progress; an interrupted copy leaves nothing behind."""
        part = dst.with_name(dst.name + ".part")
        size, done, shown = src.stat().st_size, 0, 0.0
        step = f"复制 {src.name}"
        try:
            with open(src, "rb") as f, open(part, "wb") as g:
                for b in iter(lambda: f.read(8 << 20), b""):
                    g.write(b)
                    done += len(b)
                    if time.time() - shown > 0.5:
                        self.ev.progress(step, done, size, unit="B")
                        shown = time.time()
            shutil.copystat(str(src), str(part))
            os.replace(str(part), str(dst))
        except BaseException:
            try:
                part.unlink()
            except OSError:
                pass
            raise
        finally:
            if shown and not self.ev.json:
                print()

    def _stage(self, kind: str, src: Path):
        """A folder holding only this file (prepare_v143.py takes folders and mounts them into its containers, so a file
        path cannot go in): the file's own folder when the backend would take this file from it (prepare_v143.base_file
        and merge_v143.sh take the first base kind present, in BASE_KINDS order, so for the base only files of its own
        kind or an earlier one count: a card beside the update .nsz is used where it is; the update: the only .nsz);
        else a hard link (no copy) in the backend's ignored prepared/nx2h-stage, this app's work folder or a .nx2h-stage
        folder beside the file; else (another drive, a network or VM shared folder, exFAT: no hard link) a copy with
        progress in the first of the first two places with room for it. None (said why): none of these."""
        src = Path(src)
        own = src.suffix.lower()[1:]
        rank = self.BASE_KINDS.index(own) if own in self.BASE_KINDS else len(self.BASE_KINDS) - 1
        kinds = {"." + k for k in self.BASE_KINDS[:rank + 1]} if kind == "base" else {".nsz"}
        if [p.name for p in src.parent.iterdir() if p.is_file() and p.suffix.lower() in kinds] == [src.name]:
            return src.parent
        places = self._stage_dirs(kind, src)
        for d in places:
            dst = d / src.name
            try:
                d.mkdir(parents=True, exist_ok=True)
                self._clear(d, src.name)
                if dst.exists() and not self._same(src, dst):
                    dst.unlink()
                if not dst.exists():
                    os.link(src, dst)
                return d
            except OSError:
                continue
        size = src.stat().st_size
        for d in places[:2]:
            dst = d / src.name
            try:
                d.mkdir(parents=True, exist_ok=True)
                if dst.exists() and self._same(src, dst):
                    return d
                self._clear(d, "")
                if shutil.disk_usage(str(d)).free < size + (1 << 30):
                    continue
                self.ev.say(f"    {src.parent} 里还有别的游戏文件，又建不了硬链接：复制 {src.name} 到 {d}"
                            f"（{size / 2**30:.1f} GB，用完就删）")
                self._copy(src, dst)
                return d
            except OSError as e:
                self.ev.say(f"    复制到 {d} 失败：{e}")
        self.ev.red(f"{src.parent} 里还有别的游戏文件，建不了硬链接，也没有 {size / 2**30:.1f} GB 空位复制："
                    f"把 {src.name} 单独放进一个文件夹再试")
        return None

    def _unstage(self, kind: str, src) -> None:
        """The links and copies made for an input whose step is done (a copy of the card is 16 GB): a file there with
        another link, or a whole copy of src. One that is the last of its kind (no other link, no original) stays."""
        if not src:
            return
        src = Path(src)
        for d in self._stage_dirs(kind, src):
            try:
                if not d.is_dir() or d.resolve() == src.parent.resolve():
                    continue
                for f in d.iterdir():
                    if not f.is_file():
                        continue
                    copy = f.name == src.name and src.is_file() and not os.path.samefile(src, f) and self._same(src, f)
                    if f.stat().st_nlink > 1 or copy:
                        f.unlink()
                d.rmdir()
                if d.parent.name == ".nx2h-stage":
                    d.parent.rmdir()
            except OSError:
                continue

    # ---------- steps ----------
    def _guest_again(self) -> bool:
        """prepare_guest.py v143 itself: prepare_v143.py takes the guest step for done once the embed and the runtime
        tables are there, also when its last part (game_data_ohos.py) failed."""
        if not self._ensure_image(za_202.D2R, self.root / "tools" / "docker" / "d2r.Dockerfile"):
            return False
        env = dict(self._env(), TOTK_SHARED_FONTS=str(self.fonts))
        return self._run("guest", [config.python(), "-I", str(self.root / "native-harmonyos" / "prepare_guest.py"), "v143"],
                         env=env)[0] == 0

    def _done_without_script(self, step: str) -> bool:
        """prepare_v143.py's own "done before" checks for one step (prepare_v143.py:136-252): for a PC without Docker, and
        to know whether a step will download anything (_downloads)."""
        w, o = self.prep / "totk-v143" / "extracted" / "v143", self.prep / "harmonyos-v143"
        st = state(self.root)
        if step == "extract":
            return st["extract"]
        if step == "merge":
            return (w / "romfs" / "index.json").is_file() and (w / "romfs" / "romfs.bin").is_file()
        if step == "patches":
            wide = self.wide_screen()
            return (w / "romfs-patches" / "manifest.tsv").is_file() and \
                (not wide or (w / ("romfs-patches-%dx%d" % wide) / "manifest.tsv").is_file())
        if step == "xlate":
            return (self.root / "artifacts" / "xlate-ohos" / "totk_xlate").is_file()
        if step == "rewrite":
            return (o / "rewritten" / "phase12-validation.json").is_file()
        if step in ("guest", "seed"):
            return st[step]
        if step == "deps":
            return all((self.root / "artifacts" / lib).is_file() for lib in self.PREBUILT.values())
        return False

    def _downloads(self, step: str):
        """What a step that still has to run downloads, from the sources that answer fastest (nx2h/mirrors.py, measured
        only now that something is needed): the published images it runs in, pulled first (common._ensure_base), and
        the environment of its attempts, which prepare_v143.py hands into its containers (PIP_INDEX_URL for the Python
        environment extract makes in the totk-venv volume; the Ubuntu archive and NuGet feed of the translator build,
        a second set when the first fails). [{}] when it downloads nothing; None when an image could not be had."""
        if step in ("extract", "merge", "patches", "seed") and not self._ensure_base("python:3.14"):
            return None
        if step == "extract" and proc.run(["docker", "volume", "inspect", "totk-venv"], capture_output=True).returncode:
            # prepare_v143.py hands NX2H_PIP_INDEX_URL into its containers as PIP_INDEX_URL (never the host's own)
            return [{"NX2H_" + k: v for k, v in mirrors.pip_env(self.ev.say).items()}]
        if step == "xlate":
            if not self._ensure_base(za_202.DOTNET):
                return None
            return mirrors.xlate_envs(self.ev.say)
        return [{}]

    def _fonts_reachable(self, inputs: dict, keys, library: list) -> bool:
        """A base that is not a card image (.nsp / .nsz / .xcz) carries no system fonts: they must come from where
        fonts.extract looks (a firmware dump near the keys or nx2h.local.json "firmware", one of the player's own card images in the library).
        Checked before the long steps, not after them at the fonts step."""
        from .. import fonts
        base = inputs.get("base")
        if not base or Path(base).suffix.lower() == ".xci" or state(self.root)["fonts"] or fonts.ready():
            return True
        fw = config.local().get("firmware")
        if (fw and Path(fw).is_dir()) or (keys and fonts.firmware_folders(Path(keys))) \
                or any(getattr(it, "kind", "") == "xci" for it in library or []):
            return True
        self.ev.red("本体是 eShop 版（.nsp / .nsz）或 .xcz，不带系统字体：把你主机导出的固件放在 prod.keys 所在文件夹旁边，"
                    "或在游戏文件夹里放一张你自己卡带的 .xci 镜像，再构建")
        return False

    def prepare(self, inputs: dict, keys, library: list) -> bool:
        if not self.upstream_ready():
            return False
        if not self._fonts_reachable(inputs, keys, library):
            return False
        script = self.root / "native-harmonyos" / "prepare_v143.py"
        o = self.prep / "harmonyos-v143"
        for step, label in STEPS:
            self.ev.step(label)
            st = state(self.root)
            if step in ("rewrite", "guest") and not st["guest"]:
                # the tools image, built here with retries and Debian mirrors (prepare_v143.py builds it once, with neither)
                if not self._ensure_image(za_202.D2R, self.root / "tools" / "docker" / "d2r.Dockerfile"):
                    return False
            if step == "fonts":
                # nx2h/fonts.py (firmware beside the keys, else one of the player's own card images), kept where prepare_v143.py's guest step
                # reads them; that script's own fonts step (from the base card only) is not run
                if st["fonts"]:
                    self.ev.say("    已有，跳过")
                elif not self._fonts(keys, library):
                    return False
                listing = o / "game-data" / "shared-fonts.txt"
                want = self._font_want(listing)
                if want and not self._fonts_in(self.fonts, want):
                    # game data made from other fonts (another firmware's): the guest step lists these ones again
                    listing.unlink()
                    self.ev.say("    游戏数据里登记的是另一份字体，客体数据一步会按这份重新生成")
                continue
            lines = []
            if not have_docker():
                # prepare_v143.py stops at once without Docker ("docker is not on PATH"), even for a step done before. On a
                # PC that got the prepared data from another one (no Docker here), its own done checks are made here
                if step == "deps":
                    self._prebuilt()
                if self._done_without_script(step):
                    self.ev.say("    已有，跳过")
                    continue
                self.ev.red(f"{label}：这一步要用 Docker Desktop（在“环境”页安装并启动），或者把在别的电脑上准备好的数据放进 {self.prep}")
                return False
            if step == "guest" and not st["guest"] and (o / "embed/guest_modules.c").is_file() \
                    and (o / "game-data/runtime-tables.bin").is_file():
                self.ev.say("    客体数据不完整，重新生成")
                if not self._guest_again():
                    return False
            else:
                args = ["--keys", str(keys)] if keys else []
                # the card / the update only for the step that still reads it
                for kind, flag, needed in (("update", "--update", step == "extract" and not st["extract"]),
                                           ("base", "--base", step == "merge" and not st["merge"])):
                    if needed and inputs.get(kind):
                        d = self._stage(kind, inputs[kind])
                        if not d:
                            return False
                        args += [flag, str(d)]
                extra = []
                if step == "deps":
                    self._prebuilt()   # prepare_v143.py reports "present" for libraries already in artifacts/
                if step == "patches" and self.wide_screen():
                    extra = ["--aspect", "%dx%d" % self.wide_screen()]   # totk-ohos: also romfs-patches-<W>x<H>, the UI laid out for it
                runs = [{}] if self._done_without_script(step) else self._downloads(step)
                if runs is None:
                    return False
                for i, more in enumerate(runs):
                    if i:
                        self.ev.say("    换下一组下载源再试一次")
                    code, lines = self._run(step if not i else f"{step}-{i + 1}",
                                            [config.python(), "-I", str(script), "--only", step] + args + extra,
                                            env=dict(self._env(), **more) if more else None)
                    if not code:
                        break
                if code:
                    return False
            if step in st and not state(self.root)[step]:
                gone = [f.name for f in guest_files(self.root) if not f.is_file()] if step == "guest" else []
                self.ev.red(f"{label}：运行结束但没有产出预期的文件" + (f"（缺 {'、'.join(gone)}）" if gone else "")
                            + f"，看日志 {config.LOGS}")
                return False
            if step == "extract":
                self._unstage("update", inputs.get("update"))
            elif step == "merge":
                self._unstage("base", inputs.get("base"))
            # reused: the script found its outputs and ran nothing (a "[hh:mm:ss] $ <command>" line is a command it ran)
            ran = any(re.match(r"\[[\d:]+\] \$ ", l) for l in lines)
            reused = not ran and any("done before" in l or "present" in l for l in lines)
            self.ev.say(f"    {'已有，跳过' if reused else '完成'}")
        return True

    def libs(self) -> bool:
        if not self._embed_here("prepared/harmonyos-v143"):
            return False
        if not have_docker():
            # what prepare_v143.py --only libs runs (prepare_v143.py:257-258), called directly: that script stops without
            # Docker before any step, and this one needs only DevEco Studio's clang
            return self._run("libs", [config.python(), "-I", str(self.root / "native-harmonyos" / "build_ohos.py"), "--gpu", "vk",
                                      "--version", "v143", "--guest", str(self.prep / "harmonyos-v143" / "embed")],
                             cwd=self.root)[0] == 0
        script = self.root / "native-harmonyos" / "prepare_v143.py"
        return self._run("libs", [config.python(), "-I", str(script), "--only", "libs"])[0] == 0

    # ---------- push ----------
    def push_list(self) -> list:
        """As v143/push_v143.ps1 -GameData -RomFS -Seed -Patches sends them."""
        g = self.prep / "harmonyos-v143" / "game-data"
        r = self.prep / "totk-v143" / "extracted" / "v143" / "romfs"
        s = self.prep / "harmonyos-v143" / "seed-build-sb256" / "shaders" / "v143"
        out = [(g / f, f"game-v143/{f}") for f in ("runtime-tables.bin", "romfs-config.txt", "shared-fonts.txt")]
        out += [(r / f, f"game-v143/{f}") for f in ("runtime-index.bin", "romfs.bin")]
        out += [(s / f, f"android/shaders/v143/{f}") for f in ("sb-align.txt", "seed.vkpack")]
        # totk-ohos: the console's shared fonts into game-v143/fonts, which the app makes at start (totk_napi.c
        # TOTK_SHARED_FONTS). Chinese / Korean text reads all six and a missing one stops the game at the title; English
        # never asks for them, so while nothing pushed them (until 2026-11-15) only choosing Chinese crashed.
        out += [(p, f"game-v143/fonts/{p.name}") for p in self.font_files(g / "shared-fonts.txt")]
        v = self.prep / "totk-v143" / "extracted" / "v143"
        wide = self.wide_screen()
        # totk-ohos: the overlay for this phone's screen (romfs-patches-<W>x<H>) beside the base one; the app picks it
        # (not when push() found that the phone has no folder for it)
        for name in ["romfs-patches"] + (["romfs-patches-%dx%d" % wide] if wide and not self._screen_skip else []):
            patches = v / name
            # the manifest first: the app makes the overlay's subfolders from it when its runtime starts (totk_napi.c)
            files = sorted((p for p in patches.rglob("*") if p.is_file()), key=lambda p: (p.name != "manifest.tsv", p.as_posix()))
            for p in files or [patches / "manifest.tsv"]:   # none: push() names the missing manifest
                out.append((p, f"android/extracted/v143/{name}/" + p.relative_to(patches).as_posix()))
        return out

    @staticmethod
    def _font_want(listing: Path) -> dict:
        """{file name: SHA-256} of the fonts shared-fonts.txt names (FONT<n>_FILE, FONT<n>_SHA256); {} without it."""
        if not listing.is_file():
            return {}
        rows = dict(l.split(None, 1) for l in listing.read_text(encoding="utf-8").splitlines() if " " in l.strip())
        return {rows[k].strip(): rows.get(k[:-5] + "_SHA256", "").strip().lower()
                for k in rows if re.fullmatch(r"FONT\d+_FILE", k)}

    @staticmethod
    def _fonts_in(d: Path, want: dict) -> bool:
        import hashlib
        return bool(want) and all((d / n).is_file() and hashlib.sha256((d / n).read_bytes()).hexdigest() == h
                                  for n, h in want.items())

    def font_files(self, listing: Path) -> list:
        """The six .bfttf files shared-fonts.txt names, from the first folder that holds all of them with those hashes:
        prepared/totk-v143/shared-fonts (the fonts step) or nx2h's own work/shared-fonts (nx2h/fonts.py, kept for every
        game). None whole: the prepared paths, which push() reports as missing."""
        from .. import fonts
        want = self._font_want(listing)
        if not want:
            return []
        for d in (self.fonts, fonts.FONTS):
            if self._fonts_in(d, want):
                return [d / n for n in sorted(want)]
        return [self.fonts / n for n in sorted(want)]

    def push(self) -> bool:
        """totk-ohos: the screen's overlay goes only into a folder the app has made. hdc cannot make folders in the
        sandbox, and totk_napi.c makes romfs-patches-<W>x<H> only when env.txt's TOTK_ASPECT names this screen (画面比例
        全面屏). The settings page writes env.txt when 开始游戏 is tapped, and the start that install() makes writes it
        (from settings.json, or the defaults on a fresh install) when there is no env.txt yet, so a fresh install has the folder in this same run. Still missing (16:9
        chosen, or an env.txt for another screen size): the push goes on without that overlay (the HUD stays in the
        middle 16:9) instead of failing on it."""
        wide = self.wide_screen()
        if wide and not self._screen_skip:
            name = "romfs-patches-%dx%d" % wide
            if "No such file" in self.hdc.shell(f"ls -d '{self.shell_files}/android/extracted/v143/{name}' 2>&1"):
                self._screen_skip = True
                self.ev.say(f"    手机上还没有 {name} 文件夹（App 只在“画面比例”为全面屏、按这块屏幕的尺寸启动时建它，设置里可能选了"
                            " 16:9）：这次不推这块屏幕的界面布局，HUD 先在中间 16:9。设置里选全面屏、点一次“开始游戏”后再构建一次就会补上")
        return super().push()
