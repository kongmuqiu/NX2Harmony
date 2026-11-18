"""Pokémon Legends: Z-A 2.0.2 (Switch 2 Edition, NVN 55): the LZA port's scripts, in the order they were run by hand.

  inputs   the game's NUP holding the 2.0.2 update (v393216); prod.keys only for the shared fonts (the NUP has none)
  check    first the totk-android checkout (upstream_ready: native-harmonyos/upstream.py --check); a step that mounts a
           folder read-only refuses a missing one (_docker), which docker would make an empty one
  unpack   tools/nup_extract.py <nup> --out prepared/nup --parts update   (every file's SHA-256 checked; zstandard)
  rewrite  native-harmonyos/za/rewrite_za.py in nativegames-tools:1: main and sdk, against assets/expected-za-2.0.2.json
  tables   native-harmonyos/za/prepare_runtime_za.py -> prepared/za/game-data/runtime-tables.bin
  romfs    native-harmonyos/za/pack_romfs_za.py -> prepared/za/romfs/{romfs.bin, index.json}
  fonts    the console's shared fonts (nx2h/fonts.py: a card image or firmware with prod.keys) -> prepared/za/shared-fonts
  xlate    native-harmonyos/xlate/build_xlate.sh in dotnet/sdk:10.0-noble-aot -> artifacts/xlate-ohos/totk_xlate
  guest    native-harmonyos/prepare_guest.py za202 (nativegames-tools:1): main's text exported with the game
           patches (overlay/runtime/game_patches_za.c: the wide-screen camera, through runtime/aspect_patch.h) into
           prepared/za/x0/main.x0.bin, then gen_embed_ohos.py -> prepared/za/embed (the guest library sources, which carry
           the exported text's SHA-256). Done again when the patch sources are newer than the export, or the export newer
           than the embed
  gamedata native-harmonyos/game_data_ohos.py -> romfs-config.txt, shared-fonts.txt (and runtime-index.bin)
  deps     libopus / libvpx static libraries: the TOTK backend's when it has them (same pinned sources), else built
  libs     native-harmonyos/build_ohos.py --gpu vk --version za202 --guest prepared/za/embed
  hap, install, push   common.py; push as za/push_za.ps1 -GameData -Fonts -RomFS -Seed
  seed     the shader seed comes from the phone the first time: the app started with ZA_SHADER_DUMP decodes every shader
           container of data.trpfs with the game's own Oodle (runtime/za_shader_dump.c, about 50 s, 7.5 GB) and exits;
           za/za_seed.py --pull --send brings it to the PC, translates every shader for the default bind state in
           python:3.14 containers and sends seed.vkpack; the dump is then removed from the phone (ZA_SHADER_DUMP_REMOVE).
           Diagnostics variables reach the app as files/android/env-dev-<time>.txt (za/devenv.ps1), the last one winning.
Every step whose output is there is skipped, so a stopped build continues where it stopped."""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from .. import config
from .. import mirrors
from .. import proc
from .common import APP_FILES, BaseBackend

PART = "0100F43008C44000-v393216-update"     # tools/nup_extract.py: <title>-v<version>-<type>
D2R = "nativegames-tools:1"
DOTNET = "mcr.microsoft.com/dotnet/sdk:10.0-noble-aot"
SPIRV_CROSS = "aa217aeb6c9f0ace7a0ab233b28807edf45eb165"   # native-harmonyos/build_seed.sh's pin
DUMP_DIR = f"{APP_FILES}/android/za-dump"
UPSTREAM_MARKER = "NX2HARMONY-UPSTREAM.txt"   # upstream.py MARKER: a bundled totk-android copy (packaging/full_bundle.py)


