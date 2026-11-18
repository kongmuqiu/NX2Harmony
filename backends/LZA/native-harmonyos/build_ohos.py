# Build the Android port's native stack (the totk-android checkout named by TOTK_UPSTREAM, games/totk) for HarmonyOS with the OHOS SDK clang
# (aarch64-linux-ohos, musl). Mirrors games/totk/android/Makefile (the same source lists, parsed from it, and flags):
#   1. stage: the reference sources (runtime/, third_party/d2r_gfx/, android/gfx/, android/compat/) are copied to
#      <out>/src/games/totk, then native-harmonyos/overlay/ on top: the HarmonyOS changes are whole files there, so
#      `git diff --no-index <reference file> <overlay file>` shows each one. Unchanged files keep their time stamps.
#   2. compile: incremental (clang -MD dependency files plus a flags stamp), as many jobs as the machine has free now
#      (loadsched.py), below normal priority.
#   3. link: libtotk_ohos.so (runtime + NVN layer + backend + the HarmonyOS frontend, native-harmonyos/ohos/) and, with
#      --guest <embed folder> (gen_embed_ohos.py), libtotk_guest.so with the rewritten modules at their module offsets.
# usage: python build_ohos.py [--gpu null|vk] [--guest <embed dir>] [--only substring] [--no-link]
import argparse, hashlib, json, os, re, shutil, subprocess, sys, time, concurrent.futures as cf
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import loadsched  # noqa: E402

from ohos_env import UPSTREAM, SDK   # TOTK_UPSTREAM / TOTK_OHOS_SDK, or native-harmonyos/local.ohos.env
import upstream; upstream.require()   # the overlay matches one commit of the Android port (UPSTREAM_PIN)
REF = UPSTREAM / "games" / "totk"
CC = SDK / "llvm" / "bin" / "clang.exe"
CXX = SDK / "llvm" / "bin" / "clang++.exe"
LIBCXX = [str(SDK / "llvm" / "lib" / "aarch64-linux-ohos" / "libc++.a"), str(SDK / "llvm" / "lib" / "aarch64-linux-ohos" / "libc++abi.a")]
SYSROOT = SDK / "sysroot"
PORT = HERE.parent
OPUS_INC = PORT / "artifacts/native-opus/source/opus-1.6.1/include"
OPUS_LIB = PORT / "artifacts/native-opus/build-ohos/opus/libopus.a"
VPX = PORT / "artifacts/native-vpx/ohos"
STAGE_DIRS = ("runtime", "third_party/d2r_gfx", "android/gfx", "android/compat", "android/libtotk.map")

ap = argparse.ArgumentParser()
ap.add_argument("--gpu", default="null", choices=("null", "vk"))
ap.add_argument("--version", default="za202", choices=("za202",))   # Pokémon Legends: Z-A 2.0.2 only
ap.add_argument("--guest", type=Path, help="gen_embed_ohos.py output: also link libtotk_guest.so")
ap.add_argument("--only", default="")
ap.add_argument("--no-link", action="store_true")
ap.add_argument("--out", type=Path, help="default artifacts/harmonyos-native")
a = ap.parse_args()
OUT = a.out or PORT / "artifacts" / "harmonyos-native"
SRC = OUT / "src" / "games" / "totk"
OBJ = OUT / f"obj-{a.gpu}"
LIB = OUT / f"lib-{a.gpu}"
t0 = time.time()


# ---- 1. stage ----
def sync_file(src, dst):
    if dst.exists() and dst.stat().st_size == src.stat().st_size and dst.read_bytes() == src.read_bytes():
        return 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return 1


changed = 0
overlay = HERE / "overlay"
overlaid = {p.relative_to(overlay).as_posix() for p in overlay.rglob("*") if p.is_file()}
for rel in STAGE_DIRS:
    base = REF / rel
    files = [base] if base.is_file() else [p for p in base.rglob("*") if p.is_file()]
    for p in files:
        r = p.relative_to(REF).as_posix()
        changed += sync_file(overlay / r if r in overlaid else p, SRC / r)
for r in overlaid:   # files new in the overlay (no reference counterpart)
    if not (REF / r).exists():
        changed += sync_file(overlay / r, SRC / r)
print(f"stage: {changed} files updated, {len(overlaid)} overlaid", flush=True)

# ---- source lists (games/totk/android/Makefile) ----
mk = (REF / "android" / "Makefile").read_text(encoding="utf-8").replace("\\\n", " ")
def mk_list(name):
    m = re.search(r"^%s :=(.*)$" % re.escape(name), mk, re.M)
    return m.group(1).split()
