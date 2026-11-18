#!/usr/bin/env python3
"""After rewrite_ohos.py: export main's patched text (tools/export_text.c, game patches at build), the runtime tables
(scripts/prepare_runtime.py), the guest library sources (gen_embed_ohos.py) and the game data folder
(game_data_ohos.py), all under prepared/harmonyos-v143 (game material, never published). One docker run
(nativegames-tools:1) and two Python steps on the host: no Git Bash or sh on the host (prepare_guest.sh only runs this
file, for hand use).

    python -I native-harmonyos/prepare_guest.py v143 [--dry-run]   (--dry-run: print the commands only)
    python -I native-harmonyos/prepare_guest.py za202   Pokemon Legends: Z-A 2.0.2: only main's exported text
                                (overlay/runtime/game_patches_za.c: the wide-screen camera stubs) and the guest library
                                sources prepared/za/embed; its runtime tables and game data are
                                za/prepare_runtime_za.py's and za/pack_romfs_za.py's

v143: runtime/game_patches_v143.c, exported as the upstream's iOS export does it on the Mac (TOTK_EXPORT_IOS=1: the
core patches and their veneers go into the text, the phone only checks them; overlay/runtime/game_patches_v143.c takes
those iOS branches on HarmonyOS) plus the ultrawide stubs (runtime/aspect_patch.h; the ratio stays on the data page).
TOTK_RENDER_RES at the export puts the render-size patch in, so the device can pick any size at start (it writes the
size to the data page). The v143 export also writes the switchable renderer veneers (overlay game_patches_v143.c
patch_ohos_render_switches, TOTK_OHOS_EXPORT).

Machine values (ohos_env.py: the environment, then native-harmonyos/local.ohos.ps1 and local.ohos.env): TOTK_UPSTREAM
the totk-android checkout; TOTK_D2R_IMAGE another tools image; TOTK_SHARED_FONTS the console's shared fonts (default
prepared/totk-v143/shared-fonts, prepare_v143.py step fonts); TOTK_UPSTREAM_UNPINNED=1 goes on at another commit."""
import argparse, os, shlex, subprocess, sys, time
if hasattr(sys.stdout, "reconfigure"):   # Chinese messages on a PC whose code page has no Chinese: '?', not a traceback
    sys.stdout.reconfigure(errors="replace")
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ohos_env import PORT, UPSTREAM, docker_path   # noqa: E402
import upstream   # noqa: E402

# per version, as prepare_guest.sh had them: the output folder, the rewritten modules (on the host, and as the export
# reads them in the container), the game patches and their defines, the export's environment, the ExeFS for the
# runtime tables and the RomFS for the game data
VERSIONS = {
    "v143": dict(out="prepared/harmonyos-v143", rewritten="prepared/harmonyos-v143/rewritten",
                 x_in="$P/prepared/harmonyos-v143/rewritten",
                 patches="$P/native-harmonyos/overlay/runtime/game_patches_v143.c",
                 defs="-DTOTK_GAME_V143=1 -DTOTK_OHOS_EXPORT=1 -I$P/native-harmonyos/overlay/runtime",
                 xenv="TOTK_EXPORT_IOS=1 TOTK_RENDER_RES=1280x720",
                 exefs="prepared/totk-v143/extracted/v143/exefs", romfs="prepared/totk-v143/extracted/v143/romfs"),
    "za202": dict(out="prepared/za", rewritten="prepared/za/rewrite/rewritten", x_in="$P/prepared/za/rewrite/rewritten",
                  patches="$P/native-harmonyos/overlay/runtime/game_patches_za.c",
                  defs="-DTOTK_GAME_ZA=1 -DTOTK_OHOS_EXPORT=1 -I$P/native-harmonyos/overlay/runtime",
                  xenv="TOTK_EXPORT_ZA=1"),
}
# versions whose runtime tables and game data other scripts make: the export and the embed only
EXPORT_ONLY = ("za202",)
STAGE_RUNTIME = False   # the game patches compiled from the runtime as build_ohos.py stages it in /tmp/rt


