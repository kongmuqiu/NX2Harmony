"""The Legend of Zelda: Ocarina of Time's (Switch 2) shader seed, translated on the PC for the default bind state, as Z-A's
za_seed.py does, from shaders found on the PC (no device dump: the RomFS is zstd with the ZsDic dictionaries):

  1. inventory: seedds/nxnative's dm/gfx/tools/shader_split.py (MIT; run once, DM_GAME=0400486024DEC000, see --split)
     wrote every shader of the RomFS and main into prepared/oot/shader-split/pkg/<stage>_other.bin, one record per shader
     ('BTCDBTCD', control length at +0x28, code length at +0x2c, 0x38 bytes, then the code area, then the control
     section), and facts.tsv (each compute shader's thread group and shared memory);
  2. keys as the device computes them (overlay android/gfx/vk_shader.c, NVN2): SHA-256 of the control section's first 0x48
     bytes (+0x30 read as zero: BNSH files hold the section's own length there) and the code area (control +0x44 bytes);
  3. requests as nxnative's dmsh_build.py job_for makes them for NVN2: arch sm86, the code area up to the instruction length
     (control +0x38: the graphics stages' 0x80-byte SPH included), the c1 segment between control +0x40 and +0x44, and for
     compute nvn2_cs (its own c[0] layout: control +0xb1 / +0xb2 / +0xb3 counts, +0xba size, +0xa9 push constant words,
     +0xb4 / +0xb5 separate texture / sampler counts) with facts.tsv's thread group and shared memory;
     task and mesh shaders (stage byte +0x56: 6 / 7) are counted and left for the mesh stage work (docs/DESIGN-OOT.md);
  4. translated by --jobs python:3.14 containers (native-harmonyos/tools/fill_xlate_in_container.py with the Ampere
     translator artifacts/xlate-ohos-ns2/totk_xlate) into prepared/oot/seed-build/shaders/oot131072;
  5. --variants <tsv> (oot/oot131072-variants.tsv): a second pass, after the full translation, spirv-val
     (tools/spirv_val_seed.sh) and the SSA pass (tools/spirv_opt_seed.sh). The variants the phone filled into the tuned
     oot-a134 seed (fill_shaders_ohos.py, 2026-11-10) that a draw can select: each is the default request with the list's
     answers under the list's variant name (the device's suffix hashes its whole bind state, so it cannot be made from the
     answers), translated in the first pass's environment (XLATE_ENV); the container adds its row to its key's .vv (never
     replaces one). Then spirv-val and the SSA pass on the new modules, and each module's hash checked against the list's;
  6. --smem: a game compute shader's shared memory that is each thread's own slot made a private array
     (tools/mesh_selftest/smem_private.py, oot-a108; docs/DESIGN-OOT.md G3: 9ed48d10 0.61 -> 0.10 ms), for every compute
     module (not a task / mesh kernel) with SMEM_MIN bytes or more of it. Rewritten into seed-build/smem-staged and checked
     with spirv-val there; the valid ones replace the seed's, the originals kept in shaders/pre-smem-oot131072. After the SSA
     pass: the rewrite reads the promoted module (a134's 56 modules were rewritten from it);
  7. --pack: seed.vkpack (the Android port's pack_shader_seed.py); --send: to the phone.
From nothing (NX2Harmony's oot_v131072 steps): oot_seed.py; spirv_val_seed.sh; spirv_opt_seed.sh;
oot_seed.py --variants native-harmonyos/oot/oot131072-variants.tsv --smem; oot_mesh.py; oot_mesh.py --wrap --install; pack.
--variants and --smem run only those passes, not the first (it would translate again the modules spirv-val set aside, and
nothing would check them).
usage: python native-harmonyos/oot/oot_seed.py [--sample N] [--jobs 8] [--variants TSV] [--smem] [--pack] [--send]
Everything it writes is game-derived and stays under prepared/ (ignored)."""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from ohos_env import HDC, DEV, UPSTREAM, REPO, docker_path   # noqa: E402
sys.path.insert(0, str(UPSTREAM / 'games' / 'totk' / 'scripts'))
import prepare_shader_seed as pss   # noqa: E402

