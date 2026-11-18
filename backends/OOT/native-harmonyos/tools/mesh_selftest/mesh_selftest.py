"""Offline check of the mesh wrapper (tools/mesh_wrap_in_container.py wrap()) on Mesa's lavapipe, before any device run:
synthetic kernels whose stage memory writes are known go through the same wrap() and the same compile (glslang,
spirv-opt -O, spirv-val), are dispatched (vkrun.c) with the buffers the replay gives them, and what they export is
compared word for word with what the layout says it must be.

  plain   a mesh shader without a task: the workgroup's number from the control block's first, the vertex and index
          words, the degenerate triangles past the primitive count, the draw's index count written by group 0
  cap     the same dispatched with more groups than the buffers hold: the groups past the capacity write nothing, the
          index count stops at the capacity
  task    a task kernel, then a mesh kernel with a task: each task's payload and launch count, the table of the mesh
          groups (which task's payload, which of its launches), the count of launches, and the mesh groups' output
          from the payload they were handed

  docker run --rm -v <port>:/repo mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/mesh_selftest.py

It checks the wrapper's own work. It does not run the game's kernels: lavapipe's subgroups are not 32 wide, which they
assume."""
import os, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import mesh_wrap_in_container as mw   # noqa: E402

MAX_V, MAX_P, NCOMP, THREADS = 64, 40, 6, 32
VERTEX = [['pos', 0], ['pos', 1], ['pos', 2], ['pos', 3], [3, 0], [3, 1]]
PAYLOAD = 64   # bytes


def mesh_layout(payload):
    """as nxnative's dmsh_meshstage.py memory(): [workgroup, input, outputs, primitive bytes], the size"""
    out = 16 + (payload + 15 & ~15)
    v_blocks, p_blocks = (MAX_V + 31) // 32, (MAX_P + 31) // 32
    prim = out + max(v_blocks * NCOMP, 1) * 32 * 4
    return [0, 16, out, prim], prim + (4 + 3 * p_blocks * 32 + 3 & ~3)


def task_layout(payload):
    out = 16 + (payload + 15 & ~15)
    return [0, 16, out, out + payload], out + (payload + 3 & ~3)


def mesh_kernel(lay, words, task):
    wg, inp, out, prim = lay
    # the value every output word is made of: without a task the workgroup's number; with one, its number among its
    # task's launches (which must be in the workgroup's number and in the payload's word 0) and the task's own word 2
    seed = (f'dmsh_mem[{wg // 4}] == dmsh_mem[{inp // 4}] ? dmsh_mem[{inp // 4}] * 100u + dmsh_mem[{inp // 4 + 2}] : 999999u'
            if task else f'dmsh_mem[{wg // 4}]')
    return f'''#version 450
layout(local_size_x = {THREADS}, local_size_y = 1, local_size_z = 1) in;

shared uint dmsh_mem[{words}];

void main()
{{
    uint t = gl_LocalInvocationID.x;
    uint g = {seed};
    uint count = g % 5u == 4u ? 0u : 5u + (g % 30u);   // every fifth workgroup has no primitive
    for (uint v = t; v < {MAX_V}u; v += {THREADS}u)
        for (uint c = 0u; c < {NCOMP}u; c++)
            dmsh_mem[{out // 4}u + ((v >> 5) * {NCOMP}u + c) * 32u + (v & 31u)] = floatBitsToUint(float(g * 10000u + v * 16u + c));
    for (uint p = t; p < count; p += {THREADS}u)
        for (uint k = 0u; k < 3u; k++)
        {{
            uint at = {prim + 4}u + p * 3u + k;
            uint index = k == 0u ? p % {MAX_V}u : k == 1u ? (p + 1u) % {MAX_V}u : (p * 7u) % {MAX_V}u;
            atomicOr(dmsh_mem[at >> 2], index << (8u * (at & 3u)));
        }}
    if (t == 3u)
    {{
        atomicOr(dmsh_mem[{prim // 4}], count << 24);
    }}
    if (t > 20u)
    {{
        return;
    }}
    dmsh_mem[{words - 1}] = 0u;
}}
'''


