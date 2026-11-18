"""Container half of native-harmonyos/build_seed.py (python:3.14; /android = the reference checkout, read-only; /work = the
build's Docker volume, shared by all its containers; /out = the seed folder on the PC). The seed is the one build_seed.sh
makes, byte for byte: the reference's prepare_shader_seed.py translates, android/vk_reflect.py reflects,
scripts/pack_shader_seed.py packs; this only cuts the work into chunks that several containers take in turn, and keeps
the ~190,000 intermediate and loose files on the volume instead of the PC's folder (a Docker Desktop bind mount writes
~50 files a second; the volume thousands).

  plan <containers> <procs>
                       once: SPIRV-Cross (build_seed.sh's pin and flags; reused from /out/spirv-cross when there),
                       every shader of the RomFS and the modules (prepare_shader_seed.inventory, walked by <procs>
                       processes; located; plan, as prepare() calls them), cut into chunks of whole shaders, largest
                       first, sized for <containers> workers -> /work/plan, /work/bin
  work <services>      in each container: take the next chunk nobody took (mkdir under /work/claims), run prepare() on
                       it with <services> translators (prepare() runs min(8, os.cpu_count()) of them), reflect it into
                       /work/v143 (vk_reflect.convert, its .spv copied beside), write /work/reports/<chunk>.json; again
                       until no chunk is left
  pack <version> [sb]  once, after every worker: checks that every chunk has its report and the same translator,
                       SPIRV-Cross, list and RomFS; at least prepare_shader_seed.MIN_SHADERS startup shaders; writes
                       /out/shaders/<version>/sb-align.txt, then seed.vkpack (pack_seed, last: it marks the step done),
                       and /out/seed-report.json (what seed/seed-manifest.json said: counts, the skipped variants)

A shader's jobs (its default bind state, then the shared list's) depend only on that shader's own results and write
only files named after it, so translating chunk by chunk gives the same files as prepare() over the whole game.
"""
import concurrent.futures, contextlib, io, json, os, pickle, shutil, struct, subprocess, sys, time, traceback
from pathlib import Path

sys.path.insert(0, '/android/games/totk/android'); sys.path.insert(0, '/android/games/totk/scripts')
import prepare_shader_seed as pss   # noqa: E402
import vk_reflect                   # noqa: E402
from pack_shader_seed import pack_seed   # noqa: E402
from shader_list import ShaderList  # noqa: E402

WORK, OUT = Path('/work'), Path('/out')
PLAN, CLAIMS, REPORTS, V = WORK / 'plan', WORK / 'claims', WORK / 'reports', WORK / 'v143'
BIN = WORK / 'bin'
FAILED = WORK / 'failed'
SPIRV_CROSS_PIN = 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'   # build_seed.sh
SPIRV_CROSS_UNITS = ('main spirv_cfg spirv_cpp spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_hlsl '
                     'spirv_msl spirv_parser spirv_reflect').split()
IDENTITY = ('schema', 'chain', 'version', 'game', 'source_romfs_sha256', 'translator_sha256', 'gather_bias',
            'early_fragment_tests_policy', 'spirv_cross_sha256', 'preparation_sha256', 'shader_list_sha256', 'spirv')


def say(*a):
    print(time.strftime('[%H:%M:%S]'), *a, flush=True)


