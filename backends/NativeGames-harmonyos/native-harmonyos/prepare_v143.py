#!/usr/bin/env python3
"""Prepare Tears of the Kingdom 1.4.3 for the HarmonyOS port in one command, from the player's own files, and build the
native libraries. Every step is a container run of an existing script; steps whose output exists are skipped, so an
interrupted run continues where it stopped. Nothing produced here is ever published: prepared/ and artifacts/ are ignored.

    python native-harmonyos/prepare_v143.py --base <folder with the base game> --update <folder with the 1.4.3 .nsz>
                                          --keys <folder with prod.keys> [--firmware <folder> | --shared-fonts <folder>]
                                          [--only extract,merge,patches,xlate,rewrite,guest,seed,libs] [--dry-run]
                                          [--aspect <W>x<H> ...]   (patches: the UI laid out for that screen too)

Steps (native-harmonyos/, each with its own header comment):
  extract  v143/extract_v143.sh      update NSZ -> NSP, ExeFS, module hashes against expected-v1.4.3.json   (python:3.14)
  merge    v143/merge_v143.sh        base RomFS + update BKTR -> romfs.bin (17 GB), IVFC verified                (python:3.14)
                                     (the base: a card image .xci / .xcz or the eShop package .nsp / .nsz; NSZ and XCZ
                                     are decompressed first, as the update is)
  patches  v143/romfs_patches_v143.sh RomFS overlay files                                                       (python:3.14)
                                     + romfs-patches-<W>x<H> per --aspect: the UI layouts for a screen wider than 16:9
  fonts    extract_shared_fonts.py   the console's shared fonts, from the base card (or --firmware / --shared-fonts) (python:3.14)
  xlate    xlate/build_xlate.sh      the HarmonyOS shader translator (storage buffers at 256)           (dotnet/sdk:10.0-noble-aot)
  rewrite  rewrite_ohos.py           the three code modules, verified against the Android port's hashes       (nativegames-tools:1)
  guest    prepare_guest.py v143     patched text, runtime tables, embedded sources, game data             (nativegames-tools:1 + host)
  seed     build_seed.py             the Vulkan shader seed (~2-3 min: containers sized to Docker's CPUs/memory) (python:3.14)
  deps     build_opus_ohos.ps1, build_vpx_ohos.sh   libopus / libvpx (VP9) static libraries for aarch64-linux-ohos
  libs     build_ohos.py             libtotk_ohos.so / libtotk_guest.so with the HarmonyOS SDK                    (host)
Then: native-harmonyos/build_hap.ps1 -Tag <t> (the HAP, your DevEco signing; audited), hdc install, and v143/push_v143.ps1 (game data).

Needs: Docker (images python:3.14, mcr.microsoft.com/dotnet/sdk:10.0-noble-aot are pulled; nativegames-tools:1 is built from
tools/docker/d2r.Dockerfile when missing), the Android port checkout at its pin (upstream.py fetches it), Python 3.12+
on the host, the HarmonyOS SDK (TOTK_OHOS_SDK) for the last step. No Git Bash: the host runs Python, PowerShell and docker."""
import argparse, hashlib, json, os, platform, re, shlex, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ohos_env import UPSTREAM, PORT, REPO, SDK, docker_path
import upstream

NA = Path(__file__).resolve().parent
WORK = PORT / "prepared" / "totk-v143"                  # extract / merge / patches
OUT = PORT / "prepared" / "harmonyos-v143"           # rewrite / guest / seed
XLATE = PORT / "artifacts" / "xlate-ohos"
D2R_IMAGE = os.environ.get("TOTK_D2R_IMAGE", "nativegames-tools:1")
STEPS = ["extract", "merge", "patches", "fonts", "xlate", "rewrite", "guest", "seed", "deps", "libs"]


def say(*a): print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def run(cmd, dry, cwd=None, shell=False):
    say("$", cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd))
    if dry: return
    subprocess.run(cmd, check=True, cwd=cwd, shell=shell)