def say(*a): print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def container_script(ver):
    """The `sh -c` script of the one docker run, word for word what prepare_guest.sh passed (container paths only: $P
    the project, $T the reference's games/totk, $V the output folder)."""
    v = VERSIONS[ver]
    lines = [f"  set -e; T=/android/games/totk; P=/repo; V=$P/{v['out']}; mkdir -p $V/x0 $V/game-data /tmp/b"]
    if STAGE_RUNTIME:
        lines.append("  cp -r $T/runtime /tmp/rt && cp $P/native-harmonyos/overlay/runtime/* /tmp/rt/"
                     "   # the runtime as build_ohos.py stages it (overlay files win)")
    lines.append(f"  gcc -std=gnu11 -O1 -Wall -D_GNU_SOURCE {v['defs']} -I$T/runtime {v['patches']} "
                 "$P/native-harmonyos/tools/export_text.c -o /tmp/b/export_text -lm")
    lines.append(f"  cd /tmp && env {v['xenv']} /tmp/b/export_text {v['x_in']} $V/x0 2>&1 | tail -12")
    if EXPORT_ONLY:
        lines.append("  " + " || ".join(f"[ {ver} = {e} ]" for e in EXPORT_ONLY) + " && exit 0")
    # (the five spaces are what the .sh's line continuation inside its double quotes left)
    lines.append(f"  python3 $T/scripts/prepare_runtime.py --version {ver} --rewritten $V/rewritten --report "
                 f"$V/rewritten/phase12-validation.json     --out $V/game-data --exefs $P/{v.get('exefs', '')}")
    return "\n" + "\n".join(lines)


def commands(ver):
    """[the docker run, then the host steps], each run from the project root."""
    v = VERSIONS[ver]
    image = os.environ.get("TOTK_D2R_IMAGE") or "nativegames-tools:1"
    out = [["docker", "run", "--rm", "-v", f"{docker_path(PORT)}:/repo", "-v", f"{docker_path(UPSTREAM)}:/android:ro",
            image, "sh", "-c", container_script(ver)]]
    py = [sys.executable, "-I"]
    out.append(py + ["native-harmonyos/gen_embed_ohos.py", v["rewritten"], f"{v['out']}/x0", f"{v['out']}/embed"])
    if ver not in EXPORT_ONLY:
        fonts = os.environ.get("TOTK_SHARED_FONTS") or "prepared/totk-v143/shared-fonts"
        out.append(py + ["native-harmonyos/game_data_ohos.py", v["romfs"], fonts, f"{v['out']}/game-data"])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", nargs="?", default="v143", choices=sorted(VERSIONS))
    ap.add_argument("--dry-run", action="store_true", help="print the commands only")
    a = ap.parse_args()
    s = upstream.status()   # 0 at the pin, 1 missing, 2 another commit or an incomplete copy
    if s and os.environ.get("TOTK_UPSTREAM_UNPINNED") != "1":
        msg = (("参考仓库 totk-android 不存在：先运行 python native-harmonyos/upstream.py（软件包里应自带，缺了请重新下载）。\n"
                "the totk-android checkout is missing (native-harmonyos/upstream.py fetches it)") if s == 1 else
               ("参考仓库 totk-android 不在固定提交：先运行 python native-harmonyos/upstream.py。\n"
                "the totk-android checkout is not at the pinned commit (native-harmonyos/UPSTREAM_PIN)"))
        if not a.dry_run:
            print(msg, flush=True)
            sys.exit(2)
        say("(dry run) " + msg.splitlines()[-1])
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    for cmd in commands(a.version):
        say("$", " ".join(shlex.quote(str(c)) for c in cmd))
        if a.dry_run:
            continue
        try:
            r = subprocess.run(cmd, cwd=str(PORT), env=env)
        except OSError as e:
            sys.exit(f"启动不了 {cmd[0]}：{'先安装并启动 Docker Desktop' if cmd[0] == 'docker' else e}。\n"
                     f"{cmd[0]} could not be started ({e})")
        if r.returncode:
            sys.exit(r.returncode)


if __name__ == "__main__":
    main()