BUNDLE = os.environ.get('TOTK_OHOS_BUNDLE') or 'nx2h.u000000.oot'
FILES = '/data/storage/el2/base/haps/entry/files/android'
SPLIT = PORT / 'prepared' / 'oot' / 'shader-split'
# OOT_SEED_BUILD / OOT_XLATE: another seed folder and translator (a translator trial beside the seed in use)
BUILD = Path(os.environ['OOT_SEED_BUILD']) if os.environ.get('OOT_SEED_BUILD') else PORT / 'prepared' / 'oot' / 'seed-build'
SEED = BUILD / 'shaders' / 'oot131072'
XLATE = Path(os.environ['OOT_XLATE']) if os.environ.get('OOT_XLATE') else PORT / 'artifacts' / 'xlate-ohos-ns2' / 'totk_xlate'
ANSWERS = dict(attr={}, fout={}, img={}, tex={})
SUFFIX = pss.suffix(ANSWERS)
CTL_MAGIC = bytes.fromhex('34127698')
TOOLS = 'nativegames-tools:1'   # spirv-val, spirv-opt (tools/spirv_val_seed.sh, spirv_opt_seed.sh)
SMEM_MIN = 1024                 # --smem: shared memory of at least this many bytes (smem_private.py --scan's default)


def records():
    """(stage, control, code) of every record in the package."""
    for f in sorted((SPLIT / 'pkg').glob('*.bin')):
        stage = f.name[:2]
        d = f.read_bytes()
        at = d.find(b'BTCDBTCD')
        while at >= 0:
            nxt = d.find(b'BTCDBTCD', at + 8)
            nctl, ncode = struct.unpack_from('<II', d, at + 0x28)
            code = d[at + 0x38:at + 0x38 + ncode]
            ctl = d[at + 0x38 + ncode:at + 0x38 + ncode + nctl]
            assert ctl[:4] == CTL_MAGIC, f'{f.name}@{at:#x}: no control section'
            yield stage, ctl, code
            at = nxt


def nvn2_key(ctl, code):
    k = bytearray(ctl[:0x48]); k[0x30:0x34] = bytes(4)
    n = struct.unpack_from('<I', ctl, 0x44)[0]
    return hashlib.sha256(bytes(k) + code[:n]).hexdigest()


def facts():
    out = {}
    for line in (SPLIT / 'facts.tsv').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            k, v = line.split('\t', 1)
            out[k] = json.loads(v)
    return out


def nx_key(ctl, code):   # nxnative's 16-hex key (shader_split.py), to look up facts.tsv
    k = bytearray(ctl[:0x48]); k[0x30:0x34] = bytes(4)
    n = struct.unpack_from('<I', ctl, 0x44)[0]
    return hashlib.sha256(bytes(k) + code[:n]).hexdigest()[:16]


