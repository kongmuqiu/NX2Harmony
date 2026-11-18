"""Fill shader variants the device met but the seed lacks: the HarmonyOS counterpart of the reference's
android/fill_shaders.py. A variant the seed lacks skips its draws and leaves its request in files/android/shader-requests/
(android/gfx/vk_shader.c). This pulls the requests with hdc, translates them in the python:3.14 container with the
HarmonyOS translator (artifacts/xlate-ohos/totk_xlate, the seed's storage-buffer alignment from its sb-align.txt) and the
reference's reflection (tools/fill_xlate_in_container.py), adds them to the local seed folder, and sends the new files
into the device's shaders/<version>, where the next start draws with them. The app's sandbox does not let hdc move the
requests to done/, so the ones filled are remembered in <seed>/filled-requests.txt and skipped next time.
usage: python fill_shaders_ohos.py [--version za202|v143]
"""
import argparse, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
from ohos_env import HDC, DEV, UPSTREAM, REPO, docker_path   # machine values: environment or native-harmonyos/local.ohos.env
import upstream; upstream.require()
BUNDLE = os.environ.get("TOTK_OHOS_BUNDLE") or "LZA.NativeGames.harmonyos"
FILES = "/data/storage/el2/base/haps/entry/files/android"


def hdc(*args):
    r = subprocess.run([HDC, *DEV, *args], capture_output=True, text=True, errors="replace")
    return (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="za202", choices=("za202", "v143"))
    a = ap.parse_args()
    build = PORT / "prepared" / "za" / "seed-build" if a.version == "za202" else PORT / "prepared" / f"harmonyos-{a.version}" / "seed-build-sb256"
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
               "-e", f"TOTK_XLATE_SB_ALIGN={align}", *(["-e", "TOTK_VK_DEPS=queried", "-e", "TOTK_XLATE_DRAW_PARAMS=1"] if a.version == "za202" else []), "python:3.14", "python3",
               "/repo/native-harmonyos/tools/fill_xlate_in_container.py", rel(new), rel(seed), rel(out),
               "/repo/artifacts/xlate-ohos/totk_xlate", rel(build / "spirv-cross" / "spirv-cross")]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env={**__import__("os").environ, "MSYS_NO_PATHCONV": "1"})
        print(r.stdout.strip()[-2000:], r.stderr.strip()[-2000:])
        if r.returncode:
            return 1
        filled = json.loads((out / "filled.json").read_text())
        for f in filled["files"]:
            res = hdc("file", "send", "-b", BUNDLE, str(out / f), f"{FILES}/shaders/{a.version}/{f}")
            if "finish" not in res:
                print("send failed:", f, res[-200:])
        done.update(filled["done"])
        done_list.write_text("\n".join(sorted(done)) + "\n")
        print(f"sent {len(filled['files'])} files for {len(filled['done'])} variants; {len(filled['failed'])} failed")
        if filled["files"] and a.version == "za202":
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
