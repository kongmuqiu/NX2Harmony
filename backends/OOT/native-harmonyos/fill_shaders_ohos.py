"""Fill shader variants the device met but the seed lacks: the HarmonyOS counterpart of the reference's
android/fill_shaders.py. A variant the seed lacks skips its draws and leaves its request in files/android/shader-requests/
(android/gfx/vk_shader.c). This pulls the requests with hdc, translates them in the python:3.14 container with the
HarmonyOS translator (artifacts/xlate-ohos/totk_xlate, the seed's storage-buffer alignment from its sb-align.txt) and the
reference's reflection (tools/fill_xlate_in_container.py), adds them to the local seed folder, and sends the new files
into the device's shaders/<version>, where the next start draws with them. The app's sandbox does not let hdc move the
requests to done/, so the ones filled are remembered in <seed>/filled-requests.txt and skipped next time.
usage: python fill_shaders_ohos.py [--version za202|v143|oot131072]
oot131072 (Ocarina of Time): the Switch 2 translator (artifacts/xlate-ohos-ns2), and the new modules get the seed's
SSA pass (tools/spirv_opt_seed.sh) and spirv-val (tools/spirv_val_seed.sh) before the seed is packed and sent; both run
as LF copies, and when either fails (its exit code, or spirv-val's count) nothing is packed or sent and these requests are
not recorded as filled, so the next run fills them again.
"""
import argparse, json, os, re, shlex, shutil, subprocess, sys, tempfile
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
from ohos_env import HDC, DEV, UPSTREAM, REPO, docker_path   # machine values: environment or native-harmonyos/local.ohos.env
import upstream; upstream.require()
BUNDLE = os.environ.get("TOTK_OHOS_BUNDLE") or "nx2h.u000000.oot"
FILES = "/data/storage/el2/base/haps/entry/files/android"
# the share of the new modules spirv-val may set aside, as NX2Harmony's seed step allows; more (or every one) points at the
# tools, e.g. no spirv-val in the image, rather than at a few bad translations
INVALID_MAX = 0.05


def hdc(*args):
    r = subprocess.run([HDC, *DEV, *args], capture_output=True, text=True, errors="replace")
    return (r.stdout + r.stderr).strip()


