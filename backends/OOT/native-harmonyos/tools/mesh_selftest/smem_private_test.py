"""Checks of smem_private.py (a game compute shader's per-thread slots of shared memory made a private array), on lavapipe.
PROTOTYPE, oot-a106.

1. Small compute shaders written the way the translator writes a thread's local memory (one shared array, the byte
   address 'thread number * S + K', then >> 2), compiled and brought to the seed's form (glslang, then the passes of
   tools/spirv_opt_seed.sh): one that must be rewritten and must write the same words as before, and the forms that
   must be REFUSED: a thread reading its neighbour's slot, an atomic on the array, two threads on one slot, an offset
   past the slot, a thread number that is not linear in gl_LocalInvocationID.
2. With a module of the seed (game-derived, so it is an argument): the module and its rewrite run on the same synthetic
   inputs (uniform words of one value or pseudo-random floats, storage words of one value, small pseudo-random
   textures), every storage buffer and every storage image compared byte for byte.

  docker run --rm -v <port>:/repo mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/smem_private_test.py \\
      [<module.spv> <spirv-cross> [gx gy gz]]"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import smem_private as sp   # noqa: E402

SEED_PASSES = ['--eliminate-local-single-block', '--eliminate-local-single-store', '--eliminate-local-multi-store', '--eliminate-dead-code-aggressive',
               '--merge-blocks', '--simplify-instructions', '--eliminate-dead-code-aggressive']


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'{" ".join(map(str, cmd))}\n{r.stdout}{r.stderr}')
    return r.stdout


def kernel(variant):
    """8 x 4 threads, a slot of 12 bytes a thread; out.w[thread of the dispatch] = what the thread read back"""
    slot = {'neighbour': '(((t + 1) & 31) * 12)'}                    # what the thread reads back: another thread's slot
    base = {'shared': 'int(gl_LocalInvocationID.x) * 12',            # the four threads of a column on one slot
            'nonlinear': '((t * t) & 31) * 12'}.get(variant, 't * 12')
    k2 = 12 if variant == 'past' else 8
    lines = ['#version 450', 'layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;',
             'layout(set = 0, binding = 0, std430) buffer out_t { uint w[]; } o;',
             'layout(set = 0, binding = 1, std140) uniform in_t { uint u[64]; } i;', 'shared uint m[96];', 'void main()', '{',
             '    int t = int(gl_LocalInvocationID.y) * 8 + int(gl_LocalInvocationID.x);',
             f'    int b = {base};',
             '    uint n = gl_WorkGroupID.x * 32u + uint(t);',
             '    m[uint(b) >> 2u] = n * 3u + i.u[t & 63];',
             '    m[uint(b + 4) >> 2u] = n ^ 0x5555u;',
             '    if ((t & 3) != 1)', '    {', f'        m[uint(b + {k2}) >> 2u] = n + 77u;', '    }']
    if variant == 'atomic':
        lines.append('    atomicAdd(m[uint(b + 4) >> 2u], 1u);')
    if variant in slot:
        lines += [f'    int b2 = {slot[variant]};', '    uint r = m[uint(b2) >> 2u] + m[uint(b2 + 4) >> 2u] * 5u;']
    else:
        lines.append('    uint r = m[uint(b) >> 2u] + m[uint(b + 4) >> 2u] * 5u;')
    lines += [f'    r += m[uint(b + {k2}) >> 2u] * 9u;   // a quarter of the threads did not store it: zero',
              '    o.w[n] = r;', '}', '']
    return '\n'.join(lines)


def synthetic(tmp, csrun):
    bad = 0
    (tmp / 'u.bin').write_bytes(struct.pack('<256I', *[(k * 2654435761) & 0xffffffff for k in range(256)]))
    for variant, taken in (('slot', True), ('neighbour', False), ('shared', False), ('atomic', False), ('past', False), ('nonlinear', False)):
        d = tmp / variant; d.mkdir()
        (d / 'k.comp').write_text(kernel(variant))
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-S', 'comp', str(d / 'k.comp'), '-o', str(d / 'raw.spv')])
        run(['spirv-opt', '--target-env=vulkan1.1', *SEED_PASSES, str(d / 'raw.spv'), '-o', str(d / 'k.spv')])
        try:
            out, what = sp.rewrite((d / 'k.spv').read_bytes())
        except sp.Refused as e:
            out, what = None, f'refused: {e}'
        verdict = ''
        if (out is not None) != taken:
            verdict = 'FAILED: ' + ('rewritten' if out is not None else 'not rewritten'); bad += 1
        elif out is not None:
            (d / 'p.spv').write_bytes(out)
            run(['spirv-val', '--target-env', 'vulkan1.1', str(d / 'p.spv')])
            res = []
            for name in ('k', 'p'):
                rd = d / name; rd.mkdir()
                run([str(csrun), str(d / f'{name}.spv'), '5', '1', '1', '1', '0:s:640', f'1:u:1024:{tmp / "u.bin"}'], cwd=rd)
                res.append((rd / 'out_0.bin').read_bytes())
            words = struct.unpack('<160I', res[0])
            same = res[0] == res[1] and len(set(words)) > 100
            verdict = 'the same 160 words as before' if same else 'FAILED: the words differ'
            bad += not same
        print(f'{variant:10s} {what}: {verdict or "as it must be"}', flush=True)
    return bad


KINDS = {'sampler2D': '2d', 'sampler3D': '3d', 'sampler2DArray': '2da', 'sampler2DArrayShadow': '2das'}
MiB = 1 << 20


def module(tmp, csrun, spv, cross, groups):
    spv = Path(spv)
    glsl = run([cross, '--vulkan-semantics', str(spv)])
    specs, outs = [], []
    for b, rest in re.findall(r'(?m)^layout\(set = 0, binding = (\d+)[^)]*\) (.*)$', glsl):
        t = rest.split()
        if t[0] == 'uniform' and len(t) == 2 and not t[1].endswith(';'):
            specs.append(f'{b}:u:65536:{tmp / "ubo.bin"}')
        elif t[0] == 'buffer' or t[:2] == ['readonly', 'buffer']:
            specs.append(f'{b}:s:{MiB}:{tmp / "ssbo.bin"}'); outs.append(b)
        elif t[0] == 'uniform' and t[1] in KINDS:
            specs.append(f'{b}:t:{KINDS[t[1]]}')
        elif t[:2] == ['uniform', 'writeonly'] and t[2] in ('image3D', 'image2D'):
            specs.append(f'{b}:i:3d:128x72x96' if t[2] == 'image3D' else f'{b}:i:2d:256x256x1'); outs.append(b)
        else:
            print(f'{spv.name[:16]}: a binding this runner does not make ({rest}): not run')
            return 1
    try:
        out, what = sp.rewrite(spv.read_bytes())
    except sp.Refused as e:
        print(f'{spv.name[:16]}: refused: {e}')
        return 1
    (tmp / 'private.spv').write_bytes(out)
    ver = struct.unpack_from('<I', out, 4)[0]
    run(['spirv-val', '--target-env', 'vulkan1.1' if ver <= 0x10300 else 'vulkan1.3', str(tmp / 'private.spv')])
    print(f'{spv.name[:16]}: {what}; spirv-val passes; {len(specs)} bindings, {len(outs)} compared, {"x".join(map(str, groups))} workgroups', flush=True)

    def floats(seed):
        x, ws = seed, []
        for _ in range(16384):
            x = (x * 1664525 + 1013904223) & 0xffffffff
            ws.append(struct.unpack('<I', struct.pack('<f', 0.25 + 0.75 * (x >> 8) / (1 << 24)))[0])
        return struct.pack('<16384I', *ws)

    bad = 0
    scen = [('uniform words 1.0, storage words 0x40', struct.pack('<I', 0x3f800000) * 16384, 0x40, 1),
            ('uniform words 0.5, storage words 0x44', struct.pack('<I', 0x3f000000) * 16384, 0x44, 2),
            ('uniform floats 0.25 .. 1 (pseudo-random), storage words 0x40', floats(7), 0x40, 3),
            ('uniform floats 0.25 .. 1 (another seed), storage words 0x80', floats(99), 0x80, 4)]
    for name, ubo, sword, seed in scen:
        (tmp / 'ubo.bin').write_bytes(ubo)
        (tmp / 'ssbo.bin').write_bytes(struct.pack('<I', sword) * (MiB // 4))
        res = []
        for tag, mod in (('shared', spv), ('private', tmp / 'private.spv')):
            rd = tmp / f'{tag}-{seed}'; rd.mkdir()
            try:
                r = subprocess.run([str(csrun), str(mod), *map(str, groups), str(seed), *specs], cwd=rd, capture_output=True, text=True, timeout=280)
            except subprocess.TimeoutExpired:
                r = None
            if r is None or r.returncode:
                res = None
                print(f'  {name}: the {tag} module did not finish on lavapipe ({"over the time" if r is None else r.stderr.strip()[-200:]}): not compared', flush=True)
                break
            res.append({b: (rd / f'out_{b}.bin').read_bytes() for b in outs})
        if res is None:
            bad += 1
            continue
        same = res[0] == res[1]
        written = {b: sum(1 for w in struct.unpack(f'<{len(v) // 4}I', v) if w not in (0, sword)) for b, v in res[0].items()}
        bad += not same or not any(written.values())
        print(f'  {name}: words that are neither 0 nor the fill, by binding: {written}: ' + ('EQUAL byte for byte' if same else 'DIFFERENT'), flush=True)
    return bad


def main():
    if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):   # the image names a file its Mesa does not install
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); csrun = tmp / 'csrun'
        run(['gcc', '-O1', '-o', str(csrun), str(HERE / 'csrun.c'), '-lvulkan'])
        bad = synthetic(tmp, csrun)
        if len(sys.argv) > 2:
            groups = [int(x) for x in sys.argv[3:6]] if len(sys.argv) >= 6 else [3, 2, 2]
            bad += module(tmp, csrun, sys.argv[1], sys.argv[2], groups)
    print('smem_private:', 'ok' if not bad else f'FAILED ({bad})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
