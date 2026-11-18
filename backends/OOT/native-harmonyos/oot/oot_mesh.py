"""Ocarina of Time's NVN2 task and mesh shaders as compute kernels: step 1 of the mesh emulation (docs/DESIGN-OOT.md,
"网格 / 任务着色器在 Maleoon 上的做法"). Maleoon has no VK_EXT_mesh_shader, so a mesh draw becomes compute dispatches and an
indexed draw. The translator (nxnative's Ryujinx, prepare_xlate_ns2.py) already reads a task or mesh shader as a compute
shader with its SPH taken off and its stage memory in shared memory, when the job gives the stage memory's layout
(stage_memory: the workgroup's number, the input, the outputs, a mesh shader's primitive bytes).

This script:
  1. reads every task ('ts') and mesh ('ms') record of the package (prepared/oot/shader-split/pkg, as oot_seed.py);
  2. takes each one's facts and stage memory layout from seedds/nxnative's dm/gfx/tools/dmsh_meshstage.py (MIT; info(),
     memory(): threads, payload bytes, most vertices and primitives, the components its SPHs enable);
  3. translates the kernels as compute (arch sm86, the code without its 0x80-byte SPH, local [threads, 1, 1], shared the
     stage memory's size) into prepared/oot/mesh-build/shaders/oot131072, and validates them (tools/spirv_val_seed.sh);
  4. writes prepared/oot/mesh-build/mesh-info.json: per key the facts the wrapper and the runtime need.
  5. --wrap: tools/mesh_wrap_in_container.py makes each kernel the compute shader the replay dispatches (the stage memory
     cleared, the workgroup's number, a task's payload and launches, a mesh shader's vertices and indices written to the
     draw's buffers) and gives each mesh kernel its pass-through vertex shader, into prepared/oot/mesh-build/wrapped;
  6. --install: copies the wrapped modules into the device seed's folder (prepared/oot/seed-build/shaders/oot131072);
     oot_seed.py --pack --send then packs and sends the seed.
The replay side is overlay android/gfx/vk_draw.c mesh_draw; tools/mesh_selftest checks the wrapper on lavapipe.

usage: python native-harmonyos/oot/oot_mesh.py [--jobs 8] [--sample N] [--wrap] [--install]
Everything it writes is game-derived and stays under prepared/ (ignored)."""
import argparse, json, os, shutil, struct, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent)); sys.path.insert(0, str(HERE))
from ohos_env import UPSTREAM, REPO, docker_path   # noqa: E402
import oot_seed   # noqa: E402  (records, nvn2_key, SUFFIX, ANSWERS, POOL_ENV, DRAW_ENV)

NXNATIVE = Path(os.environ.get('NXNATIVE') or PORT.parent / 'nxnative')   # NX2Harmony sets it; else backends/nxnative
sys.path.insert(0, str(NXNATIVE / 'dm' / 'gfx' / 'tools'))
import dmsh_meshstage as meshstage   # noqa: E402

BUILD = PORT / 'prepared' / 'oot' / 'mesh-build'
SEED = BUILD / 'shaders' / 'oot131072'
XLATE = Path(os.environ['OOT_XLATE']) if os.environ.get('OOT_XLATE') else PORT / 'artifacts' / 'xlate-ohos-ns2' / 'totk_xlate'


def request(key, ctl, code, info, lay, size, answers=None):
    inst = struct.unpack_from('<I', ctl, 0x38)[0]
    a, e = struct.unpack_from('<II', ctl, 0x40)
    answers = answers or oot_seed.ANSWERS
    return dict(variant=f'{key}-{oot_seed.pss.suffix(answers)}', stage='cs', arch='sm86', depth_minus_w=True, early_fragment_tests=False,
                c1=code[a:e].hex() if a <= e <= len(code) else '', code_hex=code[0x80:inst].hex(),   # the SPH taken off
                local=[info['threads'], 1, 1], shared=size, stage_memory=lay, **answers)


# the replay's names of a binding's target (overlay android/gfx/vk_shader.c target_name)
TARGETS = {0: '1d', 2: '3d', 3: '1darray', 4: '2darray', 5: '2dms', 6: '2dmsarray', 8: 'cube', 9: 'cubearray', 10: 'buffer'}


