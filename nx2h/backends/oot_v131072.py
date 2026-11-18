"""The Legend of Zelda: Ocarina of Time, Switch 2 (0400486024DEC000, update v131072, SDK 22.2.8, NVN2): the OOT port's
scripts (the OOT backend project, its docs/DESIGN-OOT.md), in the order they were run by hand.
Its status (已验证 / 优化中) is games.tsv's; either builds in one click.

  inputs   the game's NUP holding the v131072 update (the update part is the whole game); prod.keys only for the shared
           fonts (the NUP has none)
  unpack   tools/nup_extract.py <nup> --out prepared/nup --parts update   (every file's SHA-256 checked; zstandard), then
           the ExeFS checked against the port's assets/expected-oot-v131072.json (build IDs, SHA-256)
  plain    native-harmonyos/oot/nso_unzbic.py: main (ZBIC) and sdk / subsdk0 (LZ4) stored uncompressed in
           prepared/oot/exefs-plain (a Python with the zbic and lz4 packages), main.npdm and rtld beside; plain_sha256
  rewrite  native-harmonyos/oot/rewrite_oot.py in nativegames-tools:1: main, sdk and subsdk0 (--plain exefs-plain)
  tables   native-harmonyos/oot/prepare_runtime_oot.py in nativegames-tools:1 -> prepared/oot/game-data/runtime-tables.bin
  romfs    native-harmonyos/oot/pack_romfs_za.py -> prepared/oot/romfs/{romfs.bin, index.json} (zstd / ZsDic files as stored)
  fonts    the console's shared fonts (nx2h/fonts.py: a card image or firmware with prod.keys) -> prepared/oot/shared-fonts
  guest    native-harmonyos/prepare_guest.py oot131072 (nativegames-tools:1): main's text exported with the game
           patches (overlay/runtime/game_patches_oot.c: the real-time frame step, dynamic resolution) into prepared/oot/x0,
           then gen_embed_ohos.py -> prepared/oot/embed. Done again when the patch sources are newer than the export, or
           the export newer than the embed
  gamedata native-harmonyos/game_data_ohos.py -> romfs-config.txt, shared-fonts.txt (and runtime-index.bin)
  xlate    the Ampere translator: native-harmonyos/xlate/prepare_xlate_ns2.py (totk-android's totk_xlate on nxnative's
           Ryujinx; its patches applied in Python, xlate/unidiff.py), published in dotnet/sdk:10.0-noble-aot ->
           artifacts/xlate-ohos-ns2/totk_xlate
  seed     the shader seed, translated on the PC (the RomFS is zstd with dictionaries: no dump on the phone)
             1 split      nxnative's dm/gfx/tools/shader_split.py (DM_GAME; zbic, lz4, zstd) -> prepared/oot/shader-split;
                          the zstd program too (59,047 of the RomFS's zstd files name no dictionary: shader_split.py
                          runs `zstd -d` for those), and a run that could not read some files fails
             2 SPIRV-Cross  the Z-A backend's (same pin), else built in python:3.14 as za_202.py does
             3 translate  native-harmonyos/oot/oot_seed.py (python:3.14 containers) -> seed-build/shaders/oot131072
             4 validate   tools/spirv_val_seed.sh (nativegames-tools:1): invalid modules set aside; more than 5% fails
             5 SSA        tools/spirv_opt_seed.sh (nativegames-tools:1): Maleoon's compiler is slow on unpromoted variables
             6 variants   oot_seed.py --variants oot/oot131072-variants.tsv --smem (the OoT port's own pass): the 146
                          variants filled from the phone in testing (hashes and answers only, no game data), translated,
                          validated and promoted to SSA, and smem_private.py's rewrite of 56 compute modules
             7 mesh       native-harmonyos/oot/oot_mesh.py: task / mesh shaders as compute kernels -> prepared/oot/mesh-build
             8 wrap       oot_mesh.py --wrap --install: the kernels wrapped for the replay, copied into the seed folder
             9 pack       totk-android's pack_shader_seed.py in python:3.14 -> seed-build/seed.vkpack
           Steps 4-8 need spirv-val, spirv-opt and glslangValidator in nativegames-tools:1. The adapted backend's
           d2r.Dockerfile installs them; an image built earlier from the port's first Dockerfile, without them, is
           extended under the same tag (_seed_tools). The adapted backend's .sh files are LF (the package makes every
           .sh LF too); a checkout with CRLF ones still works: they run as LF copies in the container (_lf).
           With step 6 a seed built here from nothing matches the tested oot-a134 seed (the OoT port checked it
           2026-11-16: all 146 variants byte for byte; it lacks only 182 fills the runtime never picks and has newer
           builds of 4 old modules).
  deps     libopus / libvpx and libdav1d (the AV1 movies): another backend's when it has them (same pinned sources), else
           built
  libs     native-harmonyos/build_ohos.py --gpu vk --version oot131072 --guest prepared/oot/embed
  hap, install, push   common.py; push as oot/push_oot.ps1 -GameData -Fonts -RomFS -Seed
Before the first step the totk-android checkout is checked (za_202.upstream_ready), and a read-only mount of a missing
folder is refused (za_202._docker). Every step whose output is there is skipped, so a stopped build continues where it
stopped. Seed steps 3-8, whose own files do not show that they finished, leave a note in prepared/oot/nx2h-steps when
their script did; a packed seed counts for every seed step."""
from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import time
from pathlib import Path

from .. import config
from .. import mirrors
from .. import proc
from . import za_202
from .common import BaseBackend, docker_path

