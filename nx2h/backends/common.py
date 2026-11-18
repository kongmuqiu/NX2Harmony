"""What every game backend does the same way: run the backend's scripts with their output logged, build the signed HAP
with the game's own bundle name, signing and icon, install it, send the game data (skipping what the phone already
has) and start the game. A game's module (totk_v143.py, za_202.py) adds its inputs, preparation steps and file list.

A backend is an adapted port's project folder (nx2h.local.json "backends", config.backend_root): its HAP builder
native-harmonyos/build_hap.ps1 writes artifacts/release/<tag>/totk-harmonyos-<tag>-signed.hap and audits every entry."""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import time
from pathlib import Path

from .. import bundle as bundles
from .. import config
from .. import device
from .. import mirrors
from .. import proc
from .. import signing

APP_FILES = "/data/storage/el2/base/haps/entry/files"            # the app's own view (hdc file send -b)


def docker_path(p) -> str:
    """A host folder for docker -v on Windows (forward slashes)."""
    return str(Path(p).resolve()).replace("\\", "/")


def have_docker() -> bool:
    """The docker client on PATH (what the backends' scripts look for before they start any step)."""
    return bool(shutil.which("docker"))


def have_image(name: str) -> bool:
    return proc.run(["docker", "image", "inspect", name], capture_output=True).returncode == 0


# the published images the steps run or build FROM (pulled through mirrors.ranked when missing); every other image the
# backends name (nativegames-tools:1) is built here
BASE_IMAGES = ("python:3.14", "mcr.microsoft.com/dotnet/sdk:10.0-noble-aot")


def dockerfile_base(dockerfile: Path) -> str:
    """The image a Dockerfile builds FROM (its first FROM line), "" when none can be read."""
    try:
        for line in Path(dockerfile).read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0].upper() == "FROM":
                return parts[1]
    except OSError:
        pass
    return ""