def texture_variants(shaders):
    """Requests for the kernels whose one variant cannot match what the game binds. A kernel whose code asks a texture's
    kind (a size query) is translated with the default answer, 2d, and its variant list then depends on it (tex:N=2d), while
    the kernel samples slot N as a 2D array (its reflection's target): the game binds an array there, no row matched, and
    the foliage's light probes' draws were skipped (oot-a70: 066f5daf12e43c71, 3500af98dd8372cb, 2 draws a frame). The
    variant translated with the kind the kernel itself samples is added."""
    out = []
    for key, (ctl, code, info, lay, size) in sorted(shaders.items()):
        vv, vk = SEED / f'{key}.vv', SEED / f'{key}-{oot_seed.SUFFIX}.vk'
        if not vv.is_file() or not vk.is_file():
            continue
        rows = [r.split('\t') for r in vv.read_text().splitlines() if r]
        base = next((r[1] if len(r) > 1 else '' for r in rows if r[0] == f'{key}-{oot_seed.SUFFIX}'), None)
        if not base:
            continue
        targets = {int(b[5]): int(b[9]) for b in (l.split() for l in vk.read_text().splitlines() if l.startswith('b ')) if int(b[4]) in (9, 13)}
        tex = {}
        for dep in base.split(','):
            kind, _, rest = dep.partition(':')
            slot, _, value = rest.partition('=')
            if kind == 'tex' and value == '2d' and targets.get(int(slot), 1) in TARGETS:
                tex[slot] = TARGETS[targets[int(slot)]]
        if not tex:
            continue
        answers = dict(oot_seed.ANSWERS, tex=tex)
        if not (SEED / f'{key}-{oot_seed.pss.suffix(answers)}.spv').is_file():
            out.append((key, (ctl, code, info, lay, size), answers))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--sample', type=int, default=0, help='only the first N of each kind (a check)')
    ap.add_argument('--wrap', action='store_true', help='wrap the kernels for the replay (step 5)')
    ap.add_argument('--install', action='store_true', help='copy the wrapped modules into the device seed folder (step 6)')
    a = ap.parse_args()
    shaders, infos, refused = {}, {}, {}
    for stage, ctl, code in oot_seed.records():
        if stage not in ('ts', 'ms'):
            continue
        key = oot_seed.nvn2_key(ctl, code)
        info = meshstage.info(ctl, code)
        lay, size = meshstage.memory(info)
        inst = struct.unpack_from('<I', ctl, 0x38)[0]
        if meshstage.own_shared(code[0x80:inst]):
            refused[key] = 'shared memory of its own'
            continue
        shaders[key] = (ctl, code, info, lay, size)
        infos[key] = dict(info, stage_memory=lay, shared=size,
                          vertex=[[n if n is not None else 'pos', c] for n, c in info.get('vertex', [])],
                          primitive=[[n, c] for n, c in info.get('primitive', [])])
    kinds = {k: sum(1 for v in shaders.values() if v[2]['kind'] == k) for k in ('ts', 'ms')}
    print(f'package: {kinds["ts"]} task, {kinds["ms"]} mesh shaders; refused {len(refused)}')
    BUILD.mkdir(parents=True, exist_ok=True); SEED.mkdir(parents=True, exist_ok=True)
    (BUILD / 'mesh-info.json').write_text(json.dumps(infos, indent=0))
    if not (SEED / 'sb-align.txt').is_file():
        (SEED / 'sb-align.txt').write_text('256\n')
    todo = [(k, v, None) for k, v in sorted(shaders.items()) if not (SEED / f'{k}-{oot_seed.SUFFIX}.spv').is_file()]
    if a.sample:
        per = {}
        todo = [t for t in todo if len(per.setdefault(t[1][2]['kind'], [])) < a.sample and not per[t[1][2]['kind']].append(t)]
    print(f'{len(todo)} to translate ({len(shaders) - len(todo)} already there)')
    for todo in (todo, None):   # the second round: the variants for the texture kinds the kernels sample (texture_variants)
        if todo is None:
            todo = texture_variants(shaders)
            if todo:
                print(f'{len(todo)} more variants for the texture kinds the kernels sample: ' + ' '.join(k[:16] for k, _, _ in todo))
        if not todo:
            continue
        reqs = BUILD / 'requests'
        if reqs.exists():
            shutil.rmtree(reqs)
        jobs = min(a.jobs, len(todo))
        shards = [reqs / f'shard{i:02d}' for i in range(jobs)]
        for s in shards:
            (s / 'in').mkdir(parents=True)
        for i, (key, (ctl, code, info, lay, size), answers) in enumerate(todo):
            rq = request(key, ctl, code, info, lay, size, answers)
            (shards[i % jobs] / 'in' / f'{rq["variant"]}.json').write_text(json.dumps(rq))
        sc = PORT / 'prepared' / 'oot' / 'seed-build' / 'spirv-cross' / 'spirv-cross'
        rel = lambda p: '/repo/' + p.resolve().relative_to(REPO).as_posix()
        env = {**os.environ, 'MSYS_NO_PATHCONV': '1'}
        t0 = time.monotonic(); procs = []
        for s in shards:
            cmd = ['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo',
                   '-e', 'TOTK_XLATE_SB_ALIGN=256', '-e', 'TOTK_VK_DEPS=queried', *oot_seed.POOL_ENV, *oot_seed.DRAW_ENV, 'python:3.14', 'python3',
                   '/repo/native-harmonyos/tools/fill_xlate_in_container.py', rel(s / 'in'), rel(SEED), rel(s / 'out'), rel(XLATE), rel(sc)]
            procs.append((s, subprocess.Popen(cmd, stdout=(s / 'log.txt').open('w'), stderr=subprocess.STDOUT, env=env)))
        done = failed = 0; errors = {}
        for s, p in procs:
            p.wait()
            f = s / 'out' / 'filled.json'
            if p.returncode or not f.is_file():
                print(f'{s.name}: the container failed ({p.returncode}); see {s / "log.txt"}'); continue
            r = json.loads(f.read_text()); done += len(r['done']); failed += len(r['failed'])
            for n, e in r['failed']:
                errors.setdefault((e or '?')[:110], []).append(n)
        print(f'translated {done} kernels, {failed} failed, in {time.monotonic() - t0:.0f} s')
        for e, names in sorted(errors.items(), key=lambda kv: -len(kv[1]))[:12]:
            print(f'  {len(names):4d}  {e}   e.g. {names[0][:16]}')
        (BUILD / 'failures.json').write_text(json.dumps(errors, indent=1))
    r = subprocess.run(['docker', 'run', '--rm', '-v', docker_path(REPO) + ':/repo', 'nativegames-tools:1', 'sh',
                        '/repo/native-harmonyos/tools/spirv_val_seed.sh', '/repo/' + SEED.resolve().relative_to(REPO).as_posix()],
                       capture_output=True, text=True, errors='replace', env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
    print('spirv-val:', (r.stdout + r.stderr).strip()[-300:])
    wrapped = BUILD / 'wrapped'
    rel = lambda q: '/repo/' + q.resolve().relative_to(REPO).as_posix()
    if a.wrap:
        if wrapped.exists():
            shutil.rmtree(wrapped)
        sc = PORT / 'prepared' / 'oot' / 'seed-build' / 'spirv-cross' / 'spirv-cross'
        r = subprocess.run(['docker', 'run', '--rm', '-v', docker_path(REPO) + ':/repo', 'nativegames-tools:1', 'python3',
                            '/repo/native-harmonyos/tools/mesh_wrap_in_container.py', rel(SEED), rel(wrapped), rel(BUILD / 'mesh-info.json'), rel(sc), str(a.jobs)],
                           capture_output=True, text=True, errors='replace', env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
        print((r.stdout + r.stderr).strip()[-1200:])
        if r.returncode:
            return 1
    if a.install:
        main_seed = PORT / 'prepared' / 'oot' / 'seed-build' / 'shaders' / 'oot131072'
        files = [f for f in wrapped.iterdir() if f.suffix in ('.spv', '.vk', '.vv')] if wrapped.is_dir() else []
        if not files or not main_seed.is_dir():
            print('nothing to install: run --wrap first (and oot_seed.py for the seed folder)'); return 1
        for f in files:
            shutil.copy2(f, main_seed / f.name)
        print(f'installed {len(files)} files of {sum(1 for f in files if f.suffix == ".spv")} modules into {main_seed}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
