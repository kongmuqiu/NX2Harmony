"""Rebuild the Z-A seed with Vulkan-only variant dependencies, and prove it offline before it goes to the device.

Why: the seed's .vv rows listed every sampled texture slot as a dependency (Service.cs keys them for Metal, where a depth
texture changes the declaration), so another kind of texture in a slot (cube, array, depth, 3D) was a miss: the draw was
skipped and the PC filled a "variant" whose SPIR-V and reflection were byte-identical to the one in the seed (217 of the
first 220 shaders filled). With TOTK_VK_DEPS=queried (tools/fill_xlate_in_container.py) a texture slot is a dependency
only where the translator asked its type (texture buffers, size / sample-count queries).

  1. the device's shader requests (files/android/shader-requests: every miss so far, with its code and bind state) are
     pulled; the default requests come from za_seed.py's shards (prepared/za/shader-dump/requests/shard*/in)
  2. pass A: every default request -> <seed>.new (one row per shader); pass B: the device's requests, plus attr 12 = uint
     (Trinity's bone indices, read by the transform feedback skinning shaders) for every vertex shader that reads
     attribute 12; a row whose dependencies another row of the shader already has is not added
  3. checks: every variant in both seeds has byte-identical .spv and .vk; every device request matches a row of the new
     seed by the runtime's rule (vk_shader.c dep_ok / deps_match), and the SPIR-V it gets is the SPIR-V that request was
     filled with
  4. --swap: the old seed becomes za202.old, the new one za202; --send then packs it (in the container), sends
     seed.vkpack and, over the loose files earlier fills left (they win over the pack), the new .vv of every shader the
     device requested
usage: python native-harmonyos/za/za_reseed.py [--jobs 16] [--check-only] [--swap [--send]]
Everything it writes is game-derived and stays under prepared/ (ignored)."""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from ohos_env import HDC, DEV, UPSTREAM, REPO, docker_path   # noqa: E402
sys.path.insert(0, str(UPSTREAM / 'games' / 'totk' / 'scripts'))
import prepare_shader_seed as pss   # noqa: E402

BUNDLE = os.environ.get('TOTK_OHOS_BUNDLE') or 'LZA.NativeGames.harmonyos'
FILES = '/data/storage/el2/base/haps/entry/files/android'
DUMP = PORT / 'prepared' / 'za' / 'shader-dump'
BUILD = PORT / 'prepared' / 'za' / 'seed-build'
OLD = BUILD / 'shaders' / 'za202'
NEW = BUILD / 'shaders' / 'za202.new'
WORK = PORT / 'prepared' / 'za' / 'reseed'
DEFAULT = pss.suffix(dict(attr={}, fout={}, img={}, tex={}))


def hdc(*args):
    r = subprocess.run([HDC, *DEV, *args], capture_output=True, text=True, errors='replace')
    return (r.stdout + r.stderr).strip()


XLATE = 'artifacts/xlate-ohos/totk_xlate'   # --xlate
XLATE_ENV = []                               # --draw-params


def translate(name, reqs, jobs):
    """reqs: [(key, request)]; the requests of one shader stay in one container (its .vv is read and written there)."""
    root = WORK / name
    if root.exists():
        shutil.rmtree(root)
    by_key = {}
    for key, rq in reqs:
        by_key.setdefault(key, []).append(rq)
    shards = [root / f'shard{i:02d}' for i in range(min(jobs, len(by_key)))]
    for s in shards:
        (s / 'in').mkdir(parents=True)
    for i, key in enumerate(sorted(by_key)):
        for rq in by_key[key]:
            (shards[i % len(shards)] / 'in' / f'{rq["variant"]}.json').write_text(json.dumps(rq))
    rel = lambda p: '/repo/' + p.resolve().relative_to(REPO).as_posix()
    env = {**os.environ, 'MSYS_NO_PATHCONV': '1'}
    align = (NEW / 'sb-align.txt').read_text().strip()
    t0 = time.monotonic(); procs = []
    for s in shards:
        cmd = ['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo',
               '-e', f'TOTK_XLATE_SB_ALIGN={align}', '-e', 'TOTK_VK_DEPS=queried', *XLATE_ENV, 'python:3.14', 'python3',
               '/repo/native-harmonyos/tools/fill_xlate_in_container.py', rel(s / 'in'), rel(NEW), rel(s / 'out'),
               '/repo/' + XLATE, rel(BUILD / 'spirv-cross' / 'spirv-cross')]
        procs.append((s, subprocess.Popen(cmd, stdout=(s / 'log.txt').open('w'), stderr=subprocess.STDOUT, env=env)))
    done, failed = 0, []
    for s, p in procs:
        p.wait()
        f = s / 'out' / 'filled.json'
        if p.returncode or not f.is_file():
            sys.exit(f'{name} {s.name}: the container failed ({p.returncode}); see {s / "log.txt"}')
        r = json.loads(f.read_text()); done += len(r['done']); failed += r['failed']
        shutil.rmtree(s / 'out')   # the copies in the seed are the ones kept
    print(f'{name}: translated {done} variants, {len(failed)} failed, in {time.monotonic() - t0:.0f} s', flush=True)
    for n, e in failed[:10]:
        print('  ', n, e)
    return failed