def request(key, stage, ctl, code, fact):
    inst = struct.unpack_from('<I', ctl, 0x38)[0]
    a, e = struct.unpack_from('<II', ctl, 0x40)
    rq = dict(variant=f'{key}-{SUFFIX}', stage=stage, arch='sm86', depth_minus_w=True, early_fragment_tests=False,
              c1=code[a:e].hex() if a <= e <= len(code) else '', code_hex=code[:inst].hex(), **ANSWERS)
    if stage == 'cs':
        rq['nvn2_cs'] = [ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<H', ctl, 0xba)[0], ctl[0xa9] * 4, ctl[0xb4], ctl[0xb5]]
        rq['local'] = fact['local'] if fact else list(struct.unpack_from('<III', ctl, 0x58))
        if fact and fact.get('shared'):
            rq['shared'] = fact['shared']
    return rq


# Bindless pools (prepare_xlate_ns2.py): descriptor set 2, the game's texture pool (33068) and sampler pool (4096) sizes
POOL_ENV = ['-e', 'TOTK_XLATE_POOL_SET=2', '-e', 'TOTK_XLATE_POOL_TEX=33068', '-e', 'TOTK_XLATE_POOL_SAMP=4096']
# The draw's base instance, which 1937 vertex shaders read at c0 + 0x644 (oot/drv_reads.py) and the replay's driver constants
# leave 0, comes from Vulkan's BaseInstance (xlate_edits.py, as Z-A's seed): without it a draw with a base instance read
# instance 0's data
DRAW_ENV = ['-e', 'TOTK_XLATE_DRAW_PARAMS=1']
# Every translation pass (the full one and --variants; fill_shaders_ohos.py's phone fills had the same): the storage-buffer
# alignment the seed's sb-align.txt states, a variant's texture dependencies only the slots the translator asked the type of
XLATE_ENV = ['-e', 'TOTK_XLATE_SB_ALIGN=256', '-e', 'TOTK_VK_DEPS=queried', *POOL_ENV, *DRAW_ENV]


def rel(p):
    return '/repo/' + Path(p).resolve().relative_to(REPO).as_posix()


def translate(reqs, jobs, failures='failures.json'):
    """Translate the requests in python:3.14 containers (tools/fill_xlate_in_container.py: <variant>.spv / .vk into the seed
    folder, the variant's row added to its key's .vv). The requests of one key go to the same container, which reads and
    writes that key's .vv. -> (the variants translated, whether every container finished)"""
    reqs_dir = BUILD / 'requests'
    if reqs_dir.exists():
        shutil.rmtree(reqs_dir)
    keys = sorted({rq['variant'][:64] for rq in reqs})
    jobs = max(1, min(jobs, len(keys)))
    shard_of = {k: i % jobs for i, k in enumerate(keys)}
    shards = [reqs_dir / f'shard{i:02d}' for i in range(jobs)]
    for s in shards:
        (s / 'in').mkdir(parents=True)
    for rq in reqs:
        (shards[shard_of[rq['variant'][:64]]] / 'in' / f'{rq["variant"]}.json').write_text(json.dumps(rq))
    sc = PORT / 'prepared' / 'oot' / 'seed-build' / 'spirv-cross' / 'spirv-cross'
    if not sc.is_file():
        sys.exit(f'no SPIRV-Cross at {sc} (build it as the Z-A backend does, or copy LZA prepared/za/seed-build/spirv-cross)')
    env = {**os.environ, 'MSYS_NO_PATHCONV': '1'}
    t0 = time.monotonic(); procs = []
    for s in shards:
        cmd = ['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo',
               *XLATE_ENV, 'python:3.14', 'python3',
               '/repo/native-harmonyos/tools/fill_xlate_in_container.py', rel(s / 'in'), rel(SEED), rel(s / 'out'), rel(XLATE), rel(sc)]
        procs.append((s, subprocess.Popen(cmd, stdout=(s / 'log.txt').open('w'), stderr=subprocess.STDOUT, env=env)))
    done, failed, ok, errors = [], 0, True, {}
    for s, p in procs:
        p.wait()
        f = s / 'out' / 'filled.json'
        if p.returncode or not f.is_file():
            print(f'{s.name}: the container failed ({p.returncode}); see {s / "log.txt"}'); ok = False; continue
        r = json.loads(f.read_text()); done += [n[:-len('.json')] for n in r['done']]; failed += len(r['failed'])
        for n, e in r['failed']:
            errors.setdefault((e or '?')[:90], []).append(n)
    print(f'translated {len(done)} variants, {failed} failed, in {time.monotonic() - t0:.0f} s')
    for e, names in sorted(errors.items(), key=lambda kv: -len(kv[1]))[:15]:
        print(f'  {len(names):5d}  {e}   e.g. {names[0]}')
    (BUILD / failures).write_text(json.dumps({e: n for e, n in errors.items()}, indent=1))
    return done, ok


def seed_tool(script, *args):
    """native-harmonyos/tools/<script> in TOOLS -> its last line, None when it failed. Without spirv-val in the image
    spirv_val_seed.sh would take every module for invalid and set it aside, so the tools are looked for first."""
    env = {**os.environ, 'MSYS_NO_PATHCONV': '1'}
    if not getattr(seed_tool, 'checked', False):
        r = subprocess.run(['docker', 'run', '--rm', TOOLS, 'sh', '-c', 'command -v spirv-val && command -v spirv-opt'],
                           capture_output=True, text=True, errors='replace', env=env)
        if r.returncode:
            print(f'{TOOLS} has no spirv-val / spirv-opt (apt-get install spirv-tools in it): nothing done')
            return None
        seed_tool.checked = True
    r = subprocess.run(['docker', 'run', '--rm', '-v', docker_path(REPO) + ':/repo', TOOLS, 'sh', f'/repo/native-harmonyos/tools/{script}', *args],
                       capture_output=True, text=True, errors='replace', env=env)
    out = (r.stdout + r.stderr).strip()
    if r.returncode:
        print(f'{script} failed ({r.returncode}): {out[-400:]}')
        return None
    return out.splitlines()[-1] if out else ''


def keep_log(name, run):
    """run() writes the seed's shaders/<name> (spirv_val_seed.sh / spirv_opt_seed.sh's log, the whole seed's record): that
    one is put back, this run's goes to shaders/variants-<name>."""
    log = SEED.parent / name
    old = log.read_bytes() if log.is_file() else None
    try:
        return run()
    finally:
        if log.is_file():
            os.replace(log, SEED.parent / f'variants-{name}')
        if old is not None:
            log.write_bytes(old)


def variant_rows(path):
    """(variant, stage, answers, the .vv row's answers, sha1 or '') of a variants list: tab-separated variant, stage,
    answers as a .vv row has them (kind:slot=value;...), optionally the module's SHA-1 in the seed it came from; # comments"""
    rows = []
    for n, line in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) < 3 or not re.fullmatch(r'[0-9a-f]{64}-[0-9a-f]{12}', f[0]) or f[1] not in ('vs', 'ps', 'cs'):
            sys.exit(f'{path}:{n}: not "variant<TAB>stage<TAB>answers[<TAB>sha1]"')
        answers = {kind: {} for kind in ANSWERS}
        for dep in filter(None, f[2].split(';')):
            m = re.fullmatch(r'(attr|fout|img|tex):([0-9]+)=([^;=\s]+)', dep)
            if not m:
                sys.exit(f'{path}:{n}: not an answer: {dep}')
            kind, slot, value = m.groups()
            if kind == 'img' and '/' in value:   # as the device's request and prepare_shader_seed.py have an image answer
                t, fmt = value.split('/', 1)
                value = dict(type=t, fmt=fmt)
            answers[kind][slot] = value
        rows.append((f[0], f[1], answers, f[2], f[3].strip() if len(f) > 3 else ''))
    return rows


