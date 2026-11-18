"""A full Pokémon Legends: Z-A shader seed from the device's shader dump, instead of filling it miss by miss.

  1. --pull: the device's files/android/za-dump/bnsh.pack (written by runtime/za_shader_dump.c with ZA_SHADER_DUMP: every
     file of arc/data.trpfs that holds BNSH shader files, decoded with the game's own Oodle) -> prepared/za/shader-dump/
  2. every BNSH in it and in main (40 more, nxnative's count) -> its programs, with the Android port's own parser and keys
     (games/totk/scripts/prepare_shader_seed.py bnsh_shaders: the same key the device computes)
  3. one request per shader for the default bind state, in the device's request format, with near = 0 depth (Z-A sets
     nvnDeviceSetDepthMode near = 0) - the variants the seed already has are skipped
  4. translated by --jobs python:3.14 containers at once (native-harmonyos/tools/fill_xlate_in_container.py, the
     HarmonyOS translator, sb-align from the seed) into prepared/za/seed-build/shaders/za202
  5. --send: packed into seed.vkpack (the Android port's pack_shader_seed.py) and sent to the device in one file
  Variants of other bind states are still met on the device and filled with fill_shaders_ohos.py, far fewer of them.
usage: python native-harmonyos/za/za_seed.py [--pull] [--jobs 8] [--send]
Everything it writes is game-derived and stays under prepared/ (ignored)."""
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from ohos_env import HDC, DEV, UPSTREAM, REPO, docker_path   # noqa: E402
sys.path.insert(0, str(UPSTREAM / 'games' / 'totk' / 'scripts'))
import prepare_shader_seed as pss   # noqa: E402
sys.path.insert(0, str(HERE))
import disas   # noqa: E402

BUNDLE = os.environ.get('TOTK_OHOS_BUNDLE') or 'LZA.NativeGames.harmonyos'
FILES = '/data/storage/el2/base/haps/entry/files/android'
DUMP = PORT / 'prepared' / 'za' / 'shader-dump'
BUILD = PORT / 'prepared' / 'za' / 'seed-build'
SEED = BUILD / 'shaders' / 'za202'
ANSWERS = dict(attr={}, fout={}, img={}, tex={})
SUFFIX = pss.suffix(ANSWERS)


def hdc(*args):
    r = subprocess.run([HDC, *DEV, *args], capture_output=True, text=True, errors='replace')
    return (r.stdout + r.stderr).strip()


def blobs(pack):
    d = pack.read_bytes()
    if d[:4] != b'ZABN' or struct.unpack_from('<I', d, 4)[0] != 1:
        sys.exit(f'{pack}: not a Z-A shader dump')
    at = 8
    while at < len(d):
        h, n = struct.unpack_from('<QQ', d, at); at += 16
        yield f'{h:016x}', d[at:at + n]; at += n


def shaders_in(data, name, out, bad):
    pos = 0
    while True:
        pos = data.find(b'BNSH\0\0\0\0', pos)
        if pos < 0:
            return
        size = struct.unpack_from('<I', data, pos + 28)[0] if pos + 32 <= len(data) else 0
        try:
            for key, stage, control, code in pss.bnsh_shaders(data[pos:pos + size]):
                out.setdefault(key, (stage, control, code, name))
        except (ValueError, struct.error) as e:
            bad.append(f'{name}@{pos:#x}: {e}')
        pos += max(size, 8)