# Download sources (NX2Harmony): the values NX2Harmony (or a package's PRESET_ENV) chose, handed into every container
# under the name used there (pip's index; the Ubuntu archive and NuGet feed xlate/build_xlate.sh reads); the host's own
# PIP_INDEX_URL is not passed on. And the mirrors a missing published image is pulled through first (then tagged with
# its own name, after its digest is checked against the official registry's). Empty here: the defaults are the official
# sources. A package can preset them (PRESET_ENV, PULL_FROM).
FORWARD_ENV = {"NX2H_PIP_INDEX_URL": "PIP_INDEX_URL", "NX2H_UBUNTU_MIRROR": "NX2H_UBUNTU_MIRROR",
               "NX2H_UBUNTU_SECURITY": "NX2H_UBUNTU_SECURITY", "NX2H_NUGET_SOURCE": "NX2H_NUGET_SOURCE"}
PRESET_ENV = {}
PULL_FROM = {}


def _official_digest(image):
    """The digest the official registry gives image's tag now ("" when it cannot be asked)."""
    try:
        r = subprocess.run(["docker", "buildx", "imagetools", "inspect", image], capture_output=True, text=True,
                           errors="replace", timeout=120)
    except (OSError, subprocess.SubprocessError):
        return ""
    m = re.search(r"^Digest:\s*(sha256:[0-9a-f]{64})", r.stdout, re.M)
    return m.group(1) if m else ""


def pull_through_mirror(image, dry):
    """A missing published image pulled through PULL_FROM's mirrors in order, tagged with its own name so docker run finds
    it; a copy whose digest differs from the official one is dropped. Best effort: when none works, docker run pulls it
    from its own registry as before."""
    refs = PULL_FROM.get(image) or []
    if dry or not refs or subprocess.run(["docker", "image", "inspect", image], capture_output=True).returncode == 0:
        return
    want = None
    for ref in refs:
        say("$ docker pull", ref)
        if subprocess.run(["docker", "pull", ref]).returncode:
            continue
        if want is None:
            want = _official_digest(image)
        got = subprocess.run(["docker", "image", "inspect", "--format", "{{json .RepoDigests}}", ref], capture_output=True,
                             text=True, errors="replace").stdout
        ok = (not want or want in got) and subprocess.run(["docker", "tag", ref, image]).returncode == 0
        subprocess.run(["docker", "rmi", ref], capture_output=True)   # the mirror's name only
        if ok:
            say(f"{image} from {ref.split('/')[0]}" + ("" if want else " (the official digest could not be asked)"))
            return
        say(f"{ref}: digest differs from the official {image} ({want}), not used")


def docker(image, mounts, cmd, dry, env=None, workdir=None):
    try:
        pull_through_mirror(image, dry)
    except Exception as e:   # only a faster way to the same image: never the reason a step fails
        say(f"pulling {image} through a mirror failed ({e}); docker run pulls it")
    args = ["docker", "run", "--rm"]
    for host, guest, ro in mounts:
        src = host if str(host).startswith("totk-") else docker_path(host)   # a named volume or a host folder
        args += ["-v", f"{src}:{guest}" + (":ro" if ro else "")]
    for k, v in (env or {}).items(): args += ["-e", f"{k}={v}"]
    env_all = dict(os.environ, MSYS_NO_PATHCONV="1")
    for src, dst in FORWARD_ENV.items():   # the environment wins over a package's preset
        value = os.environ.get(src) or PRESET_ENV.get(src)
        if value and dst not in (env or {}):
            env_all[dst] = value
            args += ["-e", dst]   # not printed: -e NAME takes the value from env_all
    if workdir: args += ["-w", workdir]
    say("$", " ".join(args + [image] + cmd))
    if dry: return
    subprocess.run(args + [image] + cmd, check=True, env=env_all)


def have_image(name):
    return subprocess.run(["docker", "image", "inspect", name], capture_output=True).returncode == 0


