# Rewrite TOTK's three code modules for the HarmonyOS port of the Android native stack (run inside the nativegames-tools:1
# container, tools/docker/d2r.Dockerfile). Mirrors android/build_android.py android_rewrite():
#   1. the verified Darwin rewrite (DM_RULESET=native, DM_PRIVATE_GATES=0), whose image hashes must equal
#      games/totk/assets/expected-v<version>.json: proves the extracted ExeFS, the rewriter and its pinned capstone;
#   2. the HarmonyOS rewrite (d2r_ohos.py: the Darwin form with tpidr_el0);
#   3. the same site / cut / param / obs / gate sets as the Darwin rewrite, and the same image word for word except
#      the host-register reads (mrs xN, tpidrro_el0 -> mrs xN, tpidr_el0, same register);
#   4. structck.py, word by word, with the HarmonyOS register layout;
#   5. <out>/rewritten/phase12-validation.json, the report scripts/prepare_runtime.py binds the tables to.
# Everything written here is game material: it stays under prepared/ (ignored), never committed or published.
# usage: python3 rewrite_ohos.py --android /android --exefs <exefs folder> --out <folder> [--version v143]
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODULES = ('main', 'subsdk0', 'sdk')
EXPECTED = {'v0': 'expected-v1.0.0.json', 'v112': 'expected-v1.1.2.json', 'v121': 'expected-v1.2.1.json', 'v143': 'expected-v1.4.3.json'}

ap = argparse.ArgumentParser()
ap.add_argument('--android', required=True, type=Path)
ap.add_argument('--exefs', required=True, type=Path)
ap.add_argument('--out', required=True, type=Path)
ap.add_argument('--version', default='v143', choices=sorted(EXPECTED))
a = ap.parse_args()
tree = a.android / 'games' / 'totk'
d2r = tree / 'third_party' / 'd2r'
expected = json.loads((tree / 'assets' / EXPECTED[a.version]).read_text())


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


for name in MODULES:
    assert sha256(a.exefs / name) == expected['exefs_sha256'][name], f'{name}: ExeFS differs from {EXPECTED[a.version]}'
print('ExeFS matches', EXPECTED[a.version], flush=True)

base_env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0', LLVM_BIN='/usr/bin',
                DM_VCACHE=str(a.out / 'cache'), PYTHONDONTWRITEBYTECODE='1')
base_env.pop('DM_HOST_TP', None)
darwin, ohos, logs = a.out / 'rewritten-darwin', a.out / 'rewritten', a.out / 'logs'
for d in (darwin, ohos, logs):
    d.mkdir(parents=True, exist_ok=True)

for name in MODULES:   # 1. verified Darwin rewrite
    img = darwin / f'{name}.patched.bin'
    want = expected['rewritten_sha256'][f'{name}.patched.bin']
    if not (img.exists() and sha256(img) == want):
        run([sys.executable, d2r / 'patch_nso.py', a.exefs / name, darwin, name], logs / f'darwin-{name}.log', base_env)
    assert sha256(img) == want, f'{name}: the Darwin rewrite does not reproduce the verified image'
print('Darwin rewrite reproduces the verified hashes', flush=True)


def site_lists(folder, name):
    out = {}
    for kind in ('sites', 'cuts', 'params', 'obs'):
        p = folder / f'{name}.{kind}.tsv'
        out[kind] = sorted(p.read_text().splitlines()) if p.exists() else []
    out['gates'] = sorted(' '.join(l.split()[1:]) for l in (folder / f'{name}.gates.tsv').read_text().splitlines())
    return out


wrapper = HERE / 'd2r_ohos.py'
for name in MODULES:   # 2-4. HarmonyOS rewrite, same sets, structural check
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
    print(f'  {name}: identical to the verified Darwin image except {words} host-register reads', flush=True)
    run([sys.executable, wrapper, d2r, 'structck.py', ohos, name, a.exefs / name], logs / f'structck-{name}.log', base_env)

report = dict(format=1, status='pass', rom_version=a.version, host='harmonyos-musl (tpidr_el0 slot base, key page = TP offsets of libtotk_ohos.so TLS slots)',
              scope='verified Darwin image word for word except host-register reads; same sites; structck',
              module_file_sha256={**{f'extracted/{a.version}/exefs/{n}': sha256(a.exefs / n) for n in MODULES},
                                  **{f'rewritten/{n}.patched.bin': sha256(ohos / f'{n}.patched.bin') for n in MODULES}})
(ohos / 'phase12-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=1))