def request(key, stage, control, code):
    c1, length = struct.unpack_from('<II', control, 0x700)
    rq = dict(variant=f'{key}-{SUFFIX}', stage=stage, depth_minus_w=False,
              early_fragment_tests=stage == 'ps' and len(control) == 0x880 and bool(control[0x718]),
              c1=code[c1:length].hex(), code_hex=code[0 if stage == 'cs' else 0x30:length].hex(), **ANSWERS)
    if stage == 'cs':
        rq['local'] = list(struct.unpack_from('<III', control, 0x748))
    return rq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pull', action='store_true', help='pull bnsh.pack from the device first')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) // 2))
    ap.add_argument('--send', action='store_true', help='pack the seed and send seed.vkpack to the device')
    a = ap.parse_args()
    DUMP.mkdir(parents=True, exist_ok=True)
    pack = DUMP / 'bnsh.pack'
    if a.pull:
        print(hdc('file', 'recv', '-b', BUNDLE, f'{FILES}/za-dump/bnsh.pack', str(pack)).splitlines()[-1])
    shaders, bad = {}, []
    nblob = 0
    for name, data in blobs(pack):
        nblob += 1
        shaders_in(data, name, shaders, bad)
    shaders_in(disas.load('main'), 'module:main', shaders, bad)
    count = {s: sum(1 for v in shaders.values() if v[0] == s) for s in ('vs', 'ps', 'cs')}
    print(f'{nblob} files with shaders: {len(shaders)} unique shaders ({count["vs"]} vertex, {count["ps"]} pixel, {count["cs"]} compute); {len(bad)} unreadable BNSH')
    for b in bad[:10]:
        print('  ', b)
    todo = [(k, v) for k, v in sorted(shaders.items()) if not (SEED / f'{k}-{SUFFIX}.spv').is_file()]
    print(f'{len(todo)} to translate for the default bind state ({len(shaders) - len(todo)} already in the seed)')
    if todo:
        reqs = DUMP / 'requests'
        if reqs.exists():
            shutil.rmtree(reqs)
        shards = [reqs / f'shard{i:02d}' for i in range(a.jobs)]
        for s in shards:
            (s / 'in').mkdir(parents=True)
        for i, (key, (stage, control, code, _)) in enumerate(todo):
            rq = request(key, stage, control, code)
            (shards[i % a.jobs] / 'in' / f'{rq["variant"]}.json').write_text(json.dumps(rq))
        align = (SEED / 'sb-align.txt').read_text().strip()
        rel = lambda p: '/repo/' + p.resolve().relative_to(REPO).as_posix()
        env = {**os.environ, 'MSYS_NO_PATHCONV': '1'}
        t0 = time.monotonic(); procs = []
        for s in shards:
            cmd = ['docker', 'run', '--rm', '-v', docker_path(UPSTREAM) + ':/android:ro', '-v', docker_path(REPO) + ':/repo',
                   '-e', f'TOTK_XLATE_SB_ALIGN={align}', '-e', 'TOTK_VK_DEPS=queried', '-e', 'TOTK_XLATE_DRAW_PARAMS=1', 'python:3.14', 'python3', '/repo/native-harmonyos/tools/fill_xlate_in_container.py',
                   rel(s / 'in'), rel(SEED), rel(s / 'out'), '/repo/artifacts/xlate-ohos/totk_xlate', rel(BUILD / 'spirv-cross' / 'spirv-cross')]
            procs.append((s, subprocess.Popen(cmd, stdout=(s / 'log.txt').open('w'), stderr=subprocess.STDOUT, env=env)))
        done = failed = 0
        for s, p in procs:
            p.wait()
            f = s / 'out' / 'filled.json'
            if p.returncode or not f.is_file():
                print(f'{s.name}: the container failed ({p.returncode}); see {s / "log.txt"}'); continue
            r = json.loads(f.read_text()); done += len(r['done']); failed += len(r['failed'])
            for n, e in r['failed'][:3]:
                print(f'  {s.name} {n}: {e}')
        print(f'translated {done} variants, {failed} failed, in {time.monotonic() - t0:.0f} s')
    if a.send:
        out = BUILD / 'seed.vkpack'
        r = subprocess.run([sys.executable, str(UPSTREAM / 'games' / 'totk' / 'scripts' / 'pack_shader_seed.py'), str(SEED), '--output', str(out)],
                           capture_output=True, text=True)
        print((r.stdout + r.stderr).strip()[-400:])
        if r.returncode:
            return 1
        print(hdc('file', 'send', '-b', BUNDLE, str(out), f'{FILES}/shaders/za202/seed.vkpack').splitlines()[-1])
    return 0


if __name__ == '__main__':
    sys.exit(main())