def task_kernel(lay, words):
    wg, inp, out, _ = lay
    return f'''#version 450
layout(local_size_x = {THREADS}, local_size_y = 1, local_size_z = 1) in;

shared uint dmsh_mem[{words}];

void main()
{{
    uint t = gl_LocalInvocationID.x;
    uint n = dmsh_mem[{wg // 4}];
    if (t == 1u)
    {{
        dmsh_mem[{out // 4 + 1}] = n % 4u;          // the mesh workgroups it launches
        dmsh_mem[{out // 4 + 2}] = 40u + n;         // its own word, which its mesh groups read
    }}
    for (uint w = 3u + t; w < {PAYLOAD // 4}u; w += {THREADS}u) dmsh_mem[{out // 4}u + w] = n * 1000u + w;
    if (t > 9u)
    {{
        return;
    }}
}}
'''


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'{" ".join(map(str, cmd))}\n{r.stdout}{r.stderr}')
    return r.stdout


def build(d, name, glsl, info):
    text, binds = mw.wrap(glsl, info)
    (d / f'{name}.comp').write_text(text)
    e = mw.compile_module(d / f'{name}.comp', d / f'{name}.spv', 'comp', 'vulkan1.1')
    if e:
        sys.exit(f'{name}: {e}')
    return {kind: b for b, _, kind, _ in binds}


def words_of(path):
    b = path.read_bytes()
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def ctl_file(path, first, cap, tcap, tasks, groups):
    c = [0] * mw.C_WORDS
    c[mw.C_FIRST], c[mw.C_CAP], c[mw.C_TCAP] = first, cap, tcap
    c[mw.C_TDISP:mw.C_TDISP + 3] = [tasks, 1, 1]
    c[mw.C_DISP:mw.C_DISP + 3] = [groups, 1, 1]
    c[mw.C_DRAW:mw.C_DRAW + 5] = [0, 1, 0, 0, 0]
    path.write_bytes(struct.pack(f'<{len(c)}I', *c))


def expect(s):
    """what the synthetic mesh kernel of value s leaves: (its triangles as vertex numbers, the vertices they use)"""
    count = 0 if s % 5 == 4 else 5 + s % 30
    tris = [(p % MAX_V, (p + 1) % MAX_V, (p * 7) % MAX_V) for p in range(count)]
    return tris, max((max(t) for t in tris), default=-1) + 1