PART = "0400486024DEC800-v131072-update"     # tools/nup_extract.py: <title>-v<version>-<type>
D2R = za_202.D2R                             # nativegames-tools:1, from the port's tools/docker/d2r.Dockerfile
DOTNET = za_202.DOTNET
VER = "oot131072"   # the port's name for this version: build_ohos.py --version, prepare_guest.py, the seed folder
MODULES = ("main", "sdk", "subsdk0")
SEED_STEPS = ("seed_translate", "seed_validate", "seed_ssa", "seed_variants", "seed_mesh", "seed_mesh_wrap")   # the noted ones, in order
# Python packages, as (import names, any one of them | ..., pip name)
ZSTANDARD = [("zstandard", "zstandard")]
ZBIC = [("zbic", "zbic"), ("lz4.block", "lz4")]                 # a Switch 2 module's ZBIC (main) and LZ4 segments
ZSTD = [("compression.zstd|backports.zstd", "backports.zstd")]   # Python 3.14's zstd, or its backport
# seed steps 4-7 (and oot_mesh.py's containers) run these in nativegames-tools:1; the port's d2r.Dockerfile has none
TOOLS = ("spirv-val", "spirv-opt", "glslangValidator")
TOOLS_DOCKERFILE = (
    b"# NX2Harmony: nativegames-tools:1 plus the SPIR-V tools Ocarina of Time's shader seed steps run (spirv-val,\n"
    b"# spirv-opt, glslangValidator), under the same tag: oot_mesh.py and fill_shaders_ohos.py name it.\n"
    b"FROM nativegames-tools:1\n"
    b"RUN apt-get update && apt-get install -y --no-install-recommends spirv-tools glslang-tools"
    b" && rm -rf /var/lib/apt/lists/*\n")
INVALID_MAX = 0.05   # spirv-val may set aside this share of the seed (the hand run: 75 of 42,328)


def jobs(n: int) -> int:
    """The hand runs' number of parallel jobs, at most this PC's processors."""
    return max(1, min(n, os.cpu_count() or n))


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