def missing_mount(folder: Path, guest: str) -> str:
    """What to tell the player when a folder a step mounts read-only is not there, by what it is for."""
    if guest == "/android":
        return (f"参考仓库 totk-android 不在（{folder}）：在“环境”页修复"
                + ("，或把你手上那份完整的 NX2Harmony 重新完整解压到原处" if config.FROZEN else ""))
    if guest == "/ohos/sysroot":
        return f"HarmonyOS SDK 不完整（缺 sysroot：{folder}）：打开一次 DevEco Studio，等 SDK 下载完"
    return f"{folder} 不存在"


class Backend(BaseBackend):
    key = "za_202"
    title_id = "0100F43008C44000"
    name = "宝可梦传说 Z-A"
    version = "2.0.2"
    update = "v393216"
    slug = "za"
    log_name = "za"

    def __init__(self, ev, hdc):
        super().__init__(ev, hdc)
        p = self.prep
        self.part = p / "nup" / PART
        self.exefs = self.part / "exefs"
        self.za = p / "za"
        self.rewritten = self.za / "rewrite" / "rewritten"
        self.game_data = self.za / "game-data"
        self.romfs = self.za / "romfs"
        self.fonts = self.za / "shared-fonts"
        self.embed = self.za / "embed"
        self.seed_build = self.za / "seed-build"
        self.seed = self.seed_build / "shaders" / "za202"
        self.xlate = self.root / "artifacts" / "xlate-ohos" / "totk_xlate"
        self.opus = self.root / "artifacts/native-opus/build-ohos/opus/libopus.a"
        self.vpx = self.root / "artifacts/native-vpx/ohos/lib/libvpx.a"
        self._upstream = None
        self._unpinned = False

    # ---------- what is there ----------
    def _done(self) -> dict:
        return {
            "unpack": (self.part / ".part.json").is_file() and (self.exefs / "main").is_file(),   # nup_extract writes it last
            "rewrite": (self.rewritten / "phase12-validation.json").is_file(),
            "tables": (self.game_data / "runtime-tables.bin").is_file(),
            "romfs": (self.romfs / "romfs.bin").is_file() and (self.romfs / "index.json").is_file(),
            "fonts": (self.fonts / "manifest.json").is_file(),
            "xlate": self.xlate.is_file(),
            "embed": self._guest_current(),
            "gamedata": all((self.game_data / f).is_file() for f in ("romfs-config.txt", "shared-fonts.txt"))
                        and (self.romfs / "runtime-index.bin").is_file(),
            "deps": self.opus.is_file() and self.vpx.is_file(),
            "seed": (self.seed_build / "seed.vkpack").is_file() and (self.seed / "sb-align.txt").is_file(),
        }

    def _guest_current(self) -> bool:
        """The guest sources come from main's exported text (the wide-screen camera), and that from today's patches."""
        x0, mod = self.za / "x0" / "main.x0.bin", self.embed / "guest_modules.c"
        if not (x0.is_file() and mod.is_file()):
            return False
        nh = self.root / "native-harmonyos"
        sources = [nh / "overlay/runtime/game_patches_za.c", nh / "overlay/runtime/aspect_patch.h", nh / "tools/export_text.c",
                   self.rewritten / "phase12-validation.json"]
        newest = max((p.stat().st_mtime for p in sources if p.is_file()), default=0)
        return x0.stat().st_mtime >= newest and mod.stat().st_mtime >= x0.stat().st_mtime

    def needs_inputs(self) -> bool:
        d = self._done()
        return not (d["rewrite"] and d["tables"] and d["romfs"])

    def needs_keys(self) -> bool:
        from .. import fonts
        return not self._done()["fonts"] and not fonts.ready()

    def pick_inputs(self, items) -> tuple:
        nups = [it.path for it in items if it.kind == "nup" and any(r == "update" and v == self.update for _, v, r in it.titles)]
        problems = [] if nups else [f"缺宝可梦传说 Z-A 的 NUP（要含 2.0.2 更新 [{self.update}]）"]
        return {"nup": nups[0] if nups else None}, problems

    def upstream(self) -> Path:
        """The totk-android checkout the backend uses (its ohos_env.py: TOTK_UPSTREAM, else local.ohos.ps1 / .env, else
        ../totk-android). The same run gives TOTK_UPSTREAM_UNPINNED as the backend's scripts see it (self._unpinned):
        this app's environment, else the backend's local files, as prepare_guest.py and upstream.require() take it."""
        if self._upstream is None:
            code = ("import os, sys; sys.path.insert(0, 'native-harmonyos'); import ohos_env; "
                    "print(os.environ.get('TOTK_UPSTREAM_UNPINNED') or '-'); print(ohos_env.UPSTREAM)")
            r = proc.run([config.python(), "-I", "-c", code], cwd=str(self.root), capture_output=True, text=True,
                         errors="replace")
            lines = r.stdout.strip().splitlines() if r.returncode == 0 else []
            self._upstream = Path(lines[-1]) if lines else self.root.parent / "totk-android"
            self._unpinned = len(lines) > 1 and lines[-2].strip() == "1"
        return self._upstream

    def upstream_ready(self) -> bool:
        """The totk-android checkout at its pin (native-harmonyos/upstream.py --check: 0 there, 1 missing, 2 elsewhere),
        before the first step: the steps mount it into containers and build_ohos.py compiles from it. Nothing is fetched
        here (the 环境 page's fix clones it; the package brings its own copy). TOTK_UPSTREAM_UNPINNED=1 (this app's
        environment, else the backend's local.ohos.ps1 / .env, as prepare_guest.py and upstream.require() take it) goes
        on at another commit, as the port's scripts do."""
        up = self.upstream()
        try:
            r = proc.run([config.python(), "-I", str(self.root / "native-harmonyos" / "upstream.py"), "--check"],
                         cwd=str(self.root), env=self._env(), capture_output=True, text=True, errors="replace", timeout=120)
            code, said = r.returncode, (r.stdout or r.stderr or "").strip().splitlines()
        except (OSError, subprocess.SubprocessError) as e:
            code, said = -1, [str(e)]
        if code == 0:
            return True
        if code == 2 and getattr(self, "_unpinned", False) and (up / "games" / "totk").is_dir():
            self.ev.say(f"    参考仓库 {up} 不在固定提交，按 TOTK_UPSTREAM_UNPINNED=1 继续")
            return True
        try:
            packaged = (up / UPSTREAM_MARKER).is_file() and not (up / ".git").exists() or \
                (config.FROZEN and up.resolve().parent == (config.APP_DIR / "backends").resolve())
        except OSError:
            packaged = False
        if packaged:
            self.ev.red(f"软件包自带的参考仓库 totk-android 缺失或不完整（{up}）：把你手上那份完整的 NX2Harmony 重新完整解压到原处再构建")
        elif code == 1:
            self.ev.red(f"还没有参考仓库 totk-android（{up}）：在“环境”页修复参考仓库后再构建")
        elif code == 2:
            self.ev.red(f"参考仓库 totk-android 不在固定提交（{up}）：在“环境”页修复参考仓库，"
                        "或运行 python native-harmonyos/upstream.py --update")
        else:
            self.ev.red(f"检查参考仓库 totk-android 失败（退出码 {code}）：在“环境”页修复参考仓库后再构建")
        for line in said[-2:]:
            self.ev.say("    " + line)
        return False

    def _docker(self, name: str, image: str, mounts: list, cmd: list, env=None) -> bool:
        """common.py's, refusing a read-only mount of a missing folder (the reference, the SDK's sysroot): docker would
        make it an empty one and the step would fail later on what looks like a broken checkout."""
        gone = [(Path(host), guest) for host, guest, ro in mounts if ro and not Path(host).is_dir()]
        if gone:
            self.ev.red(missing_mount(*gone[0]))
            return False
        return BaseBackend._docker(self, name, image, mounts, cmd, env)   # by name: oot_v131072 borrows this one

    # ---------- steps ----------
    def prepare(self, inputs: dict, keys, library: list) -> bool:
        if not self.upstream_ready():
            return False
        d = self._done
        py = [config.python(), "-I"]
        R = "prepared/za/rewrite/rewritten"
        exefs = f"prepared/nup/{PART}/exefs"
        steps = [
            ("解开 NUP（2.0.2 更新）", lambda: d()["unpack"] or not self.needs_inputs(), lambda: self._unpack(inputs.get("nup"))),
            ("改写游戏代码", lambda: d()["rewrite"], self._rewrite),
            ("运行时表", lambda: d()["tables"], lambda: self._run("tables", py + [
                "native-harmonyos/za/prepare_runtime_za.py", "--version", "za202", "--rewritten", R,
                "--report", f"{R}/phase12-validation.json", "--out", "prepared/za/game-data", "--exefs", exefs])[0] == 0),
            ("打包 RomFS", lambda: d()["romfs"], lambda: self._run("romfs", py + [
                "native-harmonyos/za/pack_romfs_za.py", f"prepared/nup/{PART}", "prepared/za/romfs"])[0] == 0),
            ("系统字体", lambda: d()["fonts"], lambda: self._fonts(keys, library)),
            ("着色器翻译器", lambda: d()["xlate"], self._xlate),
            ("客体库（含宽屏相机补丁）", lambda: d()["embed"], self._guest),
            ("游戏数据", lambda: d()["gamedata"], lambda: self._run("gamedata", py + [
                "native-harmonyos/game_data_ohos.py", "prepared/za/romfs", "prepared/za/shared-fonts", "prepared/za/game-data"])[0] == 0),
            ("第三方静态库", lambda: d()["deps"], self._deps),
        ]
        return self._steps(steps)

    def _unpack(self, nup) -> bool:
        if not nup:
            self.ev.red("要解开的 NUP 没找到（需要含 2.0.2 更新的 NUP）")
            return False
        if proc.run([config.python(), "-c", "import zstandard"], capture_output=True).returncode:
            self.ev.say("    安装 Python 模块 zstandard（解 NUP 用，只需一次）")
            if self._run("pip", [config.python(), "-m", "pip", "install", "--user", "zstandard"], env=self._pip_env())[0]:
                return False
        # not -I: zstandard may sit in the user's site-packages
        return self._run("unpack", [config.python(), "tools/nup_extract.py", str(nup), "--out", "prepared/nup",
                                    "--parts", "update"])[0] == 0

    def _xlate(self) -> bool:
        """build_xlate.sh in the .NET SDK image (pulled from the fastest registry when missing), with the Ubuntu archive
        and NuGet feed that answer fastest; a second set when the first fails (mirrors.xlate_envs)."""
        mounts = [(self.upstream(), "/android", True), (self.xlate.parent, "/out", False), (self.root, "/repo", False)]
        for i, env in enumerate(mirrors.xlate_envs(self.ev.say)):
            if i:
                self.ev.say("    换下一组下载源再试一次")
            if self._docker("xlate" if not i else f"xlate-{i + 1}", DOTNET, mounts,
                            ["sh", "/repo/native-harmonyos/xlate/build_xlate.sh"], env=env):
                return True
        return False

    def _rewrite(self) -> bool:
        if not self._ensure_image(D2R, self.root / "tools" / "docker" / "d2r.Dockerfile"):
            return False
        return self._docker("rewrite", D2R, [(self.root, "/repo", False), (self.upstream(), "/android", True)],
                            ["python3", "/repo/native-harmonyos/za/rewrite_za.py", "--android", "/android",
                             "--exefs", f"/repo/prepared/nup/{PART}/exefs", "--out", "/repo/prepared/za/rewrite"])

    def _guest(self) -> bool:
        """prepare_guest.py za202: one nativegames-tools:1 run (the export), then gen_embed_ohos.py with the same Python;
        no shell on the PC."""
        if not self._ensure_image(D2R, self.root / "tools" / "docker" / "d2r.Dockerfile"):
            return False
        # the image every other step here uses, not a TOTK_D2R_IMAGE of the backend's local.ohos.env (ohos_env.py)
        return self._run("guest", [config.python(), "-I", "native-harmonyos/prepare_guest.py", "za202"],
                         env=dict(self._env(), TOTK_D2R_IMAGE=D2R))[0] == 0

    def _fonts(self, keys, library) -> bool:
        from .. import fonts
        src, msg = fonts.extract(library, keys)
        if not src:
            self.ev.red(msg)
            return False
        self.fonts.mkdir(parents=True, exist_ok=True)
        for f in src.iterdir():
            if f.is_file():
                shutil.copy2(f, self.fonts / f.name)
        self.ev.say(f"    {msg}")
        return True

    def _deps(self) -> bool:
        """libopus and libvpx: the package's prebuilt ones, else the same pinned builds as the TOTK backend's, copied when it
        has them, else built here."""
        self._prebuilt()
        totk = config.backend_root("totk_v143") / "artifacts"
        for mine, rel in ((self.opus, "native-opus"), (self.vpx, "native-vpx")):
            if mine.is_file():
                continue
            theirs = totk / rel
            if theirs.is_dir() and (theirs / mine.relative_to(self.root / "artifacts" / rel)).is_file():
                shutil.copytree(theirs, self.root / "artifacts" / rel, dirs_exist_ok=True)
                continue
            if rel == "native-opus":
                pwsh = os.environ.get("TOTK_PWSH") or shutil.which("pwsh")
                if not pwsh:
                    self.ev.red("编译 libopus 需要 PowerShell 7（在“环境”页一键安装）")
                    return False
                from ..device import DEVECO
                if self._run("opus", [pwsh, "-NoProfile", "-File", "native-harmonyos/build_opus_ohos.ps1", "-DevEcoRoot", str(DEVECO)])[0]:
                    return False
            else:
                from ..doctor import sdk_dir
                if not self._ensure_image(D2R, self.root / "tools" / "docker" / "d2r.Dockerfile"):
                    return False
                if not self._docker("vpx", D2R, [(self.root, "/repo", False), (sdk_dir() / "sysroot", "/ohos/sysroot", True)],
                                    ["sh", "/repo/native-harmonyos/build_vpx_ohos.sh", "/repo/artifacts/native-vpx"]):
                    return False
        return True

    def libs(self) -> bool:
        if not self._embed_here("prepared/za"):
            return False
        return self._run("libs", [config.python(), "-I", "native-harmonyos/build_ohos.py", "--gpu", "vk", "--version", "za202",
                                  "--guest", "prepared/za/embed"])[0] == 0

    # ---------- push ----------
    def push_list(self) -> list:
        """As za/push_za.ps1 -GameData -Fonts -RomFS -Seed sends them (the seed once it exists)."""
        g, r = self.game_data, self.romfs
        out = [(g / f, f"game-za/{f}") for f in ("runtime-tables.bin", "romfs-config.txt", "shared-fonts.txt")]
        out += [(f, f"game-za/fonts/{f.name}") for f in sorted(self.fonts.glob("*.bfttf"))]
        out += [(r / f, f"game-za/{f}") for f in ("runtime-index.bin", "romfs.bin")]
        if self._done()["seed"]:
            out += [(self.seed / "sb-align.txt", "android/shaders/za202/sb-align.txt"),
                    (self.seed_build / "seed.vkpack", "android/shaders/za202/seed.vkpack")]
        return out

    def extra_stages(self) -> list:
        if self._done()["seed"]:
            return []
        return [("着色器种子", "首次在手机上解包一次着色器，再在电脑上翻译（约 1 小时）", self.seed_from_phone)]

    # ---------- the shader seed from the phone ----------
    def _dev_env(self, *lines: str) -> bool:
        """Variables for the app's next start: a new files/android/env-dev-<time>.txt (za/devenv.ps1)."""
        name = f"env-dev-{time.strftime('%Y%m%d%H%M%S')}{int(time.time() * 1000) % 1000:03d}.txt"
        with tempfile.TemporaryDirectory() as t:
            f = Path(t) / name
            f.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return self.send(f, f"android/{name}")

    def _alive(self) -> bool:
        return bool(self.hdc.shell(f"pidof {self.bundle}").strip())

    def seed_from_phone(self) -> bool:
        b = self.bundle
        # translating needs SPIRV-Cross (built once, as build_seed.sh does) and the seed folder with its alignment
        sc = self.seed_build / "spirv-cross" / "spirv-cross"
        if not sc.is_file():
            self.ev.step("编译 SPIRV-Cross（只需一次）")
            snippet = ("set -e; SC=/repo/prepared/za/seed-build/spirv-cross; mkdir -p $SC/obj; rm -rf $SC.src; "
                       f"git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git $SC.src; git -C $SC.src checkout -q {SPIRV_CROSS}; "
                       "for f in main spirv_cfg spirv_cpp spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_hlsl "
                       "spirv_msl spirv_parser spirv_reflect; do echo $f; done | xargs -P $(nproc) -I{} g++ -O3 -DNDEBUG -std=c++11 "
                       "-Wno-deprecated-declarations -c $SC.src/{}.cpp -o $SC/obj/{}.o; g++ $SC/obj/*.o -o $SC/spirv-cross")
            if not self._docker("spirv-cross", "python:3.14", [(self.root, "/repo", False)], ["sh", "-c", snippet]):
                return False
        self.seed.mkdir(parents=True, exist_ok=True)
        if not (self.seed / "sb-align.txt").is_file():
            (self.seed / "sb-align.txt").write_text("256\n", encoding="utf-8")   # the HarmonyOS translator's storage-buffer alignment
        # 1. the dump, on the phone (the app exits when it is written)
        self.ev.step("在手机上解包着色器（约 1 分钟）")
        if not self._dev_env(f"ZA_SHADER_DUMP={DUMP_DIR}", "ZA_SHADER_DUMP_REMOVE="):
            return False
        self.hdc.shell(f"aa force-stop {b}")
        # the "start" command starts the runtime, which dumps and exits; a plain start only shows the launcher page
        self.hdc.shell(f"aa start -b {b} -a EntryAbility --ps command start")
        remote = f"{self.shell_files}/android/za-dump/bnsh.pack"
        t0 = time.time()
        time.sleep(10)
        while self._alive() and time.time() - t0 < 900:
            self.ev.progress("解包着色器", 0, None, note=f"{(self.hdc.size_of(remote) or 0) / 2**30:.1f} GB")
            time.sleep(5)
        self._dev_env("ZA_SHADER_DUMP=")   # the next start plays again
        if self._alive():
            self.hdc.shell(f"aa force-stop {b}")
            self.ev.red("手机上的着色器解包 15 分钟没结束")
            return False
        if not self.hdc.size_of(remote):
            self.ev.red("手机上没有生成 bnsh.pack（看手机日志里的 [za-dump]）")
            return False
        # 2. to the PC, translated, packed and sent (za_seed.py), with the alignment file beside it
        self.ev.step("拉回电脑、翻译并发送种子（约 1 小时）")
        if not self._ensure_base("python:3.14"):   # za_seed.py translates in python:3.14 containers
            return False
        if self._run("seed", [config.python(), "-I", "native-harmonyos/za/za_seed.py", "--pull", "--send"])[0]:
            return False
        if not self.send(self.seed / "sb-align.txt", "android/shaders/za202/sb-align.txt"):
            return False
        # 3. the 7.5 GB dump leaves the phone
        self.ev.step("删除手机上的解包文件")
        self._dev_env(f"ZA_SHADER_DUMP_REMOVE={DUMP_DIR}")
        self.hdc.shell(f"aa start -b {b} -a EntryAbility --ps command start")
        time.sleep(10)
        self.hdc.shell(f"aa force-stop {b}")
        self._dev_env("ZA_SHADER_DUMP_REMOVE=")
        return self._done()["seed"]