class BaseBackend:
    key = ""            # the backend's name in games.tsv and nx2h.local.json "backends"
    title_id = ""
    name = ""
    version = ""
    update = ""         # the update's version as scan.py reports it, e.g. "v720896"
    slug = ""           # the bundle name's last part when games.tsv has none
    log_name = ""

    def __init__(self, ev, hdc):
        self.ev = ev
        self.hdc = hdc
        self.root = config.backend_root(self.key)
        row = next((r for r in config.games() if r.get("title_id") == self.title_id), {})
        self.bundle = bundles.for_game(self.title_id, row.get("slug") or self.slug)
        self.shell_files = f"/data/app/el2/100/base/{self.bundle}/haps/entry/files"   # APP_FILES for hdc shell
        self.prep = self.root / "prepared"
        self.stamp = time.strftime("%Y%m%d-%H%M%S")
        config.LOGS.mkdir(parents=True, exist_ok=True)

    # ---------- to be given by each game ----------
    def pick_inputs(self, items) -> tuple:
        """({input name: path}, [problems]) among the scanned items of this game."""
        raise NotImplementedError

    def needs_inputs(self) -> bool:
        """Whether the game files are still needed (False once everything made from them is prepared)."""
        return True

    def needs_keys(self) -> bool:
        return self.needs_inputs()

    def prepare(self, inputs: dict, keys, library: list) -> bool:
        raise NotImplementedError

    def libs(self) -> bool:
        raise NotImplementedError

    def push_list(self) -> list:
        """(local file, path below the app's files/)."""
        raise NotImplementedError

    def extra_stages(self) -> list:
        """(stage name, detail, function) to run after the data is on the phone."""
        return []

    # ---------- running the backend's scripts ----------
    def _run(self, name: str, cmd: list, cwd: Path = None, env=None) -> tuple:
        """Run a command, its output to logs/<stamp>-<game>-<name>.log, each line decoded by itself (UTF-8, else the
        console code page: proc.lines); the newest line goes to the detail event and the progress note.
        Returns (exit code, output lines)."""
        log = config.LOGS / f"{self.stamp}-{self.log_name or self.slug}-{name}.log"
        lines = []
        last = 0.0
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            try:
                p = proc.popen([str(c) for c in cmd], cwd=str(cwd or self.root), env=env or self._env(),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            except OSError as e:
                self.ev.red(f"{name} 启动不了（{e}）：{cmd[0]}")
                return 1, []
            try:
                for line in proc.lines(p.stdout):
                    f.write(line + "\n")
                    f.flush()
                    line = line.rstrip()
                    lines.append(line)
                    if not line.strip():
                        continue
                    self.ev.detail(line)
                    if self.ev.json and time.time() - last > 2:
                        self.ev.progress(name, 0, None, note=line[-160:])
                        last = time.time()
                p.wait()
            finally:
                self.ev.detail_end()
        if p.returncode:
            self.ev.red(f"{name} 失败（退出码 {p.returncode}），日志：{log}")
            for l in lines[-12:]:
                self.ev.say("    " + l)
        return p.returncode, lines

    def _env(self) -> dict:
        """The environment of the backend's scripts. Nothing runs under a shell on the PC (the .sh files run in Linux
        containers), so no Git Bash on PATH. Python children write and read UTF-8 whatever the console code page (their
        Chinese paths in the logs; a child run with -I ignores both). The DevEco Studio and the hdc, server and phone this
        app uses go to every script: ohos_env.py lets the environment win over local.ohos.ps1 / .env, so a script that
        talks to the phone (za_seed.py --pull --send) uses the server discover() found, not hdc's default one. The Java
        the signing uses (device.java_home) goes first on PATH and into JAVA_HOME when PATH would start another one or
        none (device.java_for_scripts): DevEco Studio's jbr only when PATH has no java, as build_hap.ps1 does itself."""
        env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8", TOTK_OHOS_BUNDLE=self.bundle)
        use = device.find_deveco().use
        if use:   # the DevEco Studio this app uses, for every script (ohos_env.py / .ps1, prepare_v143.py, build_hap.ps1)
            env["TOTK_DEVECO"] = str(use.root)
        jdk = device.java_for_scripts(env.get("PATH", ""))   # hvigor runs `java` from PATH, keytool runs this one
        if jdk:
            env["JAVA_HOME"] = str(jdk)
            env["PATH"] = str(jdk / "bin") + os.pathsep + env.get("PATH", "")
        h = getattr(self, "hdc", None)
        if h is not None:
            for name, value in (("TOTK_HDC", h.exe if h.available() else ""), ("TOTK_HDC_SERVER", h.server),
                                ("TOTK_HDC_TARGET", h.target)):
                if value:
                    env[name] = str(value)
        return env

    def _docker(self, name: str, image: str, mounts: list, cmd: list, env=None) -> bool:
        if image in BASE_IMAGES and not self._ensure_base(image):
            return False
        args = ["docker", "run", "--rm"]
        for host, guest, ro in mounts:
            args += ["-v", f"{docker_path(host)}:{guest}" + (":ro" if ro else "")]
        for k, v in (env or {}).items():
            args += ["-e", f"{k}={v}"]
        return self._run(name, args + [image] + cmd)[0] == 0

    def _ensure_base(self, image: str) -> bool:
        """A published image the steps use (BASE_IMAGES), pulled when missing from the source that answered fastest
        just now (mirrors.ranked: measured once per run, and only when an image is missing), the next one when a pull
        fails. A mirror's copy must carry the digest the probe saw (the official one, when the official registry
        answered) and is then tagged with the image's own name, so docker run and FROM find it without a pull. With
        mirrors.ENABLED off nothing is pulled here: docker run and docker build pull it themselves, as before."""
        if not mirrors.ENABLED or have_image(image):
            return True
        sources = mirrors.ranked("registry", self.ev.say, image)
        for i, s in enumerate(sources):
            ref = s.ref(image)
            self.ev.say(f"    下载 Docker 镜像 {image}" + ("" if ref == image else f"（经 {s.name}）"))
            if self._run(f"pull-{i + 1}", ["docker", "pull", ref])[0]:
                continue
            if ref == image:
                return True
            digests = proc.run(["docker", "image", "inspect", "--format", "{{json .RepoDigests}}", ref],
                               capture_output=True, text=True, errors="replace").stdout
            ok = not s.digest or s.digest in digests
            if ok:
                ok = proc.run(["docker", "tag", ref, image], capture_output=True).returncode == 0
            proc.run(["docker", "rmi", ref], capture_output=True)   # the mirror's name only; image keeps the layers
            if ok and have_image(image):
                return True
            self.ev.say(f"    {s.name} 给的镜像和测速时核对的摘要不一致，不用它")
        self.ev.red(f"Docker 镜像 {image} 从 {len(sources)} 个下载源都没拉下来：多半是网络问题，网络好时重新构建（已完成的步骤会跳过）")
        return False

    def _ensure_image(self, image: str, dockerfile: Path) -> bool:
        """The image, built from its Dockerfile when missing: its FROM image first (_ensure_base), then up to four
        builds with the Debian archive and the Python package index that answered fastest (mirrors.apt_attempts /
        pip_attempts: the fastest archive twice, then the next ones; always over HTTPS). A Dockerfile without the
        APT_MIRROR / PIP_MIRROR arguments just ignores them."""
        if have_image(image):
            return True
        self.ev.say(f"    构建 Docker 镜像 {image}（只需一次）")
        base = dockerfile_base(dockerfile)
        if base in BASE_IMAGES and not self._ensure_base(base):
            return False
        apt = mirrors.apt_attempts(self.ev.say)
        pips = mirrors.pip_attempts(self.ev.say) or [""]
        for i, mirror in enumerate(apt):
            pip = pips[i % len(pips)]
            if i:
                change = "，".join(x for x in (f"软件源换成 {mirror}" if mirror else "", f"Python 包源 {pip}" if pip else "") if x)
                self.ev.say("    镜像没建成，再试一次" + (f"（{change}）" if change else ""))
            args = ["docker", "build", "-t", image, "-f", dockerfile]
            if mirror:   # none: the Dockerfile's own default (a package may preset one there)
                args += ["--build-arg", f"APT_MIRROR={mirror}"]
            if pip:
                args += ["--build-arg", f"PIP_MIRROR={pip}"]
            if self._run(f"image-{i + 1}" if i else "image", args + [dockerfile.parent])[0] == 0:   # a log per attempt
                return True
        self.ev.red(f"Docker 镜像 {image} 建了 {len(apt)} 次都没成功：多半是网络连不上软件源，网络好时重新构建（已完成的步骤会跳过）")
        return False

    def _pip_env(self) -> dict:
        """The scripts' environment with the fastest Python package index (PIP_INDEX_URL when it is not PyPI's own),
        for a step that installs Python modules; measured the first time a run needs it."""
        return dict(self._env(), **mirrors.pip_env(self.ev.say))

    # the third-party libraries every backend links, as packaging/full_bundle.py puts them beside the backends:
    # backends/prebuilt-ohos/<folder> mirrors artifacts/<folder> (pinned sources, built for aarch64-linux-ohos)
    PREBUILT = {"native-opus": "native-opus/build-ohos/opus/libopus.a", "native-vpx": "native-vpx/ohos/lib/libvpx.a"}

    def _prebuilt(self) -> None:
        """Copy the package's prebuilt libopus / libvpx into artifacts/ where this backend has none built, so no PC has
        to build them (libopus needs PowerShell 7, libvpx a container, both the network)."""
        src = self.root.parent / "prebuilt-ohos"
        for folder, lib in self.PREBUILT.items():
            if (self.root / "artifacts" / lib).is_file() or not (src / lib).is_file():
                continue
            shutil.copytree(src / folder, self.root / "artifacts" / folder, dirs_exist_ok=True)
            self.ev.say(f"    {folder}：用软件包自带的预编译库")

    def _embed_here(self, out: str) -> bool:
        """The guest library sources (<out>/embed, gen_embed_ohos.py) name the game images by absolute path (.incbin);
        prepared data made on another PC or in another folder points elsewhere. Then they are made again here from the
        same images (<out>/rewrite[n]/..., <out>/x0: a plain Python script, no Docker): the output is the same but for
        the paths. True when the sources are usable."""
        import re as _re
        out_dir = self.root / out
        s = out_dir / "embed" / "guest_images.S"
        if not s.is_file():
            return True   # nothing prepared yet: the guest step makes them
        paths = _re.findall(r'\.incbin "([^"]+)"', s.read_text(encoding="utf-8", errors="replace"))
        if all(Path(p).is_file() for p in paths):
            return True
        rewritten = next((d for d in (out_dir / "rewrite" / "rewritten", out_dir / "rewritten") if d.is_dir()), None)
        if not rewritten or not (out_dir / "x0").is_dir():
            self.ev.red(f"客体库源码指向别的位置，又找不到改写后的模块重新生成：{out_dir}")
            return False
        self.ev.say("    客体库源码里的路径是别的电脑 / 文件夹的，按这里的位置重新生成")
        return self._run("embed", [config.python(), "-I", "native-harmonyos/gen_embed_ohos.py", str(rewritten),
                                   str(out_dir / "x0"), str(out_dir / "embed")], cwd=self.root)[0] == 0

    def _steps(self, steps: list) -> bool:
        """[(label, done, run)]: each step reported, skipped when its output is there, else run."""
        for label, done, run in steps:
            self.ev.step(label)
            if done():
                self.ev.say("    已有，跳过")
                continue
            if not run():
                return False
            if not done():
                self.ev.red(f"{label}：运行结束但没有产出预期的文件")
                return False
            self.ev.say("    完成")
        return True

    # ---------- the HAP ----------
    def hap_path(self, tag: str) -> Path:
        return self.root / "artifacts" / "release" / tag / f"totk-harmonyos-{tag}-signed.hap"

    def hap(self, tag: str, icon=None):
        """The signed HAP with this game's bundle name and signing (signing.apply) and, given the game's icon (a file
        in the app's data folder), that icon: AppScope's app_icon is moved aside to the ignored prepared/nx2h-stage
        folder for this build and put back afterwards (also at the next build, after one that was stopped), so the
        backend's project never holds a game picture."""
        self._restore_icon()
        why = signing.apply(self.root, self.bundle)   # app.json5 bundleName and build-profile.json5 signing, this build only
        if why and why != "applied":
            signing.restore(self.root)
            self.ev.red(why)
            return None
        self.ev.say(f"    包名：{self.bundle}")
        if icon:
            self._swap_icon(Path(icon))
        use = device.find_deveco().use   # named outright too: a local.ohos.ps1 the script reads may set TOTK_DEVECO
        try:
            code, _ = self._run("hap", ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                        str(self.root / "native-harmonyos" / "build_hap.ps1"), "-Tag", tag]
                                + (["-DevEcoRoot", str(use.root)] if use else []))
        finally:
            self._restore_icon()
            signing.restore(self.root)
        hap = self.hap_path(tag)
        return hap if code == 0 and hap.is_file() else None

    def _media(self) -> Path:
        return self.root / "AppScope" / "resources" / "base" / "media"

    def _kept(self) -> Path:
        return self.prep / "nx2h-stage" / "app_icon"

    IMAGES = (".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp")

    def _icon_targets(self) -> list:
        """[(image file, role)] the app's icons come from: the media AppScope/app.json5 "icon" and the abilities'
        "icon" / "startWindowIcon" (entry/src/main/module.json5) name, in every media folder (base, dark, ...) of
        AppScope and entry. A layered icon (<name>.json with "layered-image") gives its background and foreground."""
        import json
        import re
        names = set()
        for f, keys in ((self.root / "AppScope" / "app.json5", ("icon",)),
                        (self.root / "entry" / "src" / "main" / "module.json5", ("icon", "startWindowIcon"))):
            try:
                text = f.read_text(encoding="utf-8")
            except OSError:
                continue
            for k in keys:
                names.update(re.findall(rf'"{k}"\s*:\s*"\$media:([^"]+)"', text))
        names.add("game_icon")   # the shell's own picture of the game (loading screen, launcher), where it has one
        dirs = [d for top in (self.root / "AppScope" / "resources", self.root / "entry" / "src" / "main" / "resources")
                if top.is_dir() for d in sorted(top.glob("*/media")) if d.is_dir()]
        out = []
        for d in dirs:
            for n in sorted(names):
                layered = d / f"{n}.json"
                if layered.is_file():
                    try:
                        spec = json.loads(layered.read_text(encoding="utf-8")).get("layered-image") or {}
                    except ValueError:
                        spec = {}
                    for role in ("background", "foreground"):
                        m = re.match(r"\$media:(.+)", str(spec.get(role) or ""))
                        if m:
                            out += [(f, role) for f in d.glob(m.group(1) + ".*") if f.suffix.lower() in self.IMAGES]
                out += [(f, "full") for f in d.glob(n + ".*") if f.suffix.lower() in self.IMAGES]
        seen, unique = set(), []
        for f, role in out:
            if f not in seen:
                seen.add(f)
                unique.append((f, role))
        return unique

    def _swap_icon(self, icon: Path) -> None:
        """The game's icon in place of each image the app's icons come from (layered: the icon as the background, a
        clear foreground). The originals go to prepared/nx2h-stage/app_icon with a list of what was written."""
        import json
        from PySide6.QtCore import Qt
        from PySide6.QtGui import QImage   # packed with the app; no window needed to convert a picture
        img = QImage(str(icon))
        if img.isNull():
            self.ev.say(f"    游戏图标读不出来（{icon.name}），HAP 用默认图标")
            return
        targets = self._icon_targets()
        if not targets:
            self.ev.say("    工程里找不到应用图标的资源，HAP 用默认图标")
            return
        big = img.scaled(1024, 1024, Qt.IgnoreAspectRatio, Qt.SmoothTransformation)
        clear = QImage(1024, 1024, QImage.Format_ARGB32)
        clear.fill(Qt.transparent)
        kept = self._kept()
        kept.mkdir(parents=True, exist_ok=True)
        moved, written = [], []
        try:
            for f, role in targets:
                rel = f.relative_to(self.root)
                dst = kept / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(f), str(dst))
                moved.append(rel.as_posix())
                new = f.with_suffix(".png")
                if not (clear if role == "foreground" else big).save(str(new), "PNG"):
                    raise OSError(f"写不进 {new}")
                written.append(new.relative_to(self.root).as_posix())
        except OSError as e:
            (kept / "manifest.json").write_text(json.dumps({"moved": moved, "written": written}), encoding="utf-8")
            self._restore_icon()
            self.ev.say(f"    游戏图标写不进 HAP 资源（{e}），HAP 用默认图标")
            return
        (kept / "manifest.json").write_text(json.dumps({"moved": moved, "written": written}), encoding="utf-8")
        names = "、".join(sorted({Path(w).stem for w in written}))
        self.ev.say(f"    HAP 图标：{icon.name}（{img.width()}×{img.height()}）→ {names}")

    def _restore_icon(self) -> None:
        import json
        kept = self._kept()
        manifest = kept / "manifest.json"
        if manifest.is_file():
            m = json.loads(manifest.read_text(encoding="utf-8"))
            for rel in m.get("written", []):
                (self.root / rel).unlink(missing_ok=True)
            for rel in m.get("moved", []):
                src = kept / rel
                if src.is_file():
                    (self.root / rel).unlink(missing_ok=True)
                    shutil.move(str(src), str(self.root / rel))
            manifest.unlink()
            return
        originals = list(kept.glob("app_icon.*")) if kept.is_dir() else []   # the layout before the manifest
        if not originals:
            return
        for f in self._media().glob("app_icon.*"):
            f.unlink()
        for f in originals:
            shutil.move(str(f), str(self._media() / f.name))

    # ---------- the phone ----------
    def install(self, hap: Path) -> bool:
        self.hdc.shell(f"aa force-stop {self.bundle}")
        out = self.hdc.install(hap)
        if "success" not in out.lower():
            self.ev.red("安装失败：" + out.splitlines()[0] if out else "安装失败")
            return False
        # libtotk_ohos.so makes the game folders when its runtime starts (totk_napi.c totk_ohos_start); hdc's shell user
        # may not create them. A plain start only shows the launcher page; the "start" command starts the runtime
        # without the game page (entryability/EntryAbility.ets dispatchCommand). Wait until the first folder the data
        # goes to is there (a cold first start can take longer than a few seconds).
        first = next((rel.split("/", 1)[0] for _, rel in self.push_list() if "/" in rel), "")
        self._start_runtime(first)
        return True

    def _start_runtime(self, wait_for: str = "") -> None:
        """Start the app's runtime once without the game page, so it makes its folders (also the RomFS overlay's
        subfolders, from the overlay manifest already on the phone), then stop it."""
        self.hdc.shell(f"aa force-stop {self.bundle}")
        self.hdc.shell(f"aa start -b {self.bundle} -a EntryAbility --ps command start")
        t0 = time.time()
        time.sleep(4)
        while wait_for and time.time() - t0 < 45 and "No such file" in (self.hdc.shell(f"ls -d '{self.shell_files}/{wait_for}' 2>&1") or "No such file"):
            time.sleep(2)
        time.sleep(3)
        self.hdc.shell(f"aa force-stop {self.bundle}")

    @staticmethod
    def _samples(size: int) -> list:
        mb = max(size >> 20, 1)
        return sorted({i * (mb - 1) // 15 for i in range(16)})

    def _same_on_phone(self, local: Path, rel: str) -> bool:
        """Same size and the same SHA-256 (whole file up to 16 MiB, else 16 sampled MiB)."""
        size = local.stat().st_size
        remote = f"{self.shell_files}/{rel}"
        if self.hdc.size_of(remote) != size:
            return False
        if size <= 16 << 20:
            want = hashlib.sha256(local.read_bytes()).hexdigest()
            got = self.hdc.shell(f"sha256sum '{remote}'", timeout=60).split()
            return bool(got) and got[0] == want
        offs = self._samples(size)
        want = []
        with open(local, "rb") as f:
            for o in offs:
                f.seek(o << 20)
                want.append(hashlib.sha256(f.read(1 << 20)).hexdigest())
        cmd = "for o in " + " ".join(map(str, offs)) + f"; do dd if='{remote}' bs=1048576 skip=$o count=1 2>/dev/null | sha256sum; done"
        got = [l.split()[0] for l in self.hdc.shell(cmd, timeout=120).splitlines() if l.strip()]
        return got == want

    def send(self, local: Path, rel: str, progress=None) -> bool:
        out = self.hdc.send(local, f"{APP_FILES}/{rel}", self.bundle, progress)
        if "finish" not in out.lower() and "/" in rel:
            # its folder is missing: only the app can make folders in its sandbox; its runtime start does (from the
            # overlay manifest for the overlay's subfolders), so start it once and send again
            self._start_runtime(rel.rsplit("/", 1)[0])
            out = self.hdc.send(local, f"{APP_FILES}/{rel}", self.bundle, progress)
        if "finish" not in out.lower():
            self.ev.red(f"发送 {local.name} 失败：{out.splitlines()[-1] if out else ''}")
            return False
        return True

    def push(self) -> bool:
        files = self.push_list()
        missing = [str(p) for p, _ in files if not p.is_file()]
        if missing:
            self.ev.red("缺少要推送的文件：" + ", ".join(missing[:3]))
            return False
        # hdc cannot create folders in the sandbox; the shell can (as the push scripts do)
        folders = sorted({rel.rsplit("/", 1)[0] for _, rel in files if "/" in rel})
        for i in range(0, len(folders), 40):
            self.hdc.shell("mkdir -p " + " ".join(f"'{self.shell_files}/{d}'" for d in folders[i:i + 40]))
        total = sum(p.stat().st_size for p, _ in files)
        done = sent = skipped = 0
        for p, rel in files:
            size = p.stat().st_size
            if self._same_on_phone(p, rel):
                skipped += 1
            else:
                if size > 64 << 20:
                    self.ev.step(f"发送 {p.name}（{size / 2**30:.1f} GB）")
                live = None
                if size > 64 << 20:   # a big file: follow it while it goes (hdc reports only at the end)
                    live = lambda n, base=done: self.ev.progress("推送游戏数据", base + n, total, unit="B")
                if not self.send(p, rel, live):
                    return False
                sent += 1
            done += size
            self.ev.progress("推送游戏数据", done, total, unit="B")
        if not self.ev.json:
            print()
        self.ev.say(f"    发送 {sent} 个文件，{skipped} 个手机上已是同一份，跳过")
        return True

    def launch(self) -> None:
        self.hdc.shell(f"aa start -b {self.bundle} -a EntryAbility")
