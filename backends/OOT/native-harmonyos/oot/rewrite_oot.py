# Rewrite The Legend of Zelda: Ocarina of Time's (Switch 2, v131072) code modules (main, sdk, subsdk0) for the HarmonyOS
# host, inside the nativegames-tools:1 container (tools/docker/d2r.Dockerfile). The Z-A driver's steps (rewrite_za.py),
# on the modules as nso_unzbic.py stores them (a system 22 module's ZBIC segments uncompressed, the same image):
#   1. the ExeFS modules against assets/expected-oot-v131072.json (sha256 and NSO build ids), and each --plain module
#      against its plain_sha256 there (nso_unzbic.py on the PC: the container has no ZBIC decoder);
#   2. the Darwin rewrite (DM_RULESET=native, DM_PRIVATE_GATES=0): with --record its image hashes are written to the
#      expected file the first time; afterwards they must reproduce;
#   3. the HarmonyOS rewrite (../d2r_ohos.py: the Darwin form with tpidr_el0);
#   4. the same site / cut / param / obs / gate sets as the Darwin rewrite, and the same image word for word except the
#      host-register reads (mrs xN, tpidrro_el0 -> mrs xN, tpidr_el0);
#   5. structck.py, word by word, with the HarmonyOS register layout;
#   6. <out>/rewritten/phase12-validation.json, after
#   7. pointer authentication removed from the HarmonyOS images (see below).
# Everything written here is game material: it stays under prepared/ (ignored), never committed or published.
# usage: python3 rewrite_oot.py --android /android --exefs <exefs folder> --plain <nso_unzbic.py output folder> --out <folder>
#        [--record] [--modules main,sdk,subsdk0]
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXPECTED = ROOT / 'assets' / 'expected-oot-v131072.json'

ap = argparse.ArgumentParser()
ap.add_argument('--android', required=True, type=Path)
ap.add_argument('--exefs', required=True, type=Path)
ap.add_argument('--plain', required=True, type=Path, help='the modules as nso_unzbic.py wrote them')
ap.add_argument('--out', required=True, type=Path)
ap.add_argument('--record', action='store_true', help='write the Darwin image hashes the first time')
ap.add_argument('--modules', default='main,sdk,subsdk0')
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
    assert sha256(a.plain / name) == expected['plain_sha256'][name], f'{name}: --plain module is not nso_unzbic.py of the ExeFS one'
print('ExeFS matches', EXPECTED.name, '(and the plain modules match it)', flush=True)

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
        run([sys.executable, d2r / 'patch_nso.py', a.plain / name, darwin, name], logs / f'darwin-{name}.log', base_env)
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
    run([sys.executable, wrapper, d2r, 'patch_nso.py', a.plain / name, ohos, name], logs / f'ohos-{name}.log', base_env)
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
    run([sys.executable, wrapper, d2r, 'structck.py', ohos, name, a.plain / name], logs / f'structck-{name}.log', base_env)

# Pointer authentication (step 7): the Switch 2's Cortex-A78C signs return addresses (pacibsp / autibsp / retab: main
# 161,622 / 63,806 / 126,456), and sdk signs a few pointers in registers. The phone may have no PAC (retab, pacia, autib,
# xpaci are then undefined instructions) or keys the host's own code does not share, so every PAC instruction becomes
# the instruction it is with nothing signed: the hint forms, the register forms and xpac nop (a pointer stays unsigned, as
# it already is), retaa / retab ret, pacga eor (below). Text and the rewriter's patch area (it copies original instructions); counted per kind.
NOP, RET = 0xD503201F, 0xD65F03C0
PAC_HINTS = {8, 10, 12, 14, 24, 25, 26, 27, 28, 29, 30, 31, 7}   # pacia1716 .. autibsp, xpaclri (hint #imm)
def pac_kind(w):
    if (w & 0xFFFFF01F) == 0xD503201F and ((w >> 5) & 0x7F) in PAC_HINTS: return 'hint', NOP
    if w in (0xD65F0BFF, 0xD65F0FFF): return 'retab/retaa', RET
    if (w & 0xFFFFC000) == 0xDAC10000: return 'register', NOP   # pacia .. autdzb Xd, Xn|sp (opcode 0..15)
    if (w & 0xFFFFFBE0) == 0xDAC143E0: return 'xpaci/xpacd', NOP
    # pacga Xd, Xn, Xm|sp: sdk's setjmp / longjmp (and its fiber switch) checksum the saved registers with it and compare;
    # both sides run the same code, so any deterministic mix keeps them equal: eor Xd, Xn, Xm (no Xm = sp in this sdk).
    if (w & 0xFFE0FC00) == 0x9AC03000:
        assert (w >> 16) & 31 != 31, f'pacga with sp: {w:08x}'
        return 'pacga', 0xCA000000 | (w & 0x001F03FF)
    return None, None
def pac_left(w):   # what nothing above handles: authenticated branches and loads (none in OoT's text; fail if one appears)
    return ((w & 0xFFFFFC1F) in (0xD63F081F, 0xD63F0C1F, 0xD61F081F, 0xD61F0C1F) or (w & 0xFEDFF800) == 0xD61F0800
            or (w & 0xFF200400) == 0xF8200400 or (w & 0xFFFFFBFF) == 0xD69F0BFF)
def fixup(name, key):
    for line in (ohos / f'{name}.fixups.tsv').read_text().splitlines():
        q = line.split()
        if len(q) == 3 and q[1] == key: return int(q[2], 16)
    raise KeyError(key)
pac = {}
for name in MODULES:   # 7
    img = bytearray((ohos / f'{name}.patched.bin').read_bytes())
    counts = {}
    for lo, hi in ((0, fixup(name, 'TEXT_END')), (fixup(name, 'PATCH_OFF'), len(img) & ~3)):
        for o in range(lo, hi & ~3, 4):
            w = int.from_bytes(img[o:o + 4], 'little')
            kind, rep = pac_kind(w)
            if kind:
                img[o:o + 4] = rep.to_bytes(4, 'little'); counts[kind] = counts.get(kind, 0) + 1
            elif lo == 0 and pac_left(w):
                counts.setdefault('left (authenticated branch / load)', []).append(hex(o))
    assert 'left (authenticated branch / load)' not in counts, f'{name}: {counts}'
    (ohos / f'{name}.patched.bin').write_bytes(bytes(img))
    pac[name] = counts
    print(f'  {name}: pointer authentication removed {counts}', flush=True)

report = dict(format=1, status='pass', game='The Legend of Zelda: Ocarina of Time (Switch 2)', rom_version='v131072',
              host='harmonyos-musl (tpidr_el0 slot base, key page = TP offsets of the runtime library TLS slots)',
              scope='Darwin image (recorded) word for word except host-register reads; same sites; structck; then pointer authentication removed',
              pointer_authentication_removed=pac,
              module_file_sha256={**{f'exefs/{n}': sha256(a.exefs / n) for n in MODULES},
                                  **{f'rewritten/{n}.patched.bin': sha256(ohos / f'{n}.patched.bin') for n in MODULES}})
(ohos / 'phase12-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=1))