def screen_size(text):
    """totk-ohos: --aspect <W>x<H>, a landscape screen wider than 16:9 in pixels, as its overlay folder names it."""
    m = re.fullmatch(r"([1-9][0-9]{0,4})x([1-9][0-9]{0,4})", text.strip().lower())
    if not m: sys.exit(f"--aspect {text}: expected <width>x<height> in pixels, e.g. 2848x1320")
    w, h = int(m[1]), int(m[2])
    if w * 900 <= h * 1616:   # totk-ohos: the app uses an overlay only on a screen more than 1% wider than 16:9 (totk_napi.c)
        sys.exit(f"--aspect {text}: " + (f"width first ({h}x{w})" if h * 9 > w * 16 else
                                         "not wider than 16:9 by more than 1%, the game's own layouts fit it (the app would not use these)"))
    return f"{w}x{h}"


def overlay(folder):
    """path -> (size, sha256, original_sha256) from an overlay folder's manifest.tsv; {} without one."""
    f = Path(folder) / "manifest.tsv"
    if not f.exists(): return {}
    rows = (l.split("\t") for l in f.read_text(encoding="utf-8").splitlines() if l and not l.startswith("#"))
    return {r[0]: tuple(r[1:]) for r in rows if len(r) == 4}


def overlay_intact(folder, rows):
    """totk-ohos: every file an overlay's manifest lists is there with its size and SHA-256 (what runtime/romfs.c checks)."""
    try:
        return all((Path(folder) / p).is_file() and (Path(folder) / p).stat().st_size == int(r[0])
                   and hashlib.sha256((Path(folder) / p).read_bytes()).hexdigest() == r[1] for p, r in rows.items())
    except (OSError, ValueError):
        return False


def layout_inputs():
    """totk-ohos: what a per-screen overlay is made from besides the base overlay's inputs (which the manifests compare):
    the port's layout scripts, the reference's make_romfs_patches.py and the step's script, line endings aside. Written to
    romfs-patches-<W>x<H>.inputs beside the folder once the folder is made, so one made by other scripts is made again."""
    files = (NA / "v143/ultrawide_layout.py", NA / "v143/title_layouts_v143.py", UPSTREAM / "games/totk/scripts/make_romfs_patches.py",
             NA / "v143/romfs_patches_v143.sh")
    return "".join(hashlib.sha256(f.read_bytes().replace(b"\r\n", b"\n") if f.is_file() else b"missing").hexdigest() + "  " + f.name + "\n"
                   for f in files)


def one_file(folder, suffix):
    files = sorted(Path(folder).glob("*" + suffix))
    if len(files) != 1: sys.exit(f"{folder}: expected exactly one {suffix} file, found {len(files)}")
    return files[0]


BASE_SUFFIXES = (".xci", ".nsp", ".xcz", ".nsz")   # merge_v143.sh's order when a folder holds several