patches = "runtime/game_patches_za.c"   # overlay/runtime: the Z-A game-patch file
_rt = mk_list("RUNTIME_C")
_rt = _rt[:next(i for i, t in enumerate(_rt) if t.startswith("$(if"))]   # the APP=1 / headless frontend choice ends the list
# The window build (vk) is the app build: TOTK_ANDROID_APP (the presenter's window code in android/gfx/vk_draw.c) with
# the HarmonyOS window frontend (ohos/frontend_ohos.c, which includes runtime/frontend_headless.c); the null build is
# the headless one.
APP = a.gpu == "vk"
RUNTIME_C = [t.replace("$(PATCHES)", patches) for t in _rt] + ([] if APP else ["runtime/frontend_headless.c"])
# Z-A only (overlay/runtime, no reference counterpart): the AudioOut server Wwise sends its final mix to
RUNTIME_C += ["runtime/audio_out_ohos.c", "runtime/za_hle.c", "runtime/za_shader_dump.c"]   # and the SDK takeovers of za_hle.c
RUNTIME_S = mk_list("RUNTIME_S")
if a.gpu == "vk":
    vk = re.search(r"ifeq \(\$\(GPU\),vk\)(.*?)else ifeq", mk, re.S).group(1)
    BACKEND_C = [t.replace("$(TOTK)/", "") for t in vk.split() if t.endswith(".c")]
    BACKEND_DEF = ['-DGPU_LOG_PREFIX="[nvn-vk] "', "-DGPU_VK"]
else:
    BACKEND_C, BACKEND_DEF = ["android/gfx/nvn_null.c"], []
GFX_C = sorted(p.relative_to(SRC).as_posix() for d in ("third_party/d2r_gfx/gfx/nvn", "third_party/d2r_gfx/gfx/tex")
               for p in (SRC / d).glob("*.c") if p.name not in ("fps_overlay.c", "totk_sysload.c"))
GFX_C += BACKEND_C + ["android/gfx/nvn_stubs.c"]
GFX_S = ["third_party/d2r_gfx/gfx/nvn/nvn_thunks.S", "third_party/d2r_gfx/gfx/nvn/nvn_entry.S"]
OHOS_C = sorted(p for p in (HERE / "ohos").glob("*.c") if APP or not p.name.startswith("frontend_"))
# C++: the etcpak ETC2 encoder (third_party/etcpak, BSD-3) and its C entry (ohos/etc_encode.cpp); libc++ linked statically
OHOS_CXX = [HERE / "ohos" / "etc_encode.cpp"] + sorted((HERE / "third_party" / "etcpak").glob("*.cpp"))
CXXFLAGS = ["--target=aarch64-linux-ohos", "--sysroot=" + str(SYSROOT), "-O3", "-g", "-fPIC", "-std=c++20", "-fno-exceptions", "-fno-rtti", "-DNDEBUG",
            "-fno-omit-frame-pointer", "-ffile-prefix-map=%s/=" % HERE.as_posix(), "-Wno-everything"]

# 1.4.3 without TOTK_PINNED: the Android build pins its own rewrite's hashes (totk_pins.h); this rewrite differs (musl TLS),
# and runtime/loader.c (TOTK_OHOS) already refuses tables made for another rewrite than the one in libtotk_guest.so.
VDEF = ["-DTOTK_GAME_ZA=1"]   # runtime/game_version.h (overlay): the Z-A block
# -fno-emulated-tls: the OHOS target defaults to emulated TLS (__emutls, heap blocks); the thread slots must be real ELF
# TLS at a fixed TP offset (overlay/runtime/host_tp.c), and native TLS is faster for every other __thread too.
COMMON = ["--target=aarch64-linux-ohos", "--sysroot=" + str(SYSROOT), "-O2", "-g", "-fPIC", "-fno-emulated-tls", "-Dmain=totk_host_main",
          "-fno-omit-frame-pointer", "-D_GNU_SOURCE", "-DTOTK_DEV_BUILD=1", "-DTOTK_OHOS=1", *VDEF,
          "-ffile-prefix-map=%s/=" % (OUT / "src").as_posix(), "-I" + str(OUT), "-I" + str(SRC / "runtime"),
          "-I" + str(SRC / "third_party/d2r_gfx/gfx/nvn"), "-I" + str(SRC / "third_party/d2r_gfx/gfx/nvn/include"),
          "-DTOTK_PSO_COLLECT_DEFAULT=0", "-I" + str(HERE / "compat"), "-I" + str(OPUS_INC), "-I" + str(VPX / "include"), "-I" + str(HERE / "ohos" / "shaders"),
          *(["-DTOTK_ANDROID_APP"] if APP else [])]
WARN = ["-include", str(HERE / "compat" / "ohos_compat.h"), "-std=gnu11", "-Wall", "-Wno-format",
        "-Wno-misleading-indentation", "-Wno-unused-function"]
COMPAT = ["-I" + str(SRC / "android/compat/include"), "-include", str(SRC / "android/compat/include/darwin_compat.h"),
          "-I" + str(SRC / "third_party/d2r_gfx/host")]

jobs = []
for s in RUNTIME_C: jobs.append((SRC / s, WARN + COMMON))
for s in RUNTIME_S: jobs.append((SRC / s, COMMON))
for s in GFX_C: jobs.append((SRC / s, WARN + ["-Wno-unused-parameter"] + COMMON + COMPAT + BACKEND_DEF))
for s in GFX_S: jobs.append((SRC / s, COMMON))
for s in OHOS_C: jobs.append((s, WARN + COMMON + ["-I" + str(SYSROOT / "usr/include")]))
for s in OHOS_CXX: jobs.append((s, CXXFLAGS))
if a.only: jobs = [j for j in jobs if a.only in str(j[0])]


