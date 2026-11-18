#!/usr/bin/env python3
"""The Vulkan shader seed for the HarmonyOS port: build_seed.sh's seed, byte for byte the same seed.vkpack, made in a few
minutes instead of about an hour. build_seed.sh runs the reference's prepare_shader_seed.py (8 translators) and the
reflection and packing in one python:3.14 container, all of it writing ~190,000 files through Docker Desktop's bind mount
(about 50 a second). Here the same reference code runs in several python:3.14 containers that share one Docker volume
(native-harmonyos/tools/seed_in_container.py):

  probe   a throwaway container reports the CPUs and the free memory Docker has on this machine; from them: how many
          worker containers, how many translators each, how many processes read the RomFS (allocate())
  plan    one container: SPIRV-Cross (pinned, reused from <out>/spirv-cross when there), every shader of the RomFS
          (17 GB, read by several processes at once) and the modules, the translations prepare_shader_seed.py would do,
          cut into chunks of whole shaders, largest first
  work    the worker containers take chunks one at a time until none is left (a fast machine or container simply takes
          more), translate each with the reference's prepare() and reflect it (android/vk_reflect.py) on the volume
  pack    one container: checks every chunk and the totals, writes <out>/shaders/<version>/sb-align.txt, then seed.vkpack
          (the reference's pack_shader_seed.py; written last, it marks the step done) and <out>/seed-report.json
Only those files and <out>/spirv-cross/spirv-cross (fill_shaders_ohos.py uses it) reach the PC; the loose .spv/.vk/.vv
are in the pack (fill_shaders_ohos.py reads a shader's .vv rows from it). The volume is removed at the end.

usage: python native-harmonyos/build_seed.py [--romfs D] [--modules D] [--out D] [--xlate FILE] [--sb-align N]
                                            [--version v143] [--containers N] [--services N] [--keep-work] [--dry-run]
The defaults are prepare_v143.py's (its seed step calls this). --containers / --services (or TOTK_SEED_CONTAINERS /
TOTK_SEED_SERVICES) replace what the probe would choose."""
import argparse, os, shutil, subprocess, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ohos_env import UPSTREAM, PORT, docker_path

NA = Path(__file__).resolve().parent
IMAGE = "python:3.14"
VOLUME = "totk-seed-work"
NAME = "totk-seed"                 # container names: totk-seed-plan, totk-seed-w01 ..., totk-seed-pack
# memory each piece is given (2026-11-16, i9-13900HX, Docker 32 CPUs / 15.5 GiB: a worker with 4 translators peaked at
# 460 MiB, the plan with 12 readers and the SPIRV-Cross compile at 2.5 GiB); about twice what was measured
SERVICE_MIB = 240                  # one translator with its share of the worker's Python
READER_MIB = 400                   # one RomFS reader process (a package is decompressed whole)
RESERVE_MIB = 1536                 # left to Docker and the page cache
ENV = dict(os.environ, MSYS_NO_PATHCONV="1")


