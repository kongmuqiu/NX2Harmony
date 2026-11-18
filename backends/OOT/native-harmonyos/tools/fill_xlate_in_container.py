"""Container half of fill_shaders_ohos.py (python:3.14, /android = the reference, read-only).
Translates the device's shader requests as android/fill_shaders.py does: the translator's --service mode with the seed's
SPIRV-Cross and chain, then the Vulkan reflection (android/vk_reflect.py). The translator is the HarmonyOS build
(native-harmonyos/xlate, TOTK_XLATE_SB_ALIGN from the environment: the same alignment as the seed). <variant>.spv and .vk
go to <out>; <key>.vv is the seed's own list (<seed>/<key>.vv, when there) plus the new row, written to <out> and <seed>.
usage: fill_xlate_in_container.py <requests dir> <seed shaders/<version> dir> <out dir> <translator> <spirv-cross>

TOTK_VK_DEPS=queried (Z-A): a variant's texture dependencies are only the slots the translator asked the type of
(res.json tex_queried: Ryujinx asks for texture buffers and size / sample-count queries only, BindlessElimination
rewriteSamplerType). Service.cs also makes every sampled slot a dependency, because a depth texture there changes the
Metal declaration; the SPIR-V does not change (Program.cs drops the depth class), so on Vulkan those dependencies only
turned another bound texture kind into a miss: 217 of the first 220 Z-A shaders filled again came out byte-identical.
A row whose dependencies an existing row already has is not added (the device would have matched that one).
"""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
VK_DEPS = os.environ.get('TOTK_VK_DEPS') == 'queried'
sys.path.insert(0, '/android/games/totk/android'); sys.path.insert(0, '/android/games/totk/scripts')
import vk_reflect
from prepare_shader_seed import CHAIN
import re

# Switch 2 (requests with arch sm86): bindings the reference's reflection leaves unknown (kind 0), named here with the
# HarmonyOS replay's kinds (overlay android/gfx/vk_int.h RB_PUSH / RB_GMEM / RB_GMEML): the push constants, which NVN2
# keeps in c[24] (the translator maps compute's c[0] push words there too, InstEmitAmpere AMapC0), and the two storage
# buffers the translator reserves under the sentinel constant buffer slot 0xff for global memory reached through a
# loaded address (GlobalToStorage.TryGenerateRawStorageOp): offset 0 the memory pool table, offset 1 its write log.
RB_PUSH, RB_GMEM, RB_GMEML = 15, 16, 17
RB_SBUF = 6
# Bindless pools (overlay android/gfx/vk_int.h RB_POOLTEX / RB_POOLSAMP; prepare_xlate_ns2.py): the translator's environment
POOL_SET = int(os.environ.get('TOTK_XLATE_POOL_SET', '-1'))
POOL_TEX = int(os.environ.get('TOTK_XLATE_POOL_TEX', '32'))
POOL_SAMP = int(os.environ.get('TOTK_XLATE_POOL_SAMP', '32'))
RB_POOLTEX, RB_POOLSAMP, RB_POOLUSED = 18, 19, 20
def ns2_kinds(r, res):
    sbufs = sorted(res.get('sbufs') or [], key=lambda b: b['binding'])   # as the reference's vk_reflect.py indexes them
    named = set()
    for b in r['binds']:
        if b['kind']:
            continue
        m = re.match(r'^[a-z]+_c(\d+)$', b['name']); ms = re.match(r'^[a-z]+_s(\d+)$', b['name'])
        if m and int(m.group(1)) == 24 and b['dtype'] == vk_reflect.UNIFORM_BUFFER:
            b['kind'] = RB_PUSH
        elif ms and b['dtype'] == vk_reflect.STORAGE_BUFFER:
            i = int(ms.group(1)); sb = sbufs[i] if i < len(sbufs) else None
            if sb and sb.get('cb_slot') == 0xff:
                b.update(kind=RB_GMEM if sb.get('cb_offset') == 0 else RB_GMEML, rw=int(sb.get('cb_offset') != 0))
            elif sb and sb.get('cb_slot') == 0 and 0x670 <= sb.get('cb_offset', 0) * 4 < 0x870 and (sb['cb_offset'] * 4 - 0x670) % 16 == 0:
                # an NVN2 task or mesh kernel translated as compute (oot/oot_mesh.py): the addresses of its storage buffers are
                # in c0's task table (0x670) or mesh table (0x770), 16 slots of 16 bytes (nxnative dmsh_abi.h DMSH_C0_SSBO_TS /
                # DMSH_C0_SSBO_MS). The reference's reflection knows the vertex, compute and pixel tables only, so these had
                # no kind and the replay would bind zeros. The slot, and the table's base in off: the replay takes the stage
                # from the program, and off lets the seed be audited (a task kernel reads 0x670 only, a mesh kernel 0x770)
                off = sb['cb_offset'] * 4; table = 0x670 if off < 0x770 else 0x770
                b.update(kind=RB_SBUF, reg=(off - table) // 16, off=table)
        # bindless pools (prepare_xlate_ns2.py, TOTK_XLATE_POOL_*): whole-pool arrays in descriptor set 2, told apart by their
        # lengths; the binding is the class (0 samplers, 1 2D, 2 2D array, 3 3D, 4 cube, 5 cube array textures)
        if POOL_SET >= 0 and b['count'] == POOL_TEX and b['dtype'] == vk_reflect.SAMPLED_IMAGE:
            b['kind'] = RB_POOLTEX
        elif POOL_SET >= 0 and b['count'] == POOL_SAMP and b['dtype'] == vk_reflect.SAMPLER:
            b['kind'] = RB_POOLSAMP
        elif POOL_SET >= 0 and b['name'] == 'bindless_used':   # the usage bitmap (binding 6 of the bindless set)
            b['kind'] = RB_POOLUSED
        if b['kind']:
            named.add(b['name'])
    r['unknown'] = [u for u in r.get('unknown', []) if u.split(' ', 1)[0] not in named]

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
        res = json.loads((xl / f'{variant}.res.json').read_text())
        r = vk_reflect.reflect((xl / f'{variant}.spv').read_bytes(), res, stage)
        if rq.get('arch') == 'sm86':
            ns2_kinds(r, res)
        deps = refl.get('deps') or {}
        if VK_DEPS:
            asked = {str(s) for s in res.get('tex_queried') or []}
            deps = {**deps, 'tex': {s: v for s, v in (deps.get('tex') or {}).items() if s in asked}}
        if refl.get('threadgroup_size'):
            r['local'] = tuple(refl['threadgroup_size'])
        shutil.copy2(xl / f'{variant}.spv', out / f'{variant}.spv')
        (out / f'{variant}.vk').write_text(vk_reflect.vk_text(r, stage, refl.get('c1', '')))
        vv_seed = seed / f'{key}.vv'
        rows = vv_seed.read_text().splitlines() if vv_seed.is_file() else []
        flat = vk_reflect.flat_deps(deps)
        row = f'{variant}\t{flat}'
        if row not in rows and not (VK_DEPS and any(r_.split('\t', 1)[1:] == [flat] or (r_.find('\t') < 0 and not flat) for r_ in rows)):
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