def check_dense(name, vtx, idx, ctl, seeds):
    """The dense output of the workgroups whose values are seeds (those inside the capacity), in any order: the draw
    record's index count and the vertex counter, every vertex's words, the triangles as a multiset, and nothing written
    past the counters."""
    from collections import Counter
    bad = 0
    def wrong(text):
        nonlocal bad
        bad += 1
        if bad <= 6: print(f'  {name}: {text}')
    want_tris = Counter(); want_i = want_v = 0
    for s in seeds:
        tris, used = expect(s)
        want_i += 3 * len(tris); want_v += used
        for t in tris: want_tris[(s, t)] += 1
    if ctl[mw.C_DRAW:mw.C_DRAW + 5] != [want_i, 1, 0, 0, 0]: wrong(f'the draw record is {ctl[mw.C_DRAW:mw.C_DRAW + 5]}, want [{want_i}, 1, 0, 0, 0]')
    if ctl[mw.C_VERT] != want_v: wrong(f'{ctl[mw.C_VERT]} vertices taken, want {want_v}')
    nv, ni = min(ctl[mw.C_VERT], len(vtx) // NCOMP), min(ctl[mw.C_DRAW], len(idx))
    def vertex(n):   # (the value, the vertex number) the kernel made vertex n of, or None
        w = [struct.unpack('<f', struct.pack('<I', vtx[n * NCOMP + c]))[0] for c in range(NCOMP)]
        base = int(round(w[0]))
        if any(int(round(w[c])) != base + c for c in range(NCOMP)): return None
        return base // 10000, base % 10000 // 16
    made = [vertex(n) for n in range(nv)]
    for n, m in enumerate(made):
        if m is None: wrong(f'vertex {n} is not a vertex the kernel made')
    got_tris = Counter()
    for i in range(0, ni - ni % 3, 3):
        v = [made[x] if x < nv else None for x in idx[i:i + 3]]
        if None in v or len({m[0] for m in v}) != 1: wrong(f'triangle at index {i}: {idx[i:i + 3]}'); continue
        got_tris[(v[0][0], tuple(m[1] for m in v))] += 1
    if got_tris != want_tris:
        wrong(f'{sum(got_tris.values())} triangles, {len((got_tris - want_tris) + (want_tris - got_tris))} of them differ from the {sum(want_tris.values())} expected')
    if any(w != 0xcdcdcdcd for w in vtx[nv * NCOMP:]): wrong('vertex words written past the vertex counter')
    if any(w != 0xcdcdcdcd for w in idx[ni:]): wrong('indices written past the index counter')
    return bad


def case_plain(tmp, vkrun, name, first, groups, cap, slack=0):
    lay, size = mesh_layout(0)
    info = dict(kind='ms', threads=THREADS, payload=0, task=False, verts=3, max_v=MAX_V, max_p=MAX_P, vertex=VERTEX, primitive=[],
                stage_memory=lay, shared=size)
    d = tmp / name; d.mkdir()
    by = build(d, 'mesh', mesh_kernel(lay, (size + 3) // 4, False), info)
    ctl_file(d / 'ctl.bin', first, cap, 0, 0, groups)
    held = cap + slack   # groups the buffers hold: more than the capacity, to see that nothing is written past it
    print(run([str(vkrun), str(d / 'mesh.spv'), str(groups), f'{by[mw.RB_MESH_VTX]}:s:{held * MAX_V * NCOMP * 4}',
               f'{by[mw.RB_MESH_IDX]}:s:{held * MAX_P * 3 * 4}', f'{by[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}'], cwd=d).strip())
    vtx, idx, ctl = (words_of(d / f'out_{by[k]}.bin') for k in (mw.RB_MESH_VTX, mw.RB_MESH_IDX, mw.RB_MESH_PAR))
    bad = check_dense(name, vtx, idx, ctl, [first + g for g in range(min(groups, cap))])
    print(f'{name}: {groups} groups dispatched, capacity {cap}: {ctl[mw.C_VERT]} vertices and {ctl[mw.C_DRAW]} indices (dense), the counters and the untouched rest checked, {bad} wrong')
    return bad


def case_task(tmp, vkrun, name, first, tasks, cap):
    pw = PAYLOAD // 4
    d = tmp / name; d.mkdir()
    tlay, tsize = task_layout(PAYLOAD)
    tinfo = dict(kind='ts', threads=THREADS, payload=PAYLOAD, stage_memory=tlay, shared=tsize)
    tby = build(d, 'task', task_kernel(tlay, (tsize + 3) // 4), tinfo)
    mlay, msize = mesh_layout(PAYLOAD)
    minfo = dict(kind='ms', threads=THREADS, payload=PAYLOAD, task=True, verts=3, max_v=MAX_V, max_p=MAX_P, vertex=VERTEX, primitive=[],
                 stage_memory=mlay, shared=msize)
    mby = build(d, 'mesh', mesh_kernel(mlay, (msize + 3) // 4, True), minfo)
    in_bytes = (2 * cap + tasks * pw) * 4
    ctl_file(d / 'ctl.bin', first, cap, tasks, tasks, 0)
    print(run([str(vkrun), str(d / 'task.spv'), str(tasks), f'{tby[mw.RB_MESH_IN]}:s:{in_bytes}', f'{tby[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}'], cwd=d).strip())
    table, ctl = words_of(d / f'out_{tby[mw.RB_MESH_IN]}.bin'), words_of(d / f'out_{tby[mw.RB_MESH_PAR]}.bin')
    bad = 0
    launches = [(first + g) % 4 for g in range(tasks)]
    total = sum(launches)
    if ctl[mw.C_DISP] != total:
        bad += 1; print(f'  {name}: {ctl[mw.C_DISP]} launches counted, want {total}')
    # the payloads, and the table: every (task, launch) once, in any order of the tasks
    seen, seeds = set(), []
    for g in range(tasks):
        n = first + g; at = 2 * cap + g * pw
        want = [0xcdcdcdcd, n % 4, 40 + n] + [n * 1000 + w for w in range(3, pw)]
        want[0] = table[at]   # word 0 is not the task's (it wrote 0 there: the cleared stage memory)
        if table[at] != 0 or table[at:at + pw] != want:
            bad += 1; print(f'  {name}: payload of task {g}: {table[at:at + pw][:6]}..., want {[0] + want[1:6]}...')
    for gi in range(min(total, cap)):
        at, j = table[2 * gi], table[2 * gi + 1]
        g = (at - 2 * cap) // pw
        if (at - 2 * cap) % pw or not 0 <= g < tasks or j >= launches[g] or (g, j) in seen:
            bad += 1; print(f'  {name}: table entry {gi} = ({at}, {j})')
            seeds.append(0); continue
        seen.add((g, j)); seeds.append(j * 100 + 40 + first + g)
    if len(seen) != min(total, cap):
        bad += 1; print(f'  {name}: {len(seen)} table entries, want {min(total, cap)}')
    # the mesh dispatch, with what the task step left (the replay runs it with the count in the control block, read by the GPU)
    (d / 'ctl2.bin').write_bytes((d / f'out_{tby[mw.RB_MESH_PAR]}.bin').read_bytes())
    (d / 'in2.bin').write_bytes((d / f'out_{tby[mw.RB_MESH_IN]}.bin').read_bytes())
    print(run([str(vkrun), str(d / 'mesh.spv'), str(total), f'{mby[mw.RB_MESH_IN]}:s:{in_bytes}:{d / "in2.bin"}',
               f'{mby[mw.RB_MESH_VTX]}:s:{cap * MAX_V * NCOMP * 4}', f'{mby[mw.RB_MESH_IDX]}:s:{cap * MAX_P * 3 * 4}',
               f'{mby[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl2.bin"}'], cwd=d).strip())
    vtx, idx, ctl = (words_of(d / f'out_{mby[k]}.bin') for k in (mw.RB_MESH_VTX, mw.RB_MESH_IDX, mw.RB_MESH_PAR))
    bad += check_dense(name, vtx, idx, ctl, seeds)
    print(f'{name}: {tasks} tasks launched {total} mesh groups (capacity {cap}): payloads, table, {len(seeds)} groups\' output and the draw record checked, {bad} wrong')
    return bad


def main():
    if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):   # the image names a file its Mesa does not install
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); vkrun = tmp / 'vkrun'
        run(['gcc', '-O1', '-o', str(vkrun), str(HERE / 'vkrun.c'), '-lvulkan'])
        bad = case_plain(tmp, vkrun, 'plain', 7, 5, 5)
        bad += case_plain(tmp, vkrun, 'cap', 3, 9, 6, slack=2)
        bad += case_task(tmp, vkrun, 'task', 2, 11, 64)
        bad += case_task(tmp, vkrun, 'task-cap', 2, 11, 9)
        # the vertex shader of the draws compiles and validates too
        (tmp / 'v.vert').write_text(mw.vertex_shader(dict(vertex=VERTEX)))
        e = mw.compile_module(tmp / 'v.vert', tmp / 'v.spv', 'vert', 'vulkan1.1')
        if e:
            sys.exit('vertex shader: ' + e)
    print('mesh wrapper self-test:', 'ok' if not bad else f'FAILED ({bad})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