class Backend(BaseBackend):
    key = "oot_v131072"
    title_id = "0400486024DEC000"
    name = "塞尔达传说 时之笛"
    version = "v131072"
    update = "v131072"
    slug = "oot"
    log_name = "oot"
    disk_gb = 70   # prepared/ after a full build from the NUP: 66 GB (unpacked update 25, RomFS image 26, shader work)

    upstream = za_202.Backend.upstream   # the totk-android checkout (ohos_env.UPSTREAM: TOTK_UPSTREAM or local.ohos.env)
    upstream_ready = za_202.Backend.upstream_ready   # upstream.py --check, before the first step
    _docker = za_202.Backend._docker     # common.py's, refusing a read-only mount of a missing folder
    _fonts = za_202.Backend._fonts       # nx2h/fonts.py's shared fonts into self.fonts

    def __init__(self, ev, hdc):
        super().__init__(ev, hdc)
        p = self.prep
        self.part = p / "nup" / PART
        self.exefs = self.part / "exefs"
        self.oot = p / "oot"
        self.plain = self.oot / "exefs-plain"
        self.rewritten = self.oot / "rewrite" / "rewritten"
        self.game_data = self.oot / "game-data"
        self.romfs = self.oot / "romfs"
        self.fonts = self.oot / "shared-fonts"
        self.embed = self.oot / "embed"
        self.split = self.oot / "shader-split"
        self.seed_build = self.oot / "seed-build"
        self.seed = self.seed_build / "shaders" / VER
        self.spirv_cross = self.seed_build / "spirv-cross" / "spirv-cross"
        self.mesh = self.oot / "mesh-build"
        self.notes = self.oot / "nx2h-steps"
        art = self.root / "artifacts"
        self.xlate_src = art / "xlate-src-ns2"
        self.xlate = art / "xlate-ohos-ns2" / "totk_xlate"
        self.opus = art / "native-opus/build-ohos/opus/libopus.a"
        self.vpx = art / "native-vpx/ohos/lib/libvpx.a"
        self.dav1d = art / "native-dav1d/ohos/lib/libdav1d.a"
        self._upstream = None
        self._nxnative = None
        self._expected = None
        self._tools_ok = False

    # ---------- what is there ----------
    def _done(self) -> dict:
        """Each step's product, from file checks only (the library scan asks needs_inputs() every time it looks): the
        ExeFS hashes (_modules_ok, _plain_ok) are checked in prepare() only."""
        g, s = self.game_data, self.seed_build / "shaders"
        packed = (self.seed_build / "seed.vkpack").is_file() and (self.seed / "sb-align.txt").is_file()
        return {
            # .part.json: nup_extract.py writes it after every file is checked (.digests is saved every few seconds)
            "unpack": (self.part / ".part.json").is_file() and (self.exefs / "main").is_file(),
            "rewrite": self._rewrite_ok(),
            "tables": (g / "runtime-tables.bin").is_file(),   # written to .partial, then renamed (prepare_runtime_oot.py)
            "romfs": (self.romfs / "romfs.bin").is_file() and (self.romfs / "index.json").is_file(),   # index.json last
            "fonts": (self.fonts / "manifest.json").is_file(),
            "embed": self._guest_current(),
            "gamedata": all((g / f).is_file() for f in ("romfs-config.txt", "shared-fonts.txt"))
                        and (self.romfs / "runtime-index.bin").is_file(),   # pack_romfs_za.py removes a stale one
            "xlate": self.xlate.is_file() and self.xlate.with_name("totk_xlate.sha256").is_file(),   # the hash last
            "seed_split": packed or ((self.split / "pkg").is_dir()
                                     and all((self.split / f).is_file() for f in ("shader_map.tsv", "facts.tsv", "pairs.tsv"))),
            "seed_spirvcross": packed or self.spirv_cross.is_file(),
            "seed_translate": packed or (self._noted("seed_translate") and (self.seed / "sb-align.txt").is_file()),
            "seed_validate": packed or (self._noted("seed_validate") and (s / "spirv-val.txt").is_file()),
            "seed_ssa": packed or (self._noted("seed_ssa") and (s / "spirv-opt.txt").is_file()),
            "seed_variants": packed or self._noted("seed_variants"),   # oot_seed.py --variants --smem (its own variants-*.txt)
            "seed_mesh": packed or (self._noted("seed_mesh") and (self.mesh / "mesh-info.json").is_file()),
            "seed_mesh_wrap": packed or (self._noted("seed_mesh_wrap") and (self.mesh / "wrapped" / "wrapped.json").is_file()),
            "seed": packed,   # pack_shader_seed.py writes a temporary file and renames it
            "deps": self.opus.is_file() and self.vpx.is_file(),
            "dav1d": self.dav1d.is_file(),
        }

    def _expected_ids(self) -> dict:
        """assets/expected-oot-v131072.json: build_id, exefs_sha256, plain_sha256, rewritten_sha256."""
        if self._expected is None:
            try:
                self._expected = json.loads((self.root / "assets" / "expected-oot-v131072.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                self._expected = {}
        return self._expected

    def _modules_ok(self) -> bool:
        """main / sdk / subsdk0 are the ones this port is made for: the expected file's NSO build IDs and the SHA-256 of
        every ExeFS file (as Z-A's expected-za-2.0.2.json)."""
        e = self._expected_ids()
        if not e.get("build_id") or not e.get("exefs_sha256"):
            return False
        for name, bid in e["build_id"].items():
            f = self.exefs / name
            if not f.is_file():
                return False
            with open(f, "rb") as fh:
                head = fh.read(0x50)
            if head[:4] != b"NSO0" or head[0x40:0x50].hex() != bid:
                return False
        return all((self.exefs / n).is_file() and sha256(self.exefs / n) == h for n, h in e["exefs_sha256"].items())

    def _plain_ok(self) -> bool:
        """exefs-plain holds nso_unzbic.py's main / sdk / subsdk0 (plain_sha256, which rewrite_oot.py asserts too) and the
        ExeFS's main.npdm and rtld."""
        want = self._expected_ids().get("plain_sha256") or {}
        if not want or not all((self.plain / f).is_file() for f in ("main.npdm", "rtld")):
            return False
        return all((self.plain / m).is_file() and sha256(self.plain / m) == want.get(m) for m in MODULES)

    def _rewrite_ok(self) -> bool:
        """rewrite_oot.py's phase12-validation.json (written last) passed, on this game's ExeFS modules."""
        try:
            r = json.loads((self.rewritten / "phase12-validation.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return False
        want = self._expected_ids().get("exefs_sha256") or {}
        got = r.get("module_file_sha256") or {}
        return r.get("status") == "pass" and bool(want) and all(got.get(f"exefs/{m}") == want.get(m) for m in MODULES)

    def _guest_current(self) -> bool:
        """The guest sources come from main's exported text (the frame step, dynamic resolution), and that from today's
        patches and rewrite. prepare_guest.py compiles game_patches_oot.c with export_text.c (CODE_AT_BUILD 0)."""
        x0, mod = self.oot / "x0" / "main.x0.bin", self.embed / "guest_modules.c"
        if not (x0.is_file() and mod.is_file()):
            return False
        nh = self.root / "native-harmonyos"
        sources = [nh / "overlay/runtime/game_patches_oot.c", nh / "tools/export_text.c", self.rewritten / "phase12-validation.json"]
        newest = max((p.stat().st_mtime for p in sources if p.is_file()), default=0)
        return x0.stat().st_mtime >= newest and mod.stat().st_mtime >= x0.stat().st_mtime

    def _noted(self, step: str) -> bool:
        return (self.notes / f"{step}.txt").is_file()

    def needs_inputs(self) -> bool:
        d = self._done()   # what is made from the unpacked game: the rewrite, the tables, the RomFS and the shader list
        return not (d["rewrite"] and d["tables"] and d["romfs"] and d["seed_split"])

    def needs_keys(self) -> bool:
        from .. import fonts
        return not self._done()["fonts"] and not fonts.ready()

    def pick_inputs(self, items) -> tuple:
        nups = [it.path for it in items if it.kind == "nup" and any(r == "update" and v == self.update for _, v, r in it.titles)]
        problems = [] if nups else [f"缺塞尔达传说 时之笛的 NUP（要含 {self.update} 更新）"]
        return {"nup": nups[0] if nups else None}, problems

    # ---------- what the steps need ----------
    def nxnative(self):
        """seedds/nxnative (MIT): shader_split.py, dmsh_meshstage.py and the Ampere Ryujinx. NXNATIVE, nx2h.local.json
        "nxnative", else beside this app's checkout or the backend; None: not found."""
        if not self._nxnative:
            places = [os.environ.get("NXNATIVE"), config.local().get("nxnative")]
            places = [Path(p) for p in places if p] + [config.REPO.parent / "nxnative", self.root.parent / "nxnative"]
            self._nxnative = next((p for p in places if (p / "dm/gfx/tools/shader_split.py").is_file()), None)
        return self._nxnative

    def nxnative_how(self) -> str:
        return (f"取 nxnative（https://github.com/seedds/nxnative，用 backends/SOURCES.tsv 固定的提交）："
                f"放到 {self.root.parent / 'nxnative'}，或在 {config.LOCAL} 写 \"nxnative\": \"<它的位置>\"")

    def needs_nxnative(self) -> bool:
        """A step still to run uses nxnative: the translator (its Ryujinx), the shader list and the mesh shaders (doctor.py
        asks before the 25 GB unpack, not at step 10)."""
        d = self._done()
        return not (d["xlate"] and d["seed"])

    def _need_nxnative(self):
        nx = self.nxnative()
        if not nx:
            self.ev.red("缺 nxnative 仓库（着色器清单、网格着色器和 Ampere 翻译器要用）：" + self.nxnative_how())
        return nx

    def _oot_env(self) -> dict:
        """_env() plus what the port's scripts read: NXNATIVE; first on PATH the Python of this build (for a script that
        starts `python` by name; prepare_guest.py and prepare_xlate_ns2.py need no shell or patch program). Without the
        hand runs' trial switches (OOT_SEED_BUILD, OOT_XLATE: another seed folder or translator; TOTK_D2R_IMAGE: another
        image; set here to the image every step of this backend uses, which the backend's local.ohos.env would
        otherwise give prepare_guest.py): the done checks, the containers' mounts and the pack use this backend's own
        places. The zstd program's folder goes on PATH too when it is not there yet (installed by winget in this
        session: its Links folder joins PATH only for programs started later)."""
        env = self._env()
        for k in ("OOT_SEED_BUILD", "OOT_XLATE"):
            env.pop(k, None)
        env["TOTK_D2R_IMAGE"] = D2R
        nx = self.nxnative()
        if nx:
            env["NXNATIVE"] = str(nx)
        z = self.zstd()
        if z and not shutil.which("zstd", path=env.get("PATH")):
            env["PATH"] = str(Path(z).parent) + os.pathsep + env.get("PATH", "")
        if config.python():
            env["PATH"] = str(Path(config.python()).parent) + os.pathsep + env.get("PATH", "")
        return env

    @staticmethod
    def zstd() -> str:
        """The zstd program (shader_split.py runs `zstd -d` for the files that name no dictionary): on PATH, else where
        winget puts it (winget install Meta.Zstandard: a link in WinGet\\Links, the program below WinGet\\Packages);
        '' when there is none."""
        w = shutil.which("zstd")
        if w:
            return w
        bases = [Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local") / "Microsoft" / "WinGet",
                 Path(os.environ.get("ProgramFiles") or r"C:\Program Files") / "WinGet"]
        for b in bases:
            if (b / "Links" / "zstd.exe").is_file():
                return str(b / "Links" / "zstd.exe")
        for b in bases:
            for f in sorted(b.glob("Packages/Meta.Zstandard*/*/zstd.exe")) + sorted(b.glob("Packages/Meta.Zstandard*/zstd.exe")):
                if f.is_file():
                    return str(f)
        return ""

    def needs_zstd(self) -> bool:
        """Seed step 1 (the shader list) is still to run: doctor.py asks before the 25 GB unpack."""
        return not self._done()["seed_split"]

    def _pythons(self) -> list:
        """config.python(), then the other Pythons of the Python launcher (py -0p)."""
        out = [config.python()] if config.python() else []
        try:
            r = proc.run(["py", "-0p"], capture_output=True, text=True, errors="replace", timeout=30)
        except (OSError, subprocess.SubprocessError):
            return out
        for line in r.stdout.splitlines():
            m = re.search(r"[A-Za-z]:\\.*\.exe", line)
            if m and all(os.path.normcase(m.group(0)) != os.path.normcase(p) for p in out):
                out.append(m.group(0))
        return out

    def _py_with(self, need: list, why: str) -> str:
        """A Python that imports need ([(import names, any one | ..., pip name)]): the first one that has them, else the
        newest one that takes them from pip (--user, once). Run without -I: the packages sit in the user's site-packages."""
        probe = ("import importlib, sys\n"
                 "def ok(names):\n"
                 "    for n in names.split('|'):\n"
                 "        try:\n"
                 "            importlib.import_module(n)\n"
                 "            return True\n"
                 "        except Exception:\n"
                 "            pass\n"
                 "    return False\n"
                 f"print('%d.%d' % sys.version_info[:2], *[p for n, p in {need!r} if not ok(n)])\n")
        lacking = []
        for py in self._pythons():
            try:
                r = proc.run([py, "-c", probe], capture_output=True, text=True, errors="replace", timeout=60)
            except (OSError, subprocess.SubprocessError):
                continue
            out = r.stdout.split()
            if r.returncode or not out:
                continue
            if len(out) == 1:
                return py
            lacking.append((tuple(int(x) for x in out[0].split(".")), py, out[1:]))
        for _, py, pips in sorted(lacking, reverse=True):   # the newest first: the packages' wheels come for new Pythons first
            self.ev.say(f"    安装 Python 模块 {' '.join(pips)}（{why}用，只需一次）")
            if self._run("pip", [py, "-m", "pip", "install", "--user"] + pips, env=self._pip_env())[0] == 0:
                r = proc.run([py, "-c", probe], capture_output=True, text=True, errors="replace", timeout=60)
                if r.returncode == 0 and len(r.stdout.split()) == 1:
                    return py
        self.ev.red(f"{why}要用装有 {'、'.join(p for _, p in need)} 的 Python，"
                    "本机的 Python 都装不上（可以装 Python 3.12 再试：winget install Python.Python.3.12）")
        return ""

    def _d2r(self) -> bool:
        return self._ensure_image(D2R, self.root / "tools" / "docker" / "d2r.Dockerfile")

    def _has_tools(self) -> bool:
        try:
            r = proc.run(["docker", "run", "--rm", D2R, "sh", "-c", " && ".join(f"command -v {t}" for t in TOOLS)],
                         capture_output=True, text=True, errors="replace", timeout=300)
        except (OSError, subprocess.SubprocessError):
            return False
        return r.returncode == 0

    def _seed_tools(self) -> bool:
        """nativegames-tools:1 with spirv-val, spirv-opt and glslangValidator. Without them spirv_val_seed.sh takes every
        module for invalid (`spirv-val: not found`), sets the whole seed aside and still exits 0. The adapted backend's
        d2r.Dockerfile installs them; an image built before that (from the port's first Dockerfile) is extended under the
        same tag (the other backends' steps keep what they had)."""
        if self._tools_ok:
            return True
        if not self._d2r():
            return False
        if not self._has_tools():
            self.ev.say(f"    给 {D2R} 加上 spirv-tools 和 glslang（时之笛的着色器步骤要用，只需一次）")
            d = self.prep / "nx2h-stage" / "spirv-tools-image"
            d.mkdir(parents=True, exist_ok=True)
            (d / "Dockerfile").write_bytes(TOOLS_DOCKERFILE)   # LF (Python 3.9's write_text has no newline=)
            if self._run("image-spirv-tools", ["docker", "build", "-t", D2R, d])[0]:
                return False
            if not self._has_tools():
                self.ev.red(f"{D2R} 里还是没有 {' / '.join(TOOLS)}")
                return False
        self._tools_ok = True
        return True

    @staticmethod
    def _lf(script: str, args: list, beside: tuple = ()) -> list:
        """`sh -c` running an LF copy of one of the port's shell scripts in a container: a checkout may hold them CRLF
        (the hand port's are), which dash (sh in the Debian images) reads as part of each command (set -eu\\r: Illegal
        option); an LF one is copied as it is. The copy sits in /tmp/nx2h with
        links to what the script finds beside itself (HERE=$(dirname "$0")); a script that calls itself again
        (spirv_opt_seed.sh: sh "$0" --one) so runs the copy."""
        here, name = script.rsplit("/", 1)
        cmd = ["set -e", "mkdir -p /tmp/nx2h", f"tr -d '\\r' < {here}/{name} > /tmp/nx2h/{name}"]
        cmd += [f"ln -s {here}/{b} /tmp/nx2h/{b}" for b in beside]
        cmd.append(" ".join(["sh", f"/tmp/nx2h/{name}"] + [shlex.quote(str(a)) for a in args]))
        return ["sh", "-c", "; ".join(cmd)]

    def _docker_out(self, name: str, image: str, mounts: list, cmd: list) -> tuple:
        """common.py's _docker with the output lines: (exit code, lines)."""
        args = ["docker", "run", "--rm"]
        for host, guest, ro in mounts:
            args += ["-v", f"{docker_path(host)}:{guest}" + (":ro" if ro else "")]
        return self._run(name, args + [image] + cmd)

    def _validate(self) -> bool:
        """spirv_val_seed.sh over the seed. It exits 0 whatever it sets aside, so its count is checked: more than
        INVALID_MAX of the modules (or none at all) fails, and step 3's note goes too, so the next build translates the
        set-aside modules again (oot_seed.py translates what the seed folder lacks)."""
        if not self._seed_tools():
            return False
        code, lines = self._docker_out("spirv-val", D2R, [(self.root, "/repo", False)],
                                       self._lf("/repo/native-harmonyos/tools/spirv_val_seed.sh",
                                                [f"/repo/prepared/oot/seed-build/shaders/{VER}"]))
        if code:
            return False
        m = next(filter(None, (re.match(r"(\d+) modules, (\d+) invalid", l.strip()) for l in reversed(lines))), None)
        n, bad = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        if n and bad <= n * INVALID_MAX:
            return True
        (self.notes / "seed_translate.txt").unlink(missing_ok=True)
        if not m:
            self.ev.red("spirv-val 校验没有给出结果（日志里没有“N modules, F invalid”）")
        elif not n:
            self.ev.red(f"seed-build/shaders/{VER} 里没有着色器模块：下次构建会重新翻译")
        else:
            self.ev.red(f"spirv-val 校验：{n} 个着色器模块里 {bad} 个无效（正常不到 1%），已挪到 seed-build/shaders/invalid-{VER}，"
                        "下次构建会重新翻译它们；原因看 seed-build/shaders/spirv-val.txt 的第一条错误")
        return False

    def _note(self, step: str, text: str = "") -> bool:
        self.notes.mkdir(parents=True, exist_ok=True)
        (self.notes / f"{step}.txt").write_text(time.strftime("%Y-%m-%d %H:%M:%S") + "\t" + text + "\n", encoding="utf-8")
        return True

    def _seed_step(self, step: str, run):
        """A seed step that leaves a note when its script finished. The notes of this and the later seed steps go first:
        those worked on the seed folder as it was before this run."""
        def go() -> bool:
            for later in SEED_STEPS[SEED_STEPS.index(step):]:
                (self.notes / f"{later}.txt").unlink(missing_ok=True)
            return bool(run()) and self._note(step)
        return go

    def _containers_ok(self, lines: list) -> bool:
        """oot_seed.py / oot_mesh.py exit 0 when one of their translation containers failed: that is a failure here."""
        bad = [l.strip() for l in lines if "the container failed" in l]
        if bad:
            self.ev.red(f"{len(bad)} 个翻译容器失败：{bad[0]}")
        return not bad

    # ---------- steps ----------
    def steps(self, inputs: dict, keys, library: list) -> list:
        """[(label, done, run)], the hand pipeline's order (docs/DESIGN-OOT.md; native-harmonyos/oot)."""
        d = self._done
        py = [config.python(), "-I"]
        made = lambda: not self.needs_inputs()   # everything made from the game files is there
        R = "/repo/prepared/oot/rewrite/rewritten"
        return [
            ("解开 NUP（v131072 更新）", lambda: d()["unpack"] or made(), lambda: self._unpack(inputs.get("nup"))),
            ("核对游戏模块", lambda: made() or self._modules_ok(), self._check_modules),
            ("游戏模块存成未压缩 NSO（改写前置）", lambda: (d()["rewrite"] and d()["tables"]) or self._plain_ok(), self._unzbic),
            ("改写游戏代码（main / sdk / subsdk0）", lambda: d()["rewrite"], self._rewrite),
            ("运行时表", lambda: d()["tables"], lambda: self._d2r() and self._docker("tables", D2R, [(self.root, "/repo", False)], [
                "python3", "/repo/native-harmonyos/oot/prepare_runtime_oot.py", "--rewritten", R, "--report",
                f"{R}/phase12-validation.json", "--out", "/repo/prepared/oot/game-data", "--exefs", "/repo/prepared/oot/exefs-plain"])),
            ("打包 RomFS（约 12 分钟）", lambda: d()["romfs"], lambda: self._run("romfs", py + [
                "native-harmonyos/oot/pack_romfs_za.py", f"prepared/nup/{PART}", "prepared/oot/romfs"])[0] == 0),
            ("系统字体", lambda: d()["fonts"], lambda: self._fonts(keys, library)),
            ("客体库（含帧步长、动态分辨率补丁）", lambda: d()["embed"], self._guest),
            ("游戏数据", lambda: d()["gamedata"], lambda: self._run("gamedata", py + [
                "native-harmonyos/game_data_ohos.py", "prepared/oot/romfs", "prepared/oot/shared-fonts", "prepared/oot/game-data"])[0] == 0),
            ("着色器翻译器（Ampere）", lambda: d()["xlate"], self._xlate),
            ("着色器 1/9：从 RomFS 和 main 抽出全部着色器（约 10 分钟）", lambda: d()["seed_split"], self._split),
            ("着色器 2/9：SPIRV-Cross", lambda: d()["seed_spirvcross"], self._spirv_cross_bin),
            ("着色器 3/9：整份翻译（约 10 分钟）", lambda: d()["seed_translate"], self._seed_step("seed_translate", self._translate)),
            ("着色器 4/9：spirv-val 校验", lambda: d()["seed_validate"], self._seed_step("seed_validate", self._validate)),
            ("着色器 5/9：SSA 化（约 8 分钟）", lambda: d()["seed_ssa"], self._seed_step("seed_ssa", lambda: self._seed_tools() and self._docker(
                "spirv-opt", D2R, [(self.seed_build / "shaders", "/s", False), (self.root / "native-harmonyos" / "tools", "/tools", True)],
                self._lf("/tools/spirv_opt_seed.sh", [f"/s/{VER}", jobs(12)])))),
            ("着色器 6/9：测试时补翻的变体和共享内存改写（约 2 分钟）", lambda: d()["seed_variants"],
             self._seed_step("seed_variants", self._variants)),
            ("着色器 7/9：任务 / 网格着色器译成 compute 内核", lambda: d()["seed_mesh"], self._seed_step("seed_mesh", lambda: self._mesh([]))),
            ("着色器 8/9：包装网格内核并装进种子", lambda: d()["seed_mesh_wrap"],
             self._seed_step("seed_mesh_wrap", lambda: self._mesh(["--wrap", "--install"]))),
            ("着色器 9/9：打包 seed.vkpack", lambda: d()["seed"], self._pack),
            ("第三方静态库 libopus / libvpx", lambda: d()["deps"], self._deps),
            ("第三方静态库 dav1d（AV1 过场动画）", lambda: d()["dav1d"], self._dav1d),
        ]

    def prepare(self, inputs: dict, keys, library: list) -> bool:
        return self.upstream_ready() and self._steps(self.steps(inputs, keys, library))

    def _unpack(self, nup) -> bool:
        if not nup:
            self.ev.red("要解开的 NUP 没找到（需要含 v131072 更新的 NUP）")
            return False
        py = self._py_with(ZSTANDARD, "解 NUP ")
        return bool(py) and self._run("unpack", [py, "tools/nup_extract.py", str(nup), "--out", "prepared/nup",
                                                 "--parts", "update"])[0] == 0

    def _check_modules(self) -> bool:
        if self._modules_ok():
            return True
        e = self._expected_ids()
        if not e.get("build_id") or not e.get("exefs_sha256"):
            self.ev.red(f"后端 {self.root} 缺 assets/expected-oot-v131072.json（或读不出来）："
                        + ("把你手上那份完整的 NX2Harmony 重新完整解压到原处" if config.FROZEN
                           else "它是本仓库 backends/ 里的文件，用 git 恢复（git checkout -- backends）"))
        else:
            self.ev.red("main / sdk / subsdk0 和 assets/expected-oot-v131072.json 对不上，不是 v131072 的游戏文件："
                        f"删除 {self.part} 后换含 v131072 更新的 NUP 重新构建")
        return False

    def _unzbic(self) -> bool:
        """nso_unzbic.py: one NSO per call; the rewriter (and the tables' dynsym extent) read the plain copies."""
        py = self._py_with(ZBIC, "解压 Switch 2 模块")
        if not py:
            return False
        self.plain.mkdir(parents=True, exist_ok=True)
        for m in MODULES:
            if self._run(f"unzbic-{m}", [py, "native-harmonyos/oot/nso_unzbic.py", self.exefs / m, self.plain / m])[0]:
                return False
        for f in ("main.npdm", "rtld"):
            shutil.copy2(self.exefs / f, self.plain / f)
        return True

    def _rewrite(self) -> bool:
        """No --record: the Darwin image hashes recorded in the expected file must come out again."""
        if not self._d2r():
            return False
        return self._docker("rewrite", D2R, [(self.root, "/repo", False), (self.upstream(), "/android", True)],
                            ["python3", "/repo/native-harmonyos/oot/rewrite_oot.py", "--android", "/android",
                             "--exefs", f"/repo/prepared/nup/{PART}/exefs", "--plain", "/repo/prepared/oot/exefs-plain",
                             "--out", "/repo/prepared/oot/rewrite"])

    def _guest(self) -> bool:
        """prepare_guest.py oot131072: one nativegames-tools:1 run (the export), then gen_embed_ohos.py with the same
        Python; no shell on the PC."""
        if not self._d2r():
            return False
        return self._run("guest", [config.python(), "-I", "native-harmonyos/prepare_guest.py", VER], env=self._oot_env())[0] == 0

    def _xlate(self) -> bool:
        """prepare_xlate_ns2.py, then the NativeAOT publish (prepare_xlate_ns2.py:17-19). Not xlate/build_xlate.sh: that is
        the Switch 1 translator."""
        if not self._need_nxnative():
            return False
        env = self._oot_env()
        if self._run("xlate-src", [config.python(), "-I", "native-harmonyos/xlate/prepare_xlate_ns2.py", self.xlate_src], env=env)[0]:
            return False
        self.xlate.parent.mkdir(parents=True, exist_ok=True)
        # the restore from the NuGet feed that answers fastest (a NuGet.Config written in the container first), then
        # from nuget.org when that fails (mirrors.nuget_envs); publish_native.sh itself is the reference's, unchanged
        cmd = ["sh", "-c", mirrors.NUGET_CONFIG_SH + "exec sh /src/games/totk/tools/totk_xlate/publish_native.sh dotnet --out /out"]
        for i, env in enumerate(mirrors.nuget_envs(self.ev.say)):
            if i:
                self.ev.say("    换 nuget.org 再试一次")
            if self._docker("xlate" if not i else f"xlate-{i + 1}", DOTNET,
                            [(self.xlate_src, "/src", False), (self.xlate.parent, "/out", False)], cmd, env=env):
                return True
        return False

    def _split(self) -> bool:
        """nxnative's shader_split.py over the unpacked RomFS and main: every shader into pkg/<stage>_other.bin, the map,
        the compute shaders' facts and the vertex / pixel pairs (written last). DM_GAME picks the game's profile.
        shader_split.py runs the zstd program for the files that name no zstd dictionary; when it cannot read some files
        it says so ("[RED] N file(s) could not be read") and still exits 0. That is a failure here (nxnative's
        build_ipa_from_nsp.py takes it so too), and pairs.tsv goes, so the next build runs this step again."""
        nx = self._need_nxnative()
        py = self._py_with(ZBIC + ZSTD, "抽出着色器") if nx else ""
        if not py:
            return False
        if not self.zstd():
            self.ev.red("抽出着色器要用 zstd 程序（时之笛的 RomFS 里有不带字典的 zstd 文件），这台电脑上没有："
                        "在“环境”页一键安装（或 winget install Meta.Zstandard），然后重新构建")
            return False
        s = self.split
        s.mkdir(parents=True, exist_ok=True)
        env = dict(self._oot_env(), DM_GAME=self.title_id, J=str(jobs(16)))
        code, lines = self._run("shader-split", [py, nx / "dm/gfx/tools/shader_split.py", self.part / "romfs", self.exefs / "main",
                                                 s / "pkg", s / "shader_map.tsv", s / "facts.tsv", s / "pairs.tsv"],
                                cwd=self.prep, env=env)
        unread = next((l for l in lines if "file(s) could not be read" in l), "")
        if code or unread:
            (s / "pairs.tsv").unlink(missing_ok=True)
        if unread:
            m = re.search(r"(\d+) file\(s\) could not be read.*?\(first: (.*)\)", unread)
            self.ev.red(f"抽出着色器时有 {m.group(1) if m else '一些'} 个文件读不出来"
                        + (f"（第一个：{m.group(2)}）" if m else "") + "，它们的着色器会缺失：看日志 shader-split 找原因，"
                        "修好后重新构建")
            return False
        return code == 0

    def _spirv_cross_bin(self) -> bool:
        """The Z-A backend's linux-x64 SPIRV-Cross (the same pin) when it has one, else built as za_202.py builds it."""
        self.spirv_cross.parent.mkdir(parents=True, exist_ok=True)
        theirs = config.backend_root("za_202") / "prepared/za/seed-build/spirv-cross/spirv-cross"
        if theirs.is_file():
            shutil.copy2(theirs, self.spirv_cross)
            self.ev.say(f"    复制自 {theirs}")
            return True
        snippet = ("set -e; SC=/repo/prepared/oot/seed-build/spirv-cross; mkdir -p $SC/obj; rm -rf $SC.src; "
                   f"git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git $SC.src; git -C $SC.src checkout -q {za_202.SPIRV_CROSS}; "
                   "for f in main spirv_cfg spirv_cpp spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_hlsl "
                   "spirv_msl spirv_parser spirv_reflect; do echo $f; done | xargs -P $(nproc) -I{} g++ -O3 -DNDEBUG -std=c++11 "
                   "-Wno-deprecated-declarations -c $SC.src/{}.cpp -o $SC/obj/{}.o; g++ $SC/obj/*.o -o $SC/spirv-cross")
        return self._docker("spirv-cross", "python:3.14", [(self.root, "/repo", False)], ["sh", "-c", snippet])

    def _translate(self) -> bool:
        """oot_seed.py: the vertex / pixel / compute shaders not in the seed yet (its env defaults: pools, draw parameters,
        storage-buffer alignment 256), each shard a python:3.14 container."""
        if not self._ensure_base("python:3.14"):   # up to 16 shards start at once: one pull first, not 16
            return False
        code, lines = self._run("seed", [config.python(), "-I", "native-harmonyos/oot/oot_seed.py", "--jobs", str(jobs(16))],
                                env=self._oot_env())
        return code == 0 and self._containers_ok(lines)

    def _variants(self) -> bool:
        """The OoT project's second pass (oot_seed.py --variants --smem, docs/DESIGN-OOT.md "从头建的种子补齐到 a134"): the 146
        variants filled from the phone in testing (oot/oot131072-variants.tsv: hashes and answers only), translated,
        validated and promoted to SSA like the rest, and the shared-memory rewrite of the compute modules that oot-a134
        had (smem_private.py). After the SSA step, before the mesh kernels; about 1.5 minutes."""
        tsv = self.root / "native-harmonyos" / "oot" / "oot131072-variants.tsv"
        if not tsv.is_file():
            self.ev.red(f"缺 {tsv}：时之笛后端版本太旧（没有补翻变体清单），请用新的 NX2Harmony 软件包")
            return False
        if not (self._seed_tools() and self._ensure_base("python:3.14")):
            return False
        code, lines = self._run("seed-variants", [config.python(), "-I", "native-harmonyos/oot/oot_seed.py", "--variants",
                                                  "native-harmonyos/oot/oot131072-variants.tsv", "--smem", "--jobs", str(jobs(16))],
                                env=self._oot_env())
        return code == 0 and self._containers_ok(lines)

    def _mesh(self, extra: list) -> bool:
        """oot_mesh.py (nxnative's dmsh_meshstage.py; python:3.14 and nativegames-tools:1 containers: the wrapper compiles
        with glslangValidator, spirv-opt and spirv-val and drops a kernel that fails). Its own spirv-val pass over
        mesh-build/shaders (oot_mesh.py:156, the LF spirv_val_seed.sh) runs too and moves an invalid kernel aside; the
        hand run had none (docs/DESIGN-OOT.md: 520 kernels, 0 failures), and step 7 translates again whatever it would
        set aside."""
        if not (self._need_nxnative() and self._seed_tools() and self._ensure_base("python:3.14")):
            return False
        code, lines = self._run("mesh-wrap" if extra else "mesh", [config.python(), "-I", "native-harmonyos/oot/oot_mesh.py",
                                                                  "--jobs", str(jobs(12))] + extra, env=self._oot_env())
        return code == 0 and self._containers_ok(lines)

    def _pack(self) -> bool:
        """What oot_seed.py --pack runs, without that script's top-up translation first (it would translate again the
        modules spirv-val set aside, and leave them without the SSA pass)."""
        return self._docker("seed-pack", "python:3.14", [(self.upstream(), "/android", True), (self.root, "/repo", False)],
                            ["python3", "/android/games/totk/scripts/pack_shader_seed.py",
                             f"/repo/prepared/oot/seed-build/shaders/{VER}", "--output", "/repo/prepared/oot/seed-build/seed.vkpack"])

    def _from_backends(self, rel: str, lib: Path) -> bool:
        """artifacts/<rel> copied from another backend that has lib built (the same pinned sources)."""
        mine = self.root / "artifacts" / rel
        for other in ("za_202", "totk_v143"):
            theirs = config.backend_root(other) / "artifacts" / rel
            if not theirs.is_dir() or theirs.resolve() == mine.resolve():
                continue
            if (theirs / lib.relative_to(mine)).is_file():
                shutil.copytree(theirs, mine, dirs_exist_ok=True)
                self.ev.say(f"    {rel} 复制自 {theirs}")
                return True
        return False

    def _deps(self) -> bool:
        """libopus and libvpx: the package's prebuilt ones, else the Z-A or TOTK backend's when it has them, else built as
        za_202.py builds them."""
        self._prebuilt()
        for lib, rel in ((self.opus, "native-opus"), (self.vpx, "native-vpx")):
            if lib.is_file() or self._from_backends(rel, lib):
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
                # an LF copy beside loadsched.py, which it finds through its own folder (HERE=$(dirname "$0"))
                cmd = self._lf("/repo/native-harmonyos/build_vpx_ohos.sh", ["/repo/artifacts/native-vpx"], ("loadsched.py",))
                if not (self._d2r() and self._docker("vpx", D2R, [(self.root, "/repo", False), (sdk_dir() / "sysroot", "/ohos/sysroot", True)],
                                                     cmd)):
                    return False
        return True

    def _dav1d(self) -> bool:
        """dav1d 1.5.4 (native-harmonyos/third_party/dav1d) for the AV1 movies (ns2_av1.c), built against the SDK sysroot.
        No other backend has it, so this is built on every PC: an LF copy beside loadsched.py and third_party (HERE)."""
        if self._from_backends("native-dav1d", self.dav1d):
            return True
        from ..doctor import sdk_dir
        return self._d2r() and self._docker("dav1d", D2R, [(self.root, "/repo", False), (sdk_dir() / "sysroot", "/ohos/sysroot", True)],
                                            self._lf("/repo/native-harmonyos/build_dav1d_ohos.sh", ["/repo/artifacts/native-dav1d"],
                                                     ("loadsched.py", "third_party")))

    def libs(self) -> bool:
        if not self._embed_here("prepared/oot"):
            return False
        return self._run("libs", [config.python(), "-I", "native-harmonyos/build_ohos.py", "--gpu", "vk", "--version", VER,
                                  "--guest", "prepared/oot/embed"])[0] == 0

    # ---------- push ----------
    def push_list(self) -> list:
        """As oot/push_oot.ps1 -GameData -Fonts -RomFS -Seed sends them, in that order (game-oot first: install() waits
        for that folder)."""
        g, r = self.game_data, self.romfs
        out = [(g / f, f"game-oot/{f}") for f in ("runtime-tables.bin", "romfs-config.txt", "shared-fonts.txt")]
        out += [(f, f"game-oot/fonts/{f.name}") for f in sorted(self.fonts.glob("*.bfttf"))]
        out += [(r / f, f"game-oot/{f}") for f in ("runtime-index.bin", "romfs.bin")]
        out += [(self.seed / "sb-align.txt", f"android/shaders/{VER}/sb-align.txt"),
                (self.seed_build / "seed.vkpack", f"android/shaders/{VER}/seed.vkpack")]
        return out

    def push(self) -> bool:
        self.hdc.shell(f"aa force-stop {self.bundle}")   # the app maps the seed it opened (push_oot.ps1 -Seed)
        return super().push()