def spirv_cross():
    """/work/bin/spirv-cross: the seed folder's own (/out/spirv-cross/spirv-cross, fill_shaders_ohos.py uses it), else
    built as build_seed.sh builds it (in /tmp; only the program goes to /out)."""
    mine = OUT / 'spirv-cross' / 'spirv-cross'
    if not (mine.is_file() and os.access(mine, os.X_OK)):
        src, obj = Path('/tmp/spirv-cross.src'), Path('/tmp/spirv-cross.obj')
        shutil.rmtree(src, ignore_errors=True); shutil.rmtree(obj, ignore_errors=True); obj.mkdir()
        subprocess.run(['git', 'clone', '-q', 'https://github.com/KhronosGroup/SPIRV-Cross.git', str(src)], check=True)
        subprocess.run(['git', '-C', str(src), 'checkout', '-q', SPIRV_CROSS_PIN], check=True)
        def cc(unit):
            subprocess.run(['nice', '-n', '10', 'g++', '-O3', '-DNDEBUG', '-std=c++11', '-Wno-deprecated-declarations', '-c',
                            str(src / f'{unit}.cpp'), '-o', str(obj / f'{unit}.o')], check=True)
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
            list(pool.map(cc, SPIRV_CROSS_UNITS))
        built = Path('/tmp/spirv-cross')
        subprocess.run(['g++', *sorted(str(p) for p in obj.glob('*.o')), '-o', str(built)], check=True)
        mine.parent.mkdir(parents=True, exist_ok=True)
        part = mine.with_name('spirv-cross.partial')
        shutil.copyfile(built, part); os.chmod(part, 0o755); os.replace(part, mine)
        say('SPIRV-Cross built:', mine)
    shutil.copyfile(mine, BIN / 'spirv-cross'); os.chmod(BIN / 'spirv-cross', 0o755)