def lf_script(name, args):
    """`sh -c` running an LF copy of tools/<name> in the container: a CRLF checkout of the scripts (core.autocrlf) stops
    dash at its first line (set -eu\\r: Illegal option). The scripts call themselves again through $0, so the copy."""
    src = f"/repo/native-harmonyos/tools/{name}"
    return ["sh", "-c", f"set -e; mkdir -p /tmp/lf; tr -d '\\r' < {src} > /tmp/lf/{name}; "
            + " ".join(["sh", f"/tmp/lf/{name}"] + [shlex.quote(str(x)) for x in args])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="za202", choices=("za202", "v143", "oot131072"))
    a = ap.parse_args()
    build = (PORT / "prepared" / "za" / "seed-build" if a.version == "za202" else PORT / "prepared" / "oot" / "seed-build" if a.version == "oot131072"
             else PORT / "prepared" / f"harmonyos-{a.version}" / "seed-build-sb256")
    translator = "/repo/artifacts/xlate-ohos-ns2/totk_xlate" if a.version == "oot131072" else "/repo/artifacts/xlate-ohos/totk_xlate"
    if a.version == "oot131072" and os.environ.get("OOT_SEED_BUILD"):   # a translator trial beside the seed in use (oot_seed.py)
        build = Path(os.environ["OOT_SEED_BUILD"])
    if a.version == "oot131072" and os.environ.get("OOT_XLATE"):
        translator = "/repo/" + Path(os.environ["OOT_XLATE"]).resolve().relative_to(REPO).as_posix()
    seed = build / "shaders" / a.version
    align = (seed / "sb-align.txt").read_text().strip()
    done_list = seed / "filled-requests.txt"
    done = set(done_list.read_text().split()) if done_list.is_file() else set()
    with tempfile.TemporaryDirectory(dir=PORT / "prepared") as tmp:
        tmp = Path(tmp)
        print(hdc("file", "recv", "-b", BUNDLE, f"{FILES}/shader-requests", str(tmp)).splitlines()[-1])
        req = tmp / "shader-requests"
        if not req.is_dir():
            print("no shader requests on the device"); return 0
        new = tmp / "new"; new.mkdir()
        for p in req.glob("*.json"):
            if p.name not in done:
                shutil.copy2(p, new / p.name)
        n = len(list(new.glob("*.json")))
        print(f"{n} new requests ({len(done)} filled before)")
        if not n:
            return 0
        rel = lambda p: "/repo/" + p.resolve().relative_to(REPO).as_posix()
        out = tmp / "out"
        cmd = ["docker", "run", "--rm", "-v", docker_path(UPSTREAM) + ":/android:ro", "-v", docker_path(REPO) + ":/repo",
               "-e", f"TOTK_XLATE_SB_ALIGN={align}", *(["-e", "TOTK_VK_DEPS=queried", "-e", "TOTK_XLATE_DRAW_PARAMS=1"] if a.version == "za202" else
                                                    ["-e", "TOTK_VK_DEPS=queried", "-e", "TOTK_XLATE_DRAW_PARAMS=1", "-e", "TOTK_XLATE_POOL_SET=2", "-e", "TOTK_XLATE_POOL_TEX=33068", "-e", "TOTK_XLATE_POOL_SAMP=4096"]
                                                    if a.version == "oot131072" else []), "python:3.14", "python3",
               "/repo/native-harmonyos/tools/fill_xlate_in_container.py", rel(new), rel(seed), rel(out),
               translator, rel(PORT / "prepared" / "oot" / "seed-build" / "spirv-cross" / "spirv-cross" if a.version == "oot131072" else build / "spirv-cross" / "spirv-cross")]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env={**__import__("os").environ, "MSYS_NO_PATHCONV": "1"})
        print(r.stdout.strip()[-2000:], r.stderr.strip()[-2000:])
        if r.returncode:
            return 1
        filled = json.loads((out / "filled.json").read_text())
        for f in (filled["files"] if a.version != "oot131072" else []):   # oot131072: only the packed seed goes over (its modules get the SSA pass)
            res = hdc("file", "send", "-b", BUNDLE, str(out / f), f"{FILES}/shaders/{a.version}/{f}")
            if "finish" not in res:
                print("send failed:", f, res[-200:])
        done.update(filled["done"])
        if a.version != "oot131072":   # oot131072: once the new modules passed the SSA pass and spirv-val, below
            done_list.write_text("\n".join(sorted(done)) + "\n")
        print(f"sent {len(filled['files'])} files for {len(filled['done'])} variants; {len(filled['failed'])} failed")
        if filled["files"] and a.version == "oot131072":   # the seed's SSA pass and validation for the new modules
            lst = build / "new-modules.txt"
            names = [f for f in filled["files"] if f.endswith(".spv")]
            lst.write_bytes(("\n".join(names) + "\n").encode())
            for script in ("spirv_opt_seed.sh", "spirv_val_seed.sh"):
                args = [rel(seed), "4", rel(lst)] if script == "spirv_opt_seed.sh" else [rel(seed), rel(lst)]
                r = subprocess.run(["docker", "run", "--rm", "-v", docker_path(REPO) + ":/repo", "nativegames-tools:1", *lf_script(script, args)],
                                   capture_output=True, text=True, errors="replace", env={**os.environ, "MSYS_NO_PATHCONV": "1"})
                print(script, (r.stdout + r.stderr).strip()[-300:])
                why = f"exit code {r.returncode}" if r.returncode else ""
                if not why and script == "spirv_val_seed.sh":   # it exits 0 whatever it sets aside: its count decides
                    m = re.search(r"(\d+) modules, (\d+) invalid", r.stdout)
                    n, bad = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
                    why = ("no 'N modules, F invalid' line" if not m else f"{n} of {len(names)} new modules checked" if n != len(names)
                           else f"{bad} of {n} new modules invalid" if bad and (bad == n or bad > max(1, n * INVALID_MAX)) else "")
                if why:
                    print(f"{script} 没通过（{why}）：种子没有打包、没有发送。修好后再运行一次，这批请求会重新填。\n"
                          f"{script} failed ({why}): nothing packed or sent; run this again once fixed, these requests are filled again")
                    return 1
        if a.version == "oot131072":
            done_list.write_text("\n".join(sorted(done)) + "\n")
        if filled["files"] and a.version in ("za202", "oot131072"):
            # The device indexes seed.vkpack and the loose files present when the game starts; a loose file sent while the
            # app had the old pack open was missed once. So the whole seed is packed again (the reference's
            # pack_shader_seed.py, in the container: it opens directories with O_DIRECTORY, which Windows Python lacks)
            # and sent as seed.vkpack, with the app stopped (it maps the pack it opened).
            pack = build / "seed.vkpack"
            cmd = ["docker", "run", "--rm", "-v", docker_path(UPSTREAM) + ":/android:ro", "-v", docker_path(REPO) + ":/repo", "python:3.14",
                   "python3", "/android/games/totk/scripts/pack_shader_seed.py", rel(seed), "--output", rel(pack)]
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env={**__import__("os").environ, "MSYS_NO_PATHCONV": "1"})
            print((r.stdout + r.stderr).strip()[-300:])
            if r.returncode:
                return 1
            hdc("shell", f"aa force-stop {BUNDLE}")
            print(hdc("file", "send", "-b", BUNDLE, str(pack), f"{FILES}/shaders/{a.version}/seed.vkpack").splitlines()[-1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