def package():
    """{key: (stage, control, code)} of the package's vertex / pixel / compute shaders"""
    shaders, count = {}, {}
    for stage, ctl, code in records():
        count[stage] = count.get(stage, 0) + 1
        if stage in ('ts', 'ms'):
            continue
        shaders.setdefault(nvn2_key(ctl, code), (stage, ctl, code))
    print('package:', ', '.join(f'{s} {n}' for s, n in sorted(count.items())), f'-> {len(shaders)} vertex / pixel / compute keys')
    return shaders


def first_pass(a):
    fx, shaders = facts(), package()
    todo = [(k, v) for k, v in sorted(shaders.items()) if not (SEED / f'{k}-{SUFFIX}.spv').is_file()]
    if a.only:
        only = set(Path(a.only).read_text().split())
        todo = [(k, v) for k, v in sorted(shaders.items()) if k in only]
    if a.sample:
        per = {}
        todo = [t for t in todo if per.setdefault(t[1][0], []).__len__() < a.sample and not per[t[1][0]].append(t)]
    print(f'{len(todo)} to translate ({len(shaders) - len(todo)} already in the seed)')
    if todo:
        translate([request(key, stage, ctl, code, fx.get(nx_key(ctl, code))) for key, (stage, ctl, code) in todo], a.jobs)
    return 0


