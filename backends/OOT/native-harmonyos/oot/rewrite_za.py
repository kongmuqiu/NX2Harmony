# Rewrite Pokémon Legends: Z-A 2.0.2's code modules (main, sdk) for the HarmonyOS host, inside the nativegames-tools:1
# container (tools/docker/d2r.Dockerfile). The same steps as ../rewrite_ohos.py for Tears of the Kingdom, except that no
# verified Darwin images exist for this game yet:
#   1. the ExeFS modules against assets/expected-za-2.0.2.json (sha256 and NSO build ids);
#   2. the Darwin rewrite (DM_RULESET=native, DM_PRIVATE_GATES=0): with --record its image hashes are written to the
#      expected file the first time; afterwards they must reproduce;
#   3. the HarmonyOS rewrite (../d2r_ohos.py: the Darwin form with tpidr_el0);
#   4. the same site / cut / param / obs / gate sets as the Darwin rewrite, and the same image word for word except the
#      host-register reads (mrs xN, tpidrro_el0 -> mrs xN, tpidr_el0);
#   5. structck.py, word by word, with the HarmonyOS register layout;
#   6. <out>/rewritten/phase12-validation.json.
# Everything written here is game material: it stays under prepared/ (ignored), never committed or published.
# usage: python3 rewrite_za.py --android /android --exefs <exefs folder> --out <folder> [--record] [--modules main,sdk]
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXPECTED = ROOT / 'assets' / 'expected-za-2.0.2.json'

ap = argparse.ArgumentParser()
ap.add_argument('--android', required=True, type=Path)
ap.add_argument('--exefs', required=True, type=Path)
ap.add_argument('--out', required=True, type=Path)
ap.add_argument('--record', action='store_true', help='write the Darwin image hashes the first time')
ap.add_argument('--modules', default='main,sdk')
a = ap.parse_args()
MODULES = tuple(a.modules.split(','))
d2r = HERE / 'd2r'   # the reference's d2r copy with the Z-A additions (README.md there lists them)
expected = json.loads(EXPECTED.read_text())


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, log, env):
    t = time.time()
    with open(log, 'w') as f:
        r = subprocess.run([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT, env=env)
    print(f'  {Path(log).name}: rc={r.returncode} {time.time() - t:.0f}s', flush=True)
    if r.returncode:
        sys.exit(f'failed: {" ".join(map(str, cmd))} (see {log})')


for name in MODULES:   # 1
    assert sha256(a.exefs / name) == expected['exefs_sha256'][name], f'{name}: ExeFS differs from {EXPECTED.name}'
    bid = (a.exefs / name).read_bytes()[0x40:0x50].hex()
    assert bid == expected['build_id'][name], f'{name}: build id {bid}'
print('ExeFS matches', EXPECTED.name, flush=True)

base_env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0', LLVM_BIN='/usr/bin',
                DM_VCACHE=str(a.out / 'cache'), PYTHONDONTWRITEBYTECODE='1')
base_env.pop('DM_HOST_TP', None)
darwin, ohos, logs = a.out / 'rewritten-darwin', a.out / 'rewritten', a.out / 'logs'
for d in (darwin, ohos, logs):
    d.mkdir(parents=True, exist_ok=True)

recorded = dict(expected.get('rewritten_sha256') or {})
for name in MODULES:   # 2
    img = darwin / f'{name}.patched.bin'
    want = recorded.get(f'{name}.patched.bin')
    if not (img.exists() and want and sha256(img) == want):
        run([sys.executable, d2r / 'patch_nso.py', a.exefs / name, darwin, name], logs / f'darwin-{name}.log', base_env)
    got = sha256(img)
    if want:
        assert got == want, f'{name}: the Darwin rewrite does not reproduce the recorded image'
    elif a.record:
        recorded[f'{name}.patched.bin'] = got
    else:
        sys.exit(f'{name}: no recorded Darwin image hash; run again with --record after checking the logs')
if a.record and recorded != expected.get('rewritten_sha256'):
    expected['rewritten_sha256'] = recorded
    EXPECTED.write_text(json.dumps(expected, indent=2) + '\n')
    print('recorded the Darwin image hashes in', EXPECTED.name, flush=True)
print('Darwin rewrite matches the recorded hashes', flush=True)


def site_lists(folder, name):
    out = {}
    for kind in ('sites', 'cuts', 'params', 'obs'):
        p = folder / f'{name}.{kind}.tsv'
        out[kind] = sorted(p.read_text().splitlines()) if p.exists() else []
    g = folder / f'{name}.gates.tsv'
    out['gates'] = sorted(' '.join(l.split()[1:]) for l in g.read_text().splitlines()) if g.exists() else []
    return out


wrapper = HERE.parent / 'd2r_ohos.py'
for name in MODULES:   # 3-5
    run([sys.executable, wrapper, d2r, 'patch_nso.py', a.exefs / name, ohos, name], logs / f'ohos-{name}.log', base_env)
    x, y = site_lists(darwin, name), site_lists(ohos, name)
    for kind in x:
        assert x[kind] == y[kind], f'{name}: HarmonyOS rewrite has different {kind} ({len(x[kind])} vs {len(y[kind])})'
    da, oa = (darwin / f'{name}.patched.bin').read_bytes(), (ohos / f'{name}.patched.bin').read_bytes()
    assert len(da) == len(oa), f'{name}: image size differs from the Darwin rewrite'
    words = 0
    for o in range(0, len(da) - 3, 4):
        if da[o:o + 4] != oa[o:o + 4]:
            dw, ow = int.from_bytes(da[o:o + 4], 'little'), int.from_bytes(oa[o:o + 4], 'little')
            assert dw & ~31 == 0xD53BD060 and ow == 0xD53BD040 | (dw & 31), f'{name}+{o:#x}: {dw:08x} -> {ow:08x}'
            words += 1
    assert da[len(da) & ~3:] == oa[len(oa) & ~3:] and words > 0
    print(f'  {name}: identical to the Darwin image except {words} host-register reads; '
          + ', '.join(f'{k} {len(v)}' for k, v in x.items()), flush=True)
    run([sys.executable, wrapper, d2r, 'structck.py', ohos, name, a.exefs / name], logs / f'structck-{name}.log', base_env)

report = dict(format=1, status='pass', game='Pokémon Legends: Z-A', rom_version='v393216 (2.0.2)',
              host='harmonyos-musl (tpidr_el0 slot base, key page = TP offsets of the runtime library TLS slots)',
              scope='Darwin image (recorded) word for word except host-register reads; same sites; structck',
              module_file_sha256={**{f'exefs/{n}': sha256(a.exefs / n) for n in MODULES},
                                  **{f'rewritten/{n}.patched.bin': sha256(ohos / f'{n}.patched.bin') for n in MODULES}})
(ohos / 'phase12-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=1))