def say(*a): print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def allocate(cpus, free_mib, containers=None, services=None):
    """(containers, translators per container, RomFS readers) for a Docker with <cpus> CPUs and <free_mib> MiB free:
    one translator per CPU as far as memory allows, four to a container (each container's Python serves its translators
    one at a time; prepare_shader_seed runs at most 8), readers up to 12 (eight streams already read ~4x one)."""
    budget = max(0, free_mib - RESERVE_MIB)
    total = max(1, min(cpus, budget // SERVICE_MIB))
    if containers is None:
        containers = max(1, total // services if services else total // 4)
    if services is None:
        services = max(1, min(8, total // containers))
    readers = max(1, min(cpus, 12, budget // READER_MIB))
    return containers, services, readers


def probe():
    """Docker's CPUs and the memory free in its machine (Docker Desktop's VM), as a container sees them."""
    r = subprocess.run(["docker", "run", "--rm", IMAGE, "sh", "-c", "nproc; grep MemAvailable /proc/meminfo"],
                       capture_output=True, text=True, env=ENV)
    if r.returncode:
        sys.exit(f"docker run {IMAGE} failed: {(r.stdout + r.stderr).strip()[-500:]}")
    lines = r.stdout.split("\n")
    return int(lines[0]), int(lines[1].split()[1]) // 1024


def docker_run(name, mounts, env, cmd, dry):
    args = ["docker", "run", "--rm", "--name", name]
    for host, guest, ro in mounts:
        src = host if host == VOLUME else docker_path(host)
        args += ["-v", f"{src}:{guest}" + (":ro" if ro else "")]
    for k, v in env.items(): args += ["-e", f"{k}={v}"]
    args += [IMAGE] + cmd
    if dry:
        say("$", " ".join(args)); return None
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace", env=ENV)


def relay(proc, prefix, tail):
    """The container's output, line by line, with its name; the last lines kept for an error report. Read to the end
    whatever printing does: a container whose pipe is not read blocks, and its wait() with it."""
    for line in proc.stdout:
        line = line.rstrip()
        tail.append(line); del tail[:-40]
        try:
            print(f"{prefix} {line}", flush=True)
        except (OSError, ValueError):
            pass


def finish(proc, prefix):
    tail = []
    t = threading.Thread(target=relay, args=(proc, prefix, tail), daemon=True); t.start()
    proc.wait(); t.join()
    return proc.returncode, tail


def ours():
    """Every container of this script still there (an earlier run's too, whatever its container count was)."""
    r = subprocess.run(["docker", "ps", "-aq", "--filter", f"name={NAME}-"], capture_output=True, text=True, env=ENV)
    return r.stdout.split()


def cleanup(keep_volume):
    ids = ours()
    if ids:
        subprocess.run(["docker", "rm", "-f", *ids], capture_output=True, env=ENV)
    if not keep_volume:
        subprocess.run(["docker", "volume", "rm", "-f", VOLUME], capture_output=True, env=ENV)


def fresh_volume():
    """An empty work volume: an earlier run's (claims, reports, a failure mark) would be taken for this run's."""
    cleanup(False)
    if subprocess.run(["docker", "volume", "inspect", VOLUME], capture_output=True, env=ENV).returncode == 0:
        r = subprocess.run(["docker", "ps", "-a", "--filter", f"volume={VOLUME}", "--format", "{{.Names}}"],
                           capture_output=True, text=True, env=ENV)
        sys.exit(f"the Docker volume {VOLUME} is still in use by: {' '.join(r.stdout.split()) or '?'}; stop that "
                 f"(docker rm -f <name>) and run again")
    r = subprocess.run(["docker", "volume", "create", VOLUME], capture_output=True, text=True, env=ENV)
    if r.returncode: sys.exit(f"docker volume create {VOLUME}: {r.stderr.strip()}")


def positive(text):
    n = int(text)
    if n < 1: raise argparse.ArgumentTypeError("must be 1 or more")
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", type=Path, default=PORT / "prepared/totk-v143/extracted/v143/romfs", help="romfs.bin + index.json")
    ap.add_argument("--modules", type=Path, default=PORT / "prepared/harmonyos-v143/rewritten", help="the rewritten modules (*.patched.bin)")
    ap.add_argument("--out", type=Path, default=PORT / "prepared/harmonyos-v143/seed-build-sb256", help="the seed folder")
    ap.add_argument("--xlate", type=Path, default=PORT / "artifacts/xlate-ohos/totk_xlate", help="the shader translator")
    ap.add_argument("--sb-align", default="256", help="storage-buffer alignment the translator uses (sb-align.txt); '' for none")
    ap.add_argument("--version", default="v143")
    ap.add_argument("--upstream", type=Path, default=UPSTREAM, help="the totk-android checkout")
    ap.add_argument("--containers", type=positive, default=os.environ.get("TOTK_SEED_CONTAINERS") or None)
    ap.add_argument("--services", type=positive, default=os.environ.get("TOTK_SEED_SERVICES") or None)
    ap.add_argument("--keep-work", action="store_true", help=f"keep the Docker volume {VOLUME} (the chunks, reports, loose files)")
    ap.add_argument("--dry-run", action="store_true", help="print the containers it would run")
    a = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):   # piped (NX2Harmony, a log) under -I the console code page: never fail on a character
        if hasattr(stream, "reconfigure"): stream.reconfigure(errors="replace")
    for p, what in ((a.romfs / "romfs.bin", "RomFS"), (a.romfs / "index.json", "RomFS index"), (a.xlate, "translator"),
                    (a.upstream / "games/totk/scripts/prepare_shader_seed.py", "totk-android checkout")):
        if not p.is_file() and not a.dry_run: sys.exit(f"{what} missing: {p}")
    if not shutil.which("docker") and not a.dry_run: sys.exit("docker is not on PATH")
    t0 = time.monotonic()

    if a.dry_run:
        cpus, free = os.cpu_count() or 4, 8192
    else:
        cpus, free = probe()
    containers, services, readers = allocate(cpus, free, a.containers, a.services)
    say(f"Docker: {cpus} CPUs, {free / 1024:.1f} GiB free -> {containers} containers x {services} translators, "
        f"RomFS read by {readers} processes")

    a.out.mkdir(parents=True, exist_ok=True)
    android, tools = (a.upstream, "/android", True), (NA / "tools", "/tools", True)
    work, out = (VOLUME, "/work", False), (a.out, "/out", False)
    env = {"SEED_VERSION": a.version}
    xenv = dict(env, **({"TOTK_XLATE_SB_ALIGN": a.sb_align} if a.sb_align else {}))   # build_seed.sh exports it for the translator
    script = "/tools/seed_in_container.py"
    names = [f"{NAME}-plan", f"{NAME}-pack"] + [f"{NAME}-w{i + 1:02d}" for i in range(containers)]
    if not a.dry_run:
        fresh_volume()   # also removes what an interrupted run left
    try:
        # plan
        p = docker_run(f"{NAME}-plan", [android, tools, work, out, (a.romfs, "/in/romfs", True), (a.modules, "/in/modules", True),
                                        (a.xlate.parent, "/in/xlate", True)],
                       dict(env, SEED_XLATE=f"/in/xlate/{a.xlate.name}"), ["python3", script, "plan", str(containers), str(readers)], a.dry_run)
        if p:
            code, tail = finish(p, "[plan]")
            if code: sys.exit(f"seed plan failed ({code})")
        # work: every container takes chunks until none is left
        t1 = time.monotonic()
        procs = [docker_run(f"{NAME}-w{i + 1:02d}", [android, tools, work], xenv,
                            ["nice", "-n", "10", "python3", script, "work", str(services)], a.dry_run) for i in range(containers)]
        if not a.dry_run:
            results, first = [None] * containers, []
            def wait(i):
                results[i] = finish(procs[i], f"[w{i + 1:02d}]")
                if results[i][0] and not first:   # the others would stop after their current chunk (/work/failed)
                    first.append(i)
                    subprocess.run(["docker", "kill", *[n for n in names if "-w" in n]], capture_output=True, env=ENV)
            threads = [threading.Thread(target=wait, args=(i,)) for i in range(containers)]
            for t in threads: t.start()
            for t in threads: t.join()
            if first:
                i = first[0]
                print(f"--- w{i + 1:02d} exited {results[i][0]}:\n" + "\n".join(results[i][1][-25:]), flush=True)
                sys.exit(f"seed translation failed (w{i + 1:02d}; the other containers were stopped)")
            say(f"translated and reflected in {time.monotonic() - t1:.0f} s")
        # pack
        p = docker_run(f"{NAME}-pack", [android, tools, work, out], env,
                       ["python3", script, "pack", a.version] + ([a.sb_align] if a.sb_align else []), a.dry_run)
        if p:
            code, tail = finish(p, "[pack]")
            if code: sys.exit(f"seed pack failed ({code})")
            say(f"seed: {a.out / 'shaders' / a.version / 'seed.vkpack'} in {time.monotonic() - t0:.0f} s")
    finally:
        if not a.dry_run:
            cleanup(a.keep_work)
            if a.keep_work: say(f"kept the Docker volume {VOLUME} (docker volume rm {VOLUME})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
