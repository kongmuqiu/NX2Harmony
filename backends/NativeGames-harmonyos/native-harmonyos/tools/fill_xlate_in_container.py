"""Container half of fill_shaders_ohos.py (python:3.14, /android = the reference, read-only).
Translates the device's shader requests as android/fill_shaders.py does: the translator's --service mode with the seed's
SPIRV-Cross and chain, then the Vulkan reflection (android/vk_reflect.py). The translator is the HarmonyOS build
(native-harmonyos/xlate, TOTK_XLATE_SB_ALIGN from the environment: the same alignment as the seed). <variant>.spv and .vk
go to <out>; <key>.vv is the seed's own list (<seed>/<key>.vv, when there, else the one packed in <seed>/seed.vkpack: a
seed from build_seed.py has no loose files) plus the new row, written to <out> and <seed>. The device takes a loose .vv
over the packed one, so the loose one must keep the packed rows.
usage: fill_xlate_in_container.py <requests dir> <seed shaders/<version> dir> <out dir> <translator> <spirv-cross>
"""
import json, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, '/android/games/totk/android'); sys.path.insert(0, '/android/games/totk/scripts')
import vk_reflect
from prepare_shader_seed import CHAIN

_packed = None


def packed_text(seed, name):
    """A file's text from <seed>/seed.vkpack (pack_shader_seed.py v1: <8sIIQQ> header, sorted <84sIQQ> records), or None."""
    global _packed
    pack = seed / 'seed.vkpack'
    if _packed is None:
        _packed = {}
        if pack.is_file():
            with pack.open('rb') as f:
                magic, version, size, count, _ = struct.unpack('<8sIIQQ', f.read(32))
                if magic == b'TOTKVKP1' and version == 1 and size == 104:
                    for _ in range(count):
                        raw, _, offset, length = struct.unpack('<84sIQQ', f.read(104))
                        _packed[raw.rstrip(b'\0').decode('ascii')] = (offset, length)
    if name not in _packed:
        return None
    offset, length = _packed[name]
    with pack.open('rb') as f:
        f.seek(offset)
        return f.read(length).decode()

req_dir, seed, out, translator, spirv_cross = (Path(p) for p in sys.argv[1:6])
out.mkdir(parents=True, exist_ok=True)
names = sorted(p.name for p in req_dir.glob('*.json'))
print(f'{len(names)} requests', flush=True)
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp); xl = tmp / 'out'; xl.mkdir()
    proc = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross), '--out', str(xl), '--chain', CHAIN],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    done, failed = [], []
    for n in names:
        rq = json.loads((req_dir / n).read_text())
        code = tmp / f'{rq["variant"]}.code'; code.write_bytes(bytes.fromhex(rq.pop('code_hex')))
        rq['code'] = str(code)
        proc.stdin.write(json.dumps(rq) + '\n'); proc.stdin.flush()
        reply = json.loads(proc.stdout.readline())
        (done if reply.get('ok') else failed).append((n, rq, reply.get('error')))
    proc.stdin.close(); proc.wait()
    files = []
    for n, rq, _ in done:
        variant = rq['variant']; key = variant.split('-')[0]
        stage = vk_reflect.STAGES[rq['stage']]
        refl = json.loads((xl / f'{variant}_b.json').read_text())
        r = vk_reflect.reflect((xl / f'{variant}.spv').read_bytes(), json.loads((xl / f'{variant}.res.json').read_text()), stage)
        if refl.get('threadgroup_size'):
            r['local'] = tuple(refl['threadgroup_size'])
        shutil.copy2(xl / f'{variant}.spv', out / f'{variant}.spv')
        (out / f'{variant}.vk').write_text(vk_reflect.vk_text(r, stage, refl.get('c1', '')))
        vv_seed = seed / f'{key}.vv'
        text = vv_seed.read_text() if vv_seed.is_file() else packed_text(seed, f'{key}.vv')
        rows = text.splitlines() if text else []
        row = f'{variant}\t{vk_reflect.flat_deps(refl.get("deps") or {})}'
        if row not in rows:
            rows.append(row)
        text = '\n'.join(rows) + '\n'
        (out / f'{key}.vv').write_text(text); vv_seed.write_text(text)
        shutil.copy2(out / f'{variant}.spv', seed / f'{variant}.spv'); shutil.copy2(out / f'{variant}.vk', seed / f'{variant}.vk')
        files += [f'{variant}.spv', f'{variant}.vk', f'{key}.vv']
    (out / 'filled.json').write_text(json.dumps({'done': [n for n, _, _ in done], 'files': sorted(set(files)),
                                                 'failed': [[n, e] for n, _, e in failed]}, indent=1))
    print(f'filled {len(done)} variants, {len(failed)} failed', flush=True)
    for n, _, e in failed[:20]:
        print(f'  {n}: {e}')