def rows(seed, key):
    p = seed / f'{key}.vv'
    out = []
    for line in (p.read_text().splitlines() if p.is_file() else []):
        if line.strip():
            name, _, deps = line.partition('\t')
            out.append((name, deps))
    return out


def dep_ok(a, kind, slot, want):
    """vk_shader.c dep_ok, with a request's answers as the runtime's answers_t."""
    s = str(slot)
    if kind == 'tex':
        have = a['tex'].get(s, '2d')
        if want in ('*', '*_depth'):
            return have.endswith('_depth') == (want == '*_depth')
    elif kind == 'img':
        v = a['img'].get(s)
        have = '' if v is None else v.get('type', '') + ('/' + v['fmt'] if v.get('fmt') else '')
    elif kind == 'attr':
        have = a['attr'].get(s, 'float')
    elif kind == 'fout':
        have = a['fout'].get(s, 'float')
    else:
        return False
    return have == want


def deps_match(deps, a):
    for it in filter(None, deps.split(';')):
        kind, _, rest = it.partition(':')
        slot, _, want = rest.partition('=')
        if not dep_ok(a, kind, int(slot), want):
            return False
    return True


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    global XLATE
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=16)
    ap.add_argument('--swap', action='store_true', help='make the checked new seed the seed (the old one becomes za202.old)')
    ap.add_argument('--check-only', action='store_true', help='skip the translation, check the new seed as it is')
    ap.add_argument('--send', action='store_true', help='after --swap (or a swap before): pack the seed, send it and the .vv overlays')
    ap.add_argument('--xlate', default=XLATE, help='the translator, relative to the repository')
    ap.add_argument('--draw-params', action='store_true', help='TOTK_XLATE_DRAW_PARAMS=1: c0[0x640/0x644/0x648] from the draw parameters')
    ap.add_argument('--expect-diff', action='store_true', help='differing .spv/.vk are listed in reseed/differ.txt instead of failing the check')
    a = ap.parse_args()
    XLATE = a.xlate
    if a.draw_params:
        XLATE_ENV.extend(['-e', 'TOTK_XLATE_DRAW_PARAMS=1'])
    WORK.mkdir(parents=True, exist_ok=True)
    pulled = WORK / 'device-requests'
    if not a.check_only:
        if pulled.exists():
            shutil.rmtree(pulled)
        pulled.mkdir()
        print(hdc('file', 'recv', '-b', BUNDLE, f'{FILES}/shader-requests', str(pulled)).splitlines()[-1])
    device = {}
    for p in sorted((pulled / 'shader-requests').glob('*.json')):
        rq = json.loads(p.read_text())
        device[rq['variant']] = rq
    defaults = {}
    for p in sorted((DUMP / 'requests').glob('shard*/in/*.json')):
        rq = json.loads(p.read_text())
        defaults[rq['variant'].split('-')[0]] = rq
    print(f'{len(defaults)} default requests, {len(device)} device requests', flush=True)

    if not a.check_only:
        if NEW.exists():
            shutil.rmtree(NEW)
        NEW.mkdir(parents=True)
        for t in OLD.glob('*.txt'):
            shutil.copy2(t, NEW / t.name)
        # a default the dump lacks but the device asked for (57246f9a, the noise volume's vertex shader) goes first too
        first = sorted(defaults.items()) + [(n.split('-')[0], rq) for n, rq in sorted(device.items())
                                            if n.endswith('-' + DEFAULT) and n.split('-')[0] not in defaults]
        failed = translate('passA', first, a.jobs)
        extra = [(n.split('-')[0], rq) for n, rq in sorted(device.items()) if not n.endswith('-' + DEFAULT)]
        bones = 0
        for key, rq in sorted(defaults.items()):
            if rq['stage'] != 'vs' or not any('attr:12=' in d for _, d in rows(OLD, key)):
                continue
            answers = dict(attr={'12': 'uint'}, fout={}, img={}, tex={})
            v = dict(rq, variant=f'{key}-{pss.suffix(answers)}', **answers)
            if v['variant'] not in device:
                extra.append((key, v)); bones += 1
        print(f'pass B: {len(extra) - bones} device requests of other bind states, {bones} bone-index (attr 12 uint) variants', flush=True)
        failed += translate('passB', extra, a.jobs)
        if failed:
            print(f'{len(failed)} translations failed')

    # check 1: the same variant translates to the same bytes
    old_v = {p.stem for p in OLD.glob('*.spv')}
    new_v = {p.stem for p in NEW.glob('*.spv')}
    both = sorted(old_v & new_v)
    bad = [v for v in both if sha(OLD / f'{v}.spv') != sha(NEW / f'{v}.spv') or sha(OLD / f'{v}.vk') != sha(NEW / f'{v}.vk')]
    print(f'variants in both seeds: {len(both)}, differing .spv/.vk: {len(bad)}; only in the old seed: {len(old_v - new_v)}, only in the new: {len(new_v - old_v)}')
    for v in bad[:10]:
        print('   differs:', v)
    for v in sorted(old_v - new_v)[:10]:
        print('   not rebuilt:', v)
    # check 2: rows and dependencies
    keys = sorted({p.stem for p in NEW.glob('*.vv')})
    nrows = sum(len(rows(NEW, k)) for k in keys)
    old_rows = sum(len(rows(OLD, k)) for k in {p.stem for p in OLD.glob('*.vv')})
    tex_keys = sum(1 for k in keys if any('tex:' in d for _, d in rows(NEW, k)))
    print(f'new seed: {len(keys)} shaders, {nrows} rows (old: {old_rows}); shaders with any texture dependency left: {tex_keys}')
    # check 3: every miss the device has had matches the new seed, with the SPIR-V it was filled with
    miss = wrong = 0
    for n, rq in sorted(device.items()):
        key = n.split('-')[0]
        hit = next((name for name, deps in rows(NEW, key) if deps_match(deps, rq)), None)
        if hit is None:
            miss += 1; print('   no row matches', n); continue
        want = NEW / f'{n}.spv'
        if not want.is_file():
            want = OLD / f'{n}.spv'
        if want.is_file() and sha(NEW / f'{hit}.spv') != sha(want):
            wrong += 1; print(f'   {n} matches {hit}, whose SPIR-V is not the one it was filled with')
    print(f'device requests: {len(device)}, unmatched {miss}, matched to other SPIR-V {wrong}')
    if a.expect_diff:
        (WORK / 'differ.txt').write_text(''.join(v + '\n' for v in bad))
        print(f'{len(bad)} differing variants listed in {WORK / "differ.txt"} (expected with --expect-diff)')
    ok = (a.expect_diff or not bad) and not miss and not (old_v - new_v) and (a.expect_diff or not wrong)
    print('CHECK', 'PASSED' if ok else 'FAILED')
    if a.swap:
        if not ok:
            return 1
        back = BUILD / 'shaders' / 'za202.old'
        if back.exists():
            shutil.rmtree(back)
        OLD.rename(back); NEW.rename(OLD)
        print(f'swapped: {OLD} is the new seed, the old one is {back}')
    if a.send:
        if not (ok and a.swap):
            print('--send goes with --swap, after the checks passed'); return 1
        # packed in the container (pack_shader_seed.py opens directories with O_DIRECTORY, which Windows Python lacks)
        pack = BUILD / 'seed.vkpack'
        rel = lambda p: '/repo/' + p.resolve().relative_to(REPO).as_posix()
        cmd = ['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo', 'python:3.14',
               'python3', '/android/games/totk/scripts/pack_shader_seed.py', rel(OLD), '--output', rel(pack)]
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
        print((r.stdout + r.stderr).strip()[-300:])
        if r.returncode:
            return 1
        hdc('shell', f'aa force-stop {BUNDLE}')
        print(hdc('file', 'send', '-b', BUNDLE, str(pack), f'{FILES}/shaders/za202/seed.vkpack').splitlines()[-1])
        # Loose files win over the pack (vk_seed_io.c inventory_build), and every fill sent its shader's .vv loose: each
        # shader the device ever requested gets the new .vv sent over its old one.
        sent = bad_send = 0
        for key in sorted({n.split('-')[0] for n in device}):
            vv = OLD / f'{key}.vv'
            if vv.is_file():
                res = hdc('file', 'send', '-b', BUNDLE, str(vv), f'{FILES}/shaders/za202/{key}.vv')
                sent += 1; bad_send += 'finish' not in res
        print(f'sent seed.vkpack and {sent} .vv over the loose ones ({bad_send} failed)')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