def variants_pass(path, jobs):
    """--variants: the list's variants the seed lacks, translated, validated and promoted to SSA as the seed's own were"""
    rows = variant_rows(path)
    fx, shaders = facts(), package()
    reqs, have, refused = [], 0, []
    for variant, stage, answers, flat, sha in rows:
        key = variant[:64]
        if key not in shaders or shaders[key][0] != stage:
            refused.append(f'{variant}: ' + ('not in the package' if key not in shaders else f'a {shaders[key][0]} shader, not {stage}'))
        elif not (SEED / f'{key}.vv').is_file():   # the second pass adds to the first's .vv: no default, no variants
            refused.append(f'{variant}: the seed has no {key[:16]}.vv (the full translation first)')
        elif (SEED / f'{variant}.spv').is_file():
            have += 1
        else:
            st, ctl, code = shaders[key]
            rq = request(key, st, ctl, code, fx.get(nx_key(ctl, code)))
            rq.update(answers, variant=variant)
            reqs.append(rq)
    print(f'{path}: {len(rows)} variants, {len(reqs)} to translate, {have} already in the seed, {len(refused)} refused')
    for r in refused[:20]:
        print('  refused', r)
    ok = True
    if reqs:
        done, ok = translate(reqs, jobs, 'variants-failures.json')
        if done:   # spirv-val sets the invalid ones aside (invalid-oot131072), then the SSA pass as on the whole seed
            lst = BUILD / 'variants-modules.txt'
            lst.write_bytes(''.join(f'{v}.spv\n' for v in sorted(done)).encode())   # LF: spirv_val_seed.sh reads it as is
            for script, log, args in (('spirv_val_seed.sh', 'spirv-val.txt', (rel(SEED), rel(lst))),
                                      ('spirv_opt_seed.sh', 'spirv-opt.txt', (rel(SEED), str(jobs), rel(lst)))):
                last = keep_log(log, lambda: seed_tool(script, *args))
                print(f'{script}: {last}')
                if last is None:
                    return 1
    # the result: every listed variant in the seed, its row in its key's .vv, and its module the list's
    missing, unlisted, same, other = [], [], 0, []
    for variant, stage, answers, flat, sha in rows:
        spv = SEED / f'{variant}.spv'
        if not spv.is_file():
            missing.append(variant); continue
        if f'{variant}\t{flat}' not in (SEED / f'{variant[:64]}.vv').read_text().splitlines():
            unlisted.append(variant)
        if sha:
            if hashlib.sha1(spv.read_bytes()).hexdigest() == sha:
                same += 1
            else:
                other.append(variant)
    print(f'{len(rows) - len(missing)} of {len(rows)} listed variants in the seed; {len(unlisted)} without their .vv row '
          f'(a draw cannot select them); {same} modules as the list\'s hash, {len(other)} different')
    for what, names in (('not in the seed', missing), ('no .vv row', unlisted), ('another module than the list\'s', other)):
        for v in names[:10]:
            print(f'  {what}: {v}')
    return 0 if ok and not missing and not unlisted else 1