def base_file(folder):
    """The base game in folder, chosen as merge_v143.sh chooses it: the first of BASE_SUFFIXES present (a card image
    .xci, the eShop .nsp, then the compressed .xcz / .nsz), of which there must be exactly one. Later kinds do not count,
    so a card (or .nsp) can share its folder with the update .nsz; a compressed .nsz base needs a folder without it."""
    if not Path(folder).is_dir():
        sys.exit(f"--base {folder}: not a folder (give the folder that holds the base game file)")
    files = [p for p in Path(folder).iterdir() if p.is_file()]
    for suffix in BASE_SUFFIXES:
        hits = sorted(p for p in files if p.suffix.lower() == suffix)
        if hits:
            if len(hits) != 1:
                sys.exit(f"{folder}: expected exactly one base game {suffix} file, found {len(hits)} (an .nsz base needs "
                         "a folder without the update .nsz)")
            return hits[0]
    sys.exit(f"{folder}: no base game file ({' / '.join(BASE_SUFFIXES)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="folder holding the base game: the card image (.xci / .xcz; 1.0.0, or the 1G+1U card) or "
                                   "the eShop package 1.0.0 (.nsp / .nsz)")
    ap.add_argument("--update", help="folder holding the 1.4.3 update .nsz")
    ap.add_argument("--keys", help="folder holding prod.keys (read inside the containers only)")
    ap.add_argument("--firmware", help="unpacked firmware NCA folder for the shared fonts (not needed with --base: the card carries them)")
    ap.add_argument("--shared-fonts", help="an already extracted shared-fonts folder (manifest.json + .bfttf)")
    ap.add_argument("--only", help="comma-separated subset of steps: " + ",".join(STEPS))
    ap.add_argument("--dry-run", action="store_true", help="print the commands only")
    ap.add_argument("--aspect", action="append", default=[], metavar="WxH",
                    help="patches: also make extracted/v143/romfs-patches-<W>x<H>, the overlay with the UI laid out for that landscape "
                         "screen wider than 16:9 (HUD at the screen's edges; the app picks it by its screen); repeatable")
    a = ap.parse_args()
    aspects = list(dict.fromkeys(screen_size(s) for s in a.aspect))
    steps = a.only.split(",") if a.only else STEPS
    for s in steps:
        if s not in STEPS: sys.exit(f"unknown step {s}; steps: {', '.join(STEPS)}")
    dry = a.dry_run
    repo_mount = (REPO, "/repo", False)
    android_mount = (UPSTREAM, "/android", True)
    P = "/repo"                                        # the project root, mounted by every container step

    say("Android port checkout:"); upstream.ensure()
    if not shutil.which("docker"): sys.exit("docker is not on PATH")
    for d in (WORK / "logs", OUT, XLATE): d.mkdir(parents=True, exist_ok=True)

    # 1. extract: the update's ExeFS
    if "extract" in steps:
        if (WORK / "extracted/v143/elf").is_dir() and any((WORK / "extracted/v143/exefs").glob("main")):
            say("extract: done before (extracted/v143/exefs, elf)")
        else:
            if not (a.update and a.keys): sys.exit("extract needs --update and --keys")
            one_file(a.update, ".nsz")
            docker("python:3.14", [android_mount, (a.keys, "/keys", True), (a.update, "/update", True), (WORK, "/work", False), ("totk-venv", "/venv", False), repo_mount],
                   ["sh", f"{P}/native-harmonyos/v143/extract_v143.sh"], dry)
    # 2. merge: base RomFS + update
    if "merge" in steps:
        idx = WORK / "extracted/v143/romfs/index.json"
        if idx.exists() and (WORK / "extracted/v143/romfs/romfs.bin").exists():
            say("merge: done before (extracted/v143/romfs/romfs.bin)")
        else:
            if not (a.base and a.keys): sys.exit("merge needs --base and --keys")
            base_file(a.base)
            docker("python:3.14", [android_mount, (a.keys, "/keys", True), (a.base, "/base", True), (WORK, "/work", False), ("totk-venv", "/venv", False), ("totk-xci", "/xci", False), repo_mount],
                   ["sh", f"{P}/native-harmonyos/v143/merge_v143.sh"], dry)
    # 3. RomFS overlay files; totk-ohos: with --aspect also one per screen, romfs-patches-<W>x<H> (totk_napi.c picks it)
    if "patches" in steps:
        if (WORK / "extracted/v143/romfs-patches/manifest.tsv").exists():
            say("patches: done before (romfs-patches/manifest.tsv)")
        else:
            docker("python:3.14", [android_mount, (WORK, "/work", False), ("totk-venv", "/venv", False), repo_mount],
                   ["sh", f"{P}/native-harmonyos/v143/romfs_patches_v143.sh"], dry)
        base = overlay(WORK / "extracted/v143/romfs-patches")
        made_by = layout_inputs() if aspects else ""
        for size in aspects:
            name = f"romfs-patches-{size}"
            folder, stamp = WORK / "extracted/v143" / name, WORK / "extracted/v143" / f"{name}.inputs"
            have = overlay(folder)
            # the base overlay's files from the same originals, the layout archive edited further, the others the same bytes,
            # plus layout archives only (the Title one, v143/title_layouts_v143.py); every file as its manifest says; made by
            # today's scripts (the stamp, written once the folder is complete)
            same = (have and base.keys() <= have.keys() and all(p.startswith("UI/LayoutArchive/") for p in have.keys() - base.keys())
                    and all(have[p][2] == base[p][2] and (p.startswith("UI/LayoutArchive/") or have[p] == base[p]) for p in base))
            why = ("" if not have else "does not hold the files romfs-patches holds now" if not same
                   else "has a file missing or changed since its manifest" if not overlay_intact(folder, have)
                   else "was made by other scripts than now (v143/ultrawide_layout.py, title_layouts_v143.py, make_romfs_patches.py, romfs_patches_v143.sh)"
                   if not (stamp.is_file() and stamp.read_bytes() == made_by.encode()) else "")
            if have and not why:
                say(f"patches: done before ({name}/manifest.tsv)")
                continue
            if why: say(f"patches: {name} {why}: made again")
            if not dry and stamp.exists(): stamp.unlink()
            docker("python:3.14", [android_mount, (WORK, "/work", False), ("totk-venv", "/venv", False), repo_mount],
                   ["sh", f"{P}/native-harmonyos/v143/romfs_patches_v143.sh", size], dry)
            if not dry: stamp.write_bytes(made_by.encode())
    # 4. shared fonts (the firmware's), used by prepare_guest.py through TOTK_SHARED_FONTS
    fonts = Path(a.shared_fonts) if a.shared_fonts else WORK / "shared-fonts"
    if "fonts" in steps:
        if (fonts / "manifest.json").exists():
            say(f"fonts: {fonts}")
        elif a.keys and (a.firmware or (a.base and base_file(a.base).suffix.lower() == ".xci")):
            # the Android port's extract_shared_fonts.py: from the base card's system-update partition (--xci), else a
            # firmware dump (an eShop package or a compressed card does not carry the partition)
            card = bool(a.base) and base_file(a.base).suffix.lower() == ".xci"
            source = ["--xci", "/base/" + base_file(a.base).name] if card else ["--firmware", "/firmware"]
            mounts = [android_mount, (a.keys, "/keys", True), (WORK, "/work", False), ("totk-venv", "/venv", False)]
            mounts.append((a.base, "/base", True) if card else (a.firmware, "/firmware", True))
            # quoted: the card's file name goes into the sh -c line and often has spaces and brackets ("... (1G+1U).xci")
            docker("python:3.14", mounts, ["sh", "-c",
                   "[ -x /venv/bin/python ] || (python3 -m venv /venv && /venv/bin/pip install -q cryptography==50.0.2 lz4==4.4.5); "
                   "cd /android/games/totk/scripts && /venv/bin/python extract_shared_fonts.py --keys /keys/prod.keys "
                   + " ".join(shlex.quote(s) for s in source) + " --output /work/shared-fonts && ls /work/shared-fonts"], dry)
        else:
            sys.exit("fonts: give --shared-fonts <folder> (manifest.json + .bfttf), or --keys with --firmware <dump>, or --keys with a "
                     "--base that is a card image .xci (an .nsp / .nsz / .xcz carries no fonts)")
    # 5. the shader translator
    if "xlate" in steps:
        if (XLATE / "totk_xlate").exists():
            say("xlate: done before (artifacts/xlate-ohos/totk_xlate)")
        else:
            docker("mcr.microsoft.com/dotnet/sdk:10.0-noble-aot", [android_mount, (XLATE, "/out", False), repo_mount],
                   ["sh", f"{P}/native-harmonyos/xlate/build_xlate.sh"], dry)
    # 6. the code rewrite
    need_d2r = "rewrite" in steps or "guest" in steps
    if need_d2r and not have_image(D2R_IMAGE):
        run(["docker", "build", "-t", D2R_IMAGE, "-f", str(PORT / "tools/docker/d2r.Dockerfile"), str(PORT / "tools/docker")], dry)
    if "rewrite" in steps:
        if (OUT / "rewritten/phase12-validation.json").exists():
            say("rewrite: done before (rewritten/phase12-validation.json)")
        else:
            docker(D2R_IMAGE, [repo_mount, android_mount],
                   ["python3", f"{P}/native-harmonyos/rewrite_ohos.py", "--android", "/android", "--exefs", f"{P}/prepared/totk-v143/extracted/v143/exefs",
                    "--out", f"{P}/prepared/harmonyos-v143", "--version", "v143"], dry)
    # 7. export + embed + game data (prepare_guest.py runs the container itself; with --dry-run it prints its commands)
    if "guest" in steps:
        if (OUT / "embed/guest_modules.c").exists() and (OUT / "game-data/runtime-tables.bin").exists():
            say("guest: done before (embed/, game-data/)")
        else:
            env = dict(os.environ, TOTK_SHARED_FONTS=str(fonts), TOTK_D2R_IMAGE=D2R_IMAGE)
            cmd = [sys.executable, "-I", str(NA / "prepare_guest.py"), "v143"] + (["--dry-run"] if dry else [])
            say("$", " ".join(cmd))
            subprocess.run(cmd, check=True, cwd=PORT, env=env)
    # 8. the shader seed
    if "seed" in steps:
        seed_dir = OUT / "seed-build-sb256"
        if (seed_dir / "shaders/v143/seed.vkpack").exists():
            say("seed: done before (seed-build-sb256/shaders/v143/seed.vkpack)")
        else:
            # build_seed.sh's seed (the same seed.vkpack) in python:3.14 containers sized to this machine's Docker
            run([sys.executable, "-I", str(NA / "build_seed.py"), "--romfs", str(WORK / "extracted/v143/romfs"),
                 "--modules", str(OUT / "rewritten"), "--out", str(seed_dir), "--xlate", str(XLATE / "totk_xlate"),
                 "--sb-align", "256", "--version", "v143"], dry, cwd=PORT)
    # 9. third-party static libraries the runtime links (pinned public sources; artifacts/, rebuilt when missing)
    if "deps" in steps:
        opus = PORT / "artifacts/native-opus/build-ohos/opus/libopus.a"
        if opus.exists(): say("deps: libopus present")
        else:
            pwsh = os.environ.get("TOTK_PWSH") or shutil.which("pwsh") or sys.exit("libopus build needs PowerShell 7 (pwsh) and DevEco Studio (TOTK_DEVECO)")
            deveco = os.environ.get("TOTK_DEVECO") or str(SDK.parents[3])   # <DevEco>/sdk/default/openharmony/native -> <DevEco>
            run([pwsh, "-NoProfile", "-File", str(NA / "build_opus_ohos.ps1"), "-DevEcoRoot", deveco], dry, cwd=PORT)
        vpx = PORT / "artifacts/native-vpx/ohos/lib/libvpx.a"
        if vpx.exists(): say("deps: libvpx present")
        else:
            if not have_image(D2R_IMAGE):
                run(["docker", "build", "-t", D2R_IMAGE, "-f", str(PORT / "tools/docker/d2r.Dockerfile"), str(PORT / "tools/docker")], dry)
            docker(D2R_IMAGE, [repo_mount, (SDK / "sysroot", "/ohos/sysroot", True)],
                   ["sh", f"{P}/native-harmonyos/build_vpx_ohos.sh", f"{P}/artifacts/native-vpx"], dry)
    # 10. the native libraries
    if "libs" in steps:
        run([sys.executable, "-I", str(NA / "build_ohos.py"), "--gpu", "vk", "--version", "v143", "--guest", str(OUT / "embed")], dry, cwd=PORT)
    say("done. Next: native-harmonyos/build_hap.ps1 -Tag <tag> (the HAP with your DevEco signing), hdc install, start the app once, then v143/push_v143.ps1 -RomFS -GameData -Seed -Patches")


if __name__ == "__main__":
    main()