# ---- 2. compile ----
def obj_for(src):
    rel = src.relative_to(SRC).as_posix() if SRC in src.parents else ("etcpak/" if "etcpak" in src.parts else "ohos/") + src.name
    return OBJ / (rel.replace("/", "_") + ".o")


def up_to_date(src, out, stamp, flags_hash):
    dep = out.with_suffix(".d")
    if not (out.exists() and dep.exists() and stamp.exists() and stamp.read_text() == flags_hash):
        return False
    t = out.stat().st_mtime
    text = dep.read_text(errors="replace").replace("\\\n", " ")
    head = re.match(r"^.*?\.o:\s", text, re.S)   # "<obj>.o: " (a Windows path has its own drive colon)
    if not head:
        return False
    deps = re.split(r"(?<!\\) ", text[head.end():])
    for d in deps:
        d = d.strip().replace("\\ ", " ")
        if d and (not os.path.exists(d) or os.path.getmtime(d) > t):
            return False
    return True


def compile_one(job):
    src, flags = job
    out = obj_for(src)
    stamp = out.with_suffix(".flags")
    fh = hashlib.sha256("\0".join(flags).encode()).hexdigest()
    if up_to_date(src, out, stamp, fh):
        return src, 0, [], False
    cmd = [str(CXX if src.suffix == ".cpp" else CC), *flags, "-MD", "-MF", str(out.with_suffix(".d")), "-c", str(src), "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", **loadsched.low_priority())
    if r.returncode == 0:
        stamp.write_text(fh)
    errors = [l for l in (r.stdout + r.stderr).splitlines() if " error:" in l or "fatal error" in l]
    return src, r.returncode, errors, True


OBJ.mkdir(parents=True, exist_ok=True)
n_jobs, why = loadsched.pick_jobs()
print("compile:", why, flush=True)
results = []
with cf.ThreadPoolExecutor(max_workers=n_jobs) as pool:
    for src, rc, errors, built in pool.map(compile_one, jobs):
        results.append({"source": str(src), "rc": rc, "built": built, "errors": errors[:12]})
failed = [r for r in results if r["rc"]]
(OUT / f"compile-{a.gpu}.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
print("compiled %d (%d rebuilt), failed %d, %.0fs" % (len(results) - len(failed), sum(r["built"] for r in results),
                                                     len(failed), time.time() - t0), flush=True)
for r in failed:
    print("FAIL", r["source"])
    for e in r["errors"][:4]:
        print("   ", e[:260])
if failed or a.no_link or a.only:
    sys.exit(1 if failed else 0)


# ---- 3. link ----
def link(out, args):
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CC), "--target=aarch64-linux-ohos", "--sysroot=" + str(SYSROOT), "-shared", "-fuse-ld=lld", *args, "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", **loadsched.low_priority())
    lines = (r.stdout + r.stderr).splitlines()
    undefined = sorted({m.group(1) for l in lines for m in [re.search(r"undefined symbol: (\S+)", l)] if m})
    print(f"link {out.name}: rc={r.returncode}" + (f", {len(undefined)} undefined" if undefined else ""), flush=True)
    if r.returncode:
        for l in (undefined or lines)[:40]:
            print("   ", l[:260])
        sys.exit(1)


objs = [str(obj_for(s)) for s, _ in jobs]
link(LIB / "libtotk_ohos.so", ["-Wl,--build-id=sha1", "-Wl,--version-script=" + str(HERE / "libtotk_ohos.map"),
                               "-Wl,--no-undefined", "-Wl,-soname,libtotk_ohos.so", "-Wl,-z,max-page-size=16384",
                               *objs, str(OPUS_LIB), str(VPX / "lib/libvpx.a"), *LIBCXX, "-lace_napi.z", "-lhilog_ndk.z", "-lohaudio", "-lnative_window", "-lnative_buffer", "-lqos", "-lnative_vsync", "-lohsensor", "-lohvibrator.z", "-lnative_display_manager", "-lbundle_ndk.z", "-lm"])
if a.guest:
    g = a.guest.resolve()
    gobj = OBJ / "guest"
    gobj.mkdir(exist_ok=True)
    for name in ("guest_images.S", "guest_modules.c"):
        cmd = [str(CC), "--target=aarch64-linux-ohos", "--sysroot=" + str(SYSROOT), "-fPIC", "-O2", "-c", str(g / name),
               "-o", str(gobj / (name + ".o"))]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", **loadsched.low_priority())
        if r.returncode:
            sys.exit(f"{name}: {r.stderr[:2000]}")
    link(LIB / "libtotk_guest.so", ["-Wl,-T," + str(g / "guest_layout.ld"), "-Wl,-z,max-page-size=16384", "-Wl,--no-undefined",
                                    "-Wl,-soname,libtotk_guest.so", "-nostdlib", str(gobj / "guest_images.S.o"),
                                    str(gobj / "guest_modules.c.o")])
print("done %.0fs" % (time.time() - t0))