def chunk_keys(jobs, chunks):
    """Whole shaders into chunks: heaviest shaders (most jobs) first, chunk sizes falling from 1/(2*chunks) of the work
    to a floor of 1/(12*chunks), so the last chunks, which decide when the step ends, are small."""
    weight = {}
    for key, *_ in jobs:
        weight[key] = weight.get(key, 0) + 1
    order = sorted(weight, key=lambda k: (-weight[k], k))
    total = sum(weight.values()); floor = max(1, total // (12 * chunks))
    out, cur, size, remaining = [], [], 0, total
    target = max(floor, remaining // (2 * chunks))
    for key in order:
        cur.append(key); size += weight[key]
        if size >= target:
            out.append((cur, size)); remaining -= size
            cur, size = [], 0
            target = max(floor, remaining // (2 * chunks))
    if cur:
        out.append((cur, size))
    return out


# prepare_shader_seed.inventory's source (LF line endings) that _visit / _walk / inventory() below follow step for step; with
# any other version of it the plan calls it as it is (one process: about 3 minutes instead of under one)
INVENTORY_SOURCE_SHA256 = 'e2af0b4623f1c8e9de6e7a85b550c36433c20bede6dd691a5122d3c1e83ede94'


def inventory_source_sha256():
    import ast, hashlib
    text = Path(pss.__file__).read_text(encoding='utf-8').replace('\r\n', '\n')
    for node in ast.parse(text).body:
        if isinstance(node, ast.FunctionDef) and node.name == 'inventory':
            return hashlib.sha256(ast.get_source_segment(text, node).encode()).hexdigest()
    return None


def _visit(found, startup, decode, name, data, keys, game, depth=0):
    """prepare_shader_seed.inventory's visit(): found = its `shaders` (first sighting kept), startup the agl keys."""
    if data[:4] == b'BAHS':
        for key, stage, control, code in pss.sharcb_shaders(data, game):
            found.setdefault(key, (stage, control, code, name))
            keys.add(key); startup.add(key)
    if data[:4] == b'SARC':
        if depth > 4:
            return
        for member, raw in pss.sarc(data).items():
            _visit(found, startup, decode, name + '/' + member, decode(raw), keys, game, depth + 1)
        return
    pos = 0
    while True:
        pos = data.find(b'BNSH\0\0\0\0', pos)
        if pos < 0:
            break
        size = struct.unpack_from('<I', pss.checked(data, pos + 28, 4))[0]
        for key, stage, control, code in pss.bnsh_shaders(pss.checked(data, pos, size), game):
            found.setdefault(key, (stage, control, code, name))
            keys.add(key)
        pos += max(size, 8)


def _walk(task):
    """inventory()'s loop over one run of RomFS files (rows in name order) -> (shaders first seen in the run, in order;
    the keys it adds to startup; its archives, in order)."""
    romfs, rows, zsdic, game = task
    found, startup, archives = {}, set(), []
    with open(romfs / 'romfs.bin', 'rb') as stream:
        def read_at(offset, size):
            stream.seek(offset)
            raw = stream.read(size)
            if len(raw) != size:
                raise ValueError('short RomFS shader read')
            return raw
        decode = pss.decoder(romfs, lambda name: read_at(*zsdic[name]), game)
        for name, offset, size in rows:
            raw = read_at(offset, size)
            if raw[:4] not in (pss.ZSTD, pss.YAZ0, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
                continue
            data = decode(raw)
            keys = set()
            _visit(found, startup, decode, name, data, keys, game)
            if pss.startup_package(name, game):
                startup |= keys
            if keys or pss.startup_package(name, game):
                archives.append(dict(path=name, sha256=pss.digest(raw)))
    return found, startup, archives


def inventory(romfs, modules, index, procs, game='totk'):
    """prepare_shader_seed.inventory(romfs, modules, game) in <procs> processes: the files cut into runs in name order, each
    run walked as inventory() walks it, the results joined in that order (a shader keeps its first sighting's source), then
    the game modules as inventory() reads them. The same shaders, startup keys and archives; the RomFS is read by several
    streams at once (a Docker Desktop bind mount gives one stream ~150 MB/s, eight ~550)."""
    if procs <= 1 or inventory_source_sha256() != INVENTORY_SOURCE_SHA256:
        print('prepare_shader_seed.inventory is not the version the parallel walk follows: one process', flush=True)
        return pss.inventory(romfs, modules, game)
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    folders = pss.GAMES[game]['folders']
    rows = [(n, entries[n]['offset'], entries[n]['size']) for n in sorted(entries) if not (folders and not n.startswith(folders))]
    zsdic = {'Pack/ZsDic.pack.zs': (entries['Pack/ZsDic.pack.zs']['offset'], entries['Pack/ZsDic.pack.zs']['size'])}
    total = sum(r[2] for r in rows); step = max(1, total // (procs * 6))
    tasks, cur, size = [], [], 0
    for r in rows:
        cur.append(r); size += r[2]
        if size >= step:
            tasks.append((romfs, cur, zsdic, game)); cur, size = [], 0
    if cur:
        tasks.append((romfs, cur, zsdic, game))
    shaders, startup, archives = {}, set(), []
    with concurrent.futures.ProcessPoolExecutor(procs) as pool:
        for found, keys, kept in pool.map(_walk, tasks):   # in order
            for key, value in found.items():
                shaders.setdefault(key, value)
            startup |= keys
            archives += kept
    with open(romfs / 'romfs.bin', 'rb') as stream:
        def read(name):
            stream.seek(entries[name]['offset']); return stream.read(entries[name]['size'])
        decode = pss.decoder(romfs, read, game)
        for module in sorted(Path(modules).glob('*.patched.bin')) if modules else []:
            keys = set()   # the game module embeds a few BNSH files
            _visit(shaders, startup, decode, 'module:' + module.name[:-len('.patched.bin')], module.read_bytes(), keys, game)
            startup |= keys
    print(f'discovered {len(shaders)} unique shaders, {len(startup)} of them translated for the default bind state', flush=True)
    if not startup:
        raise ValueError('no startup shaders discovered')
    return index, shaders, startup, archives


def plan(chunks, procs):
    romfs, modules, xlate = Path('/in/romfs'), Path('/in/modules'), Path(os.environ['SEED_XLATE'])
    t0 = time.monotonic()
    for p in WORK.iterdir():   # build_seed.py gives a new volume; nothing of an earlier run may count for this one
        shutil.rmtree(p) if p.is_dir() and not p.is_symlink() else p.unlink()
    for d in (PLAN, CLAIMS, REPORTS, V, BIN):
        d.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(1) as side:
        sc = side.submit(spirv_cross)   # cloned and compiled while the RomFS is read
        shutil.copyfile(xlate, BIN / 'totk_xlate'); os.chmod(BIN / 'totk_xlate', 0o755)
        index = json.loads((romfs / 'index.json').read_text())
        if index.get('status') != 'pass':
            raise ValueError('RomFS has not passed integrity verification')
        # what prepare() reads of the RomFS once inventory() is given (its identity): the folder's checked hash
        (PLAN / 'romfs').mkdir(exist_ok=True)
        (PLAN / 'romfs' / 'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256=index['romfs_sha256'])))
        shared = ShaderList.load(pss.GAMES['totk']['list'])
        _, shaders, startup, archives = inventory(romfs, modules, index, procs)
        del index
        extra = pss.located({where for where in shared.locators if where[0] not in shaders},
                            pss.source_reader(romfs, modules, 'totk'), 'totk')
        print(f'{len(extra)} more shaders read where the shared list locates them (agl .sharcb, game module)', flush=True)
        everything = {**shaders, **extra}
        jobs = pss.plan(everything, startup, shared.variants)
        cut = chunk_keys(jobs, chunks)
        for i, (keys, size) in enumerate(cut):
            data = dict(shaders={k: shaders[k] for k in keys if k in shaders}, extra={k: extra[k] for k in keys if k in extra},
                        startup=sorted(k for k in keys if k in startup), jobs=size)
            with open(PLAN / f'chunk-{i:04d}.pkl', 'wb') as f:
                pickle.dump(data, f, protocol=pickle.HIGHEST_PROTOCOL)
        summary = dict(chunks=len(cut), jobs=len(jobs), keys=len({j[0] for j in jobs}), discovered=len(everything),
                       located=len(extra), startup=len(startup), listed_variants=sum(1 for *_, why in jobs if why == 'listed'),
                       archives=archives)
        (PLAN / 'plan.json').write_text(json.dumps(summary, separators=(',', ':')))
        sc.result()
    say(f'planned {len(jobs)} translations of {summary["keys"]} shaders ({len(startup)} startup) in {len(cut)} chunks, '
        f'{time.monotonic() - t0:.0f} s')


def work(services):
    """Chunks until none is left; a failure anywhere (/work/failed) stops the others after their current chunk."""
    os.cpu_count = lambda: services                      # prepare(): min(8, os.cpu_count()) translators
    pss.GAMES['totk']['min_shaders'] = 0                 # a chunk has a share of them; pack checks the whole seed
    pss.source_reader = lambda *a, **k: None             # the plan read the RomFS once
    translator, sc = BIN / 'totk_xlate', BIN / 'spirv-cross'
    version = os.environ['SEED_VERSION']
    names = sorted(p.stem for p in PLAN.glob('chunk-*.pkl'))
    mine = 0
    for name in names:
        if FAILED.exists():
            return 1
        try:
            (CLAIMS / name).mkdir()
        except FileExistsError:
            continue
        t0 = time.monotonic()
        with open(PLAN / f'{name}.pkl', 'rb') as f:
            data = pickle.load(f)
        pss.inventory = lambda *a, **k: (None, dict(data['shaders']), set(data['startup']), [])
        pss.located = lambda *a, **k: dict(data['extra'])
        seed = Path('/tmp') / name
        log = io.StringIO()
        try:
            with contextlib.redirect_stdout(log):
                report = pss.prepare(PLAN / 'romfs', seed, translator, sc, version, game='totk', keep_spirv=True)
                n, bad, unknown = vk_reflect.convert(seed, V)
                for p in sorted(seed.glob('*.spv')):
                    shutil.copy2(p, V / p.name)
        except BaseException:
            FAILED.write_text(name)
            print(log.getvalue()[-4000:]); traceback.print_exc()
            return 1
        rep = dict(chunk=name, seconds=round(time.monotonic() - t0, 1), jobs=data['jobs'], variants=report['shaders'],
                   startup_shaders=report['startup_shaders'], listed_variants=report['listed_variants'],
                   skipped=report['skipped'], reflected=n, incomplete=bad, unknown=unknown,
                   identity={k: report.get(k) for k in IDENTITY})
        part = REPORTS / f'{name}.partial'
        part.write_text(json.dumps(rep)); os.replace(part, REPORTS / f'{name}.json')
        shutil.rmtree(seed)
        mine += 1
        say(f'{name}: {report["shaders"]} variants, {len(report["skipped"])} skipped, {data["jobs"]} jobs, {rep["seconds"]} s')
    say(f'no chunk left ({mine} done here)')
    return 0


def pack(version, sb_align):
    t0 = time.monotonic()
    summary = json.loads((PLAN / 'plan.json').read_text())
    names = [f'chunk-{i:04d}' for i in range(summary['chunks'])]
    missing = [n for n in names if not (REPORTS / f'{n}.json').is_file()]
    if missing or FAILED.exists():
        sys.exit(f'chunks without a result: {missing[:5]}{" ..." if len(missing) > 5 else ""}; failed: '
                 f'{FAILED.read_text() if FAILED.exists() else "-"}')
    reports = [json.loads((REPORTS / f'{n}.json').read_text()) for n in names]
    identity = reports[0]['identity']
    odd = [r['chunk'] for r in reports if r['identity'] != identity]
    if odd:
        sys.exit(f'chunks translated with other inputs: {odd[:5]}')
    import hashlib
    this_run = dict(translator_sha256=hashlib.sha256((BIN / 'totk_xlate').read_bytes()).hexdigest(),
                    spirv_cross_sha256=hashlib.sha256((BIN / 'spirv-cross').read_bytes()).hexdigest(),
                    source_romfs_sha256=json.loads((PLAN / 'romfs' / 'index.json').read_text())['romfs_sha256'], version=version)
    other = {k: (identity.get(k), v) for k, v in this_run.items() if identity.get(k) != v}
    if other:
        sys.exit(f'the chunks were not translated with this run\'s inputs: {other}')
    variants = sum(r['variants'] for r in reports); ready = sum(r['startup_shaders'] for r in reports)
    listed = sum(r['listed_variants'] for r in reports)
    if listed != summary['listed_variants']:
        sys.exit(f'chunks planned {listed} listed variants, the whole plan {summary["listed_variants"]}')
    if ready < pss.MIN_SHADERS:
        sys.exit(f'only {ready} shaders prepared; at least {pss.MIN_SHADERS} are required for first-launch warm-up')
    n = sum(r['reflected'] for r in reports); bad = sum(r['incomplete'] for r in reports)
    unknown = {}
    for r in reports:
        for k, c in r['unknown'].items():
            unknown[k] = unknown.get(k, 0) + c
    # prepare()'s order: the default jobs by shader, then the listed ones by shader and answers; a shader's jobs are all
    # in one chunk, in that order, so a stable sort by (default first, shader) gives it back
    skipped = sorted((s for r in reports for s in r['skipped']), key=lambda s: (s['why'] != 'default', s['key']))
    print(f'startup seed ready: {variants} shader variants ({ready} startup shaders, the rest from the shared list), '
          f'{len(skipped)} skipped', flush=True)
    print('variants', n, 'incomplete', bad, 'unclassified', dict(sorted(unknown.items(), key=lambda kv: (-kv[1], kv[0]))[:5]), flush=True)
    dst = OUT / 'shaders' / version
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    if sb_align:
        (dst / 'sb-align.txt').write_text(f'{sb_align}\n')
    report = dict(**identity, shaders=variants, discovered=summary['discovered'], located=summary['located'],
                  startup_shaders=ready, listed_variants=listed, skipped=skipped, archives=summary['archives'],
                  chunks=[dict(chunk=r['chunk'], jobs=r['jobs'], seconds=r['seconds']) for r in reports])
    (OUT / 'seed-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('packed', pack_seed(V, dst / 'seed.vkpack'), f'in {time.monotonic() - t0:.0f} s', flush=True)


def main(argv):
    if argv[:1] == ['plan'] and len(argv) == 3:
        return plan(int(argv[1]), int(argv[2]))
    if argv[:1] == ['work'] and len(argv) == 2:
        return work(int(argv[1]))
    if argv[:1] == ['pack'] and len(argv) in (2, 3):
        return pack(argv[1], argv[2] if len(argv) == 3 else '')
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