def smem_pass(least=SMEM_MIN):
    """--smem: compute modules whose shared memory (SMEM_MIN bytes or more) is a slot a thread, made private arrays"""
    sys.path.insert(0, str(PORT / 'native-harmonyos' / 'tools' / 'mesh_selftest'))
    import smem_private as sp
    stage_dir = BUILD / 'smem-staged'
    staged, keep = stage_dir / SEED.name, SEED.parent / f'pre-smem-{SEED.name}'
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    staged.mkdir(parents=True)
    t0, picked, small, why = time.monotonic(), [], 0, {}
    for spv in sorted(SEED.glob('*.spv')):
        vk = spv.with_suffix('.vk')
        if vk.is_file():   # compute only; a task / mesh kernel (a 'mesh' line) is the wrapper's (smem_private.py --scan)
            lines = vk.read_text(errors='replace').splitlines()
            if len(lines) < 2 or lines[1] != 'stage 5' or any(l.startswith('mesh ') for l in lines):
                continue
        b = spv.read_bytes()
        try:
            head, ins = sp.parse(b)
            if not any(x[0] & 0xffff == sp.OP['Variable'] and x[3] == sp.WORKGROUP for x in ins) or \
               not any(x[0] & 0xffff == sp.OP['EntryPoint'] and x[1] == 5 for x in ins):
                continue
            if sp.analyse(head, ins)[4] * 4 < least:
                small += 1; continue
            out, what = sp.rewrite(b)
        except sp.Refused as e:
            why[str(e)] = why.get(str(e), 0) + 1; continue
        (staged / spv.name).write_bytes(out); picked.append(spv.name)
    print(f'smem: {len(picked)} compute modules with {least} bytes or more of shared memory that is a slot a thread '
          f'({small} smaller, {sum(why.values())} refused) in {time.monotonic() - t0:.0f} s')
    for e, n in sorted(why.items(), key=lambda kv: -kv[1])[:6]:
        print(f'  refused {n:4d}  {e}')
    if not picked:
        return 0
    last = seed_tool('spirv_val_seed.sh', rel(staged))   # the invalid go to smem-staged/invalid-oot131072, the seed's stay
    print(f'spirv_val_seed.sh (smem-staged): {last}')
    if last is None:
        return 1
    keep.mkdir(exist_ok=True)
    put = 0
    for name in picked:
        src = staged / name
        if not src.is_file():
            continue
        shutil.copy2(SEED / name, keep / name)
        tmp = SEED / (name + '.smem-tmp')
        shutil.copyfile(src, tmp); os.replace(tmp, SEED / name)
        put += 1
    print(f'smem: {put} modules replaced (originals in {keep}); {len(picked) - put} invalid, left as they were')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sample', type=int, default=0, help='translate only the first N shaders of each stage (a check)')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) // 2))
    ap.add_argument('--pack', action='store_true')
    ap.add_argument('--send', action='store_true')
    ap.add_argument('--only', help='a file of shader keys (one a line): translate only those, again if they are in the seed')
    ap.add_argument('--variants', metavar='TSV', help='second pass: the listed variants (oot/oot131072-variants.tsv), after the '
                    'full translation, spirv-val and the SSA pass')
    ap.add_argument('--smem', action='store_true', help='after the SSA pass: compute shaders\' per-thread shared memory made private')
    a = ap.parse_args()
    SEED.mkdir(parents=True, exist_ok=True)
    if not (SEED / 'sb-align.txt').is_file():
        (SEED / 'sb-align.txt').write_text('256\n')
    if a.variants or a.smem:
        if a.variants and variants_pass(Path(a.variants), a.jobs):
            return 1
        if a.smem and smem_pass():
            return 1
    else:
        first_pass(a)
    if a.pack or a.send:
        out = BUILD / 'seed.vkpack'
        # in the container: the reference's pack_shader_seed.py opens directories with O_DIRECTORY, which Windows Python lacks
        r = subprocess.run(['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo', 'python:3.14',
                            'python3', '/android/games/totk/scripts/pack_shader_seed.py', rel(SEED), '--output', rel(out)],
                           capture_output=True, text=True, errors='replace', env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
        print((r.stdout + r.stderr).strip()[-400:])
        if r.returncode:
            return 1
        if a.send:
            r = subprocess.run([HDC, *DEV, 'file', 'send', '-b', BUNDLE, str(out), f'{FILES}/shaders/oot131072/seed.vkpack'], capture_output=True, text=True)
            print((r.stdout + r.stderr).strip().splitlines()[-1])
    return 0


if __name__ == '__main__':
    sys.exit(main())
