"""The mesh wrapper's self-test for its stage memory modes (wrap_modes(): bytes, inputs, direct), on Mesa's lavapipe, beside
mesh_selftest.py (whose kernels are plain GLSL, which no mode applies to).

The mesh kernels here are written in the translator's idiom (the byte store helper with its compare-and-swap loop copied from
a translated kernel, word stores as 'MEM[int(_w)] = v' with '_w = uint(int(uint(_b) >> uint(2)))' and '_b = register + K',
loads as MEM[0], MEM[4], MEM[int(_w)]; straight-line code, no subgroup operations: lavapipe's subgroups are 8 wide), so the
modes apply (checked: a case stops when a mode it asked for was not taken).

  P     meshlet form, no task: thread t writes vertices min(nv - 1, t + 32 j) and primitives min(np - 1, t + 32 j) (threads
        past the counts write the last one again; index bytes are stored with bits above the byte set); every fifth group
        returns at once in every thread: NO count byte, no vertex, no index byte written (zero primitives)
  P160  P with 160 vertices of 23 components, 128 primitives        P32   P with one block of vertices
  C     compacting form (as a667b8bba94befa2): a live thread with c live threads below it writes vertices 3c..3c+2 and
        primitive c, the lowest live thread the count; threads that are not live return before any store; every fourth
        group has no live thread (nothing written at all)
  cap   P dispatched with more groups than the buffers hold
  PP    P with two per-primitive attribute components (their words lie after the vertex blocks; not passed on)
  T     a task kernel, then P with a task: the launch's number from payload word 0 and from the workgroup's number, a
        constant payload word, a computed payload word, and a word of the input area PAST the payload (72 payload bytes in
        an 80 byte area: the cleared stage memory, 0). The task kernel is in the translator's idiom too (oot-a106): the
        workgroup's number from word 0, the launch count and its own word at constant payload words, the other payload
        words through 'register + K', so the task mode 'payload' applies (checked as the mesh modes are); the task step's
        buffer (payload words, control block, the table as a set) must also be what the wrapper with no modes wrote.
        More task kernels, and the ones the mode must refuse: mesh_selftest_task.py
The dense output puts the workgroups into the buffers in no fixed order, so an output is compared as: the draw record, the
vertex counter, the vertices as a multiset of their words, the triangles as a multiset of their three vertices' words, and
nothing written past the counters. Every mode set's output must be that of the model AND that of the wrapper with no modes.
With direct a workgroup's vertices are in its own slot of the vertex buffer (group * MAX_V): there is no vertex counter, the
vertices are compared through the triangles that name them, and no word may be written past the slots of the groups that ran.

  docker run --rm -v <port>:/repo mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/mesh_selftest_modes.py"""
import os, struct, subprocess, sys, tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import mesh_wrap_in_container as mw   # noqa: E402

THREADS = 32
HELPER = '''void dmsh_shared_store8_0(int _2508, int _2509)
{
    uint _2512 = uint(int(uint(_2508) >> uint(2)));
    int _2514 = _2508 & 3;
    int _2516 = _2514 << 3;
    bool _2518;
    do
    {
        float _2520 = uintBitsToFloat(_33[int(_2512)]);
        int _2522 = int(bitfieldInsert(floatBitsToUint(_2520), uint(_2509), int(uint(_2516)), int(8u)));
        uint _2557 = atomicCompSwap(_33[int(_2512)], floatBitsToUint(_2520), uint(_2522));
        uint _2524 = _2557;
        _2518 = int(_2524) != floatBitsToInt(_2520);
    } while (_2518);
}
'''
FILL = 0xcdcdcdcd


class Gen:
    def __init__(self):
        self.n = 100; self.lines = []
    def t(self, typ, expr, ind='    '):
        self.n += 2; v = f'_{self.n}'
        self.lines.append(f'{ind}{typ} {v} = {expr};'); return v
    def raw(self, s):
        self.lines.append(s)
    def load(self, base, k, ind='    '):
        a = self.t('int', f'{base} + {k}', ind)
        w = self.t('uint', f'uint(int(uint({a}) >> uint(2)))', ind)
        return self.t('int', f'int(_33[int({w})])', ind)
    def store(self, base, k, value, ind='    '):
        a = self.t('int', f'{base} + {k}', ind)
        w = self.t('uint', f'uint(int(uint({a}) >> uint(2)))', ind)
        self.raw(f'{ind}_33[int({w})] = {value};')
    def store8(self, base, k, value, ind='    '):
        a = self.t('int', f'{base} + {k}', ind)
        b = self.t('int', a, ind)
        v = self.t('int', value, ind)
        self.raw(f'{ind}dmsh_shared_store8_0({b}, {v});')
    def text(self, words, helper=True, threads=THREADS):
        return (f'#version 450\nlayout(local_size_x = {threads}, local_size_y = 1, local_size_z = 1) in;\n\nshared uint _33[{words}];\n\n'
                + (HELPER + '\n' if helper else '') + 'void main()\n{\n' + '\n'.join(self.lines) + '\n}\n')


def layout(max_v, max_p, ncomp, payload=0, pcomp=0):
    """as nxnative's dmsh_meshstage.py memory(): [workgroup, input, outputs, primitive bytes], the size; the attributes are all
    the blocks of 32 vertices, then all the blocks of 32 primitives (pcomp per-primitive components)"""
    out = 16 + (payload + 15 & ~15)
    prim = out + ((max_v + 31) // 32) * ncomp * 128 + ((max_p + 31) // 32) * pcomp * 128
    return [0, 16, out, prim], prim + (4 + 3 * ((max_p + 31) // 32) * 32 + 3 & ~3)


def kernel_p(max_v, max_p, ncomp, payload=0, pcomp=0):
    lay, size = layout(max_v, max_p, ncomp, payload, pcomp); inp, out, prim = lay[1], lay[2], lay[3]
    g = Gen()
    t = g.t('int', 'int(gl_LocalInvocationID.x)')
    if payload:
        j = g.t('int', 'int(_33[4])')            # payload word 0: the launch's number
        wgn = g.t('int', 'int(_33[0])')          # the workgroup's number: the same
        own = g.t('int', 'int(_33[6])')          # payload word 2, a constant place
        jb = g.t('int', f'{j} << 2')
        rec = g.load(jb, inp + 12)               # payload word 3 + j, a computed place
        z = g.t('int', f'({wgn} - {j}) << 2')
        pad = g.load(z, inp + payload + 4)       # a word of the input area past the payload: 0
        n = g.t('int', f'{j} * 100 + {own} + {rec} * 3 + {pad} * 7777 + ({wgn} - {j}) * 1000000')
    else:
        n = g.t('int', 'int(_33[0])')
    g.raw(f'    if (({n} % 5) == 0)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    np_ = g.t('int', f'5 + ({n} * 7) % {max_p - 4}')
    nv = g.t('int', f'3 + ({n} * 5) % {max_v - 2}')
    for j in range((max_v + 31) // 32):
        v = g.t('int', f'min({nv} - 1, {t} + {32 * j})')
        hi = g.t('int', f'int(uint({v}) >> uint(5)) * {ncomp * 128}')
        lo = g.t('int', f'({v} & 31) << 2')
        base = g.t('int', f'{hi} + {lo}')
        for c in range(ncomp):
            val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}')
            g.store(base, out + c * 128, f'uint({val})')
    g.raw(f'    if ({t} == 3)'); g.raw('    {')
    a = g.t('int', str(prim + 3), '        '); c = g.t('int', np_, '        ')
    g.raw(f'        dmsh_shared_store8_0({a}, {c});'); g.raw('    }')
    for j in range((max_p + 31) // 32):
        p = g.t('int', f'min({np_} - 1, {t} + {32 * j})')
        base = g.t('int', f'({p} << 1) + {p}')
        for k in range(3):
            # bits above the byte (k 1: a negative value, k 2: bits 8 and 9) must not get into the neighbouring bytes
            g.store8(base, prim + 4 + k, f'(({p} * {(5, 11, 3)[k]} + {k}) % {nv}){("", " - 256", f" + (({p} & 3) << 8)")[k]}')
    for j in range((max_p + 31) // 32 if pcomp else 0):
        # per-primitive attribute words: after the vertex blocks; the block's place is in the register, the constant is a
        # component's offset, as a vertex word's (today's wrapper does not pass them on)
        p = g.t('int', f'min({np_} - 1, {t} + {32 * j})')
        hi = g.t('int', f'{((max_v + 31) // 32) * ncomp * 128} + int(uint({p}) >> uint(5)) * {pcomp * 128}')
        lo = g.t('int', f'({p} & 31) << 2')
        base = g.t('int', f'{hi} + {lo}')
        for c in range(pcomp):
            val = g.t('int', f'-559038737 + {p} * 4 + {c}')
            g.store(base, out + c * 128, f'uint({val})')
    g.raw(f'    if ({t} > 20)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    g.t('int', f'{t} + 1')
    def model(n):
        if n % 5 == 0:
            return 0, {}, {}
        np_, nv = 5 + (n * 7) % (max_p - 4), 3 + (n * 5) % (max_v - 2)
        return (np_, {(v, c): n * 10000 + v * 16 + c for v in range(nv) for c in range(ncomp)},
                {(p, k): (p * (5, 11, 3)[k] + k) % nv for p in range(np_) for k in range(3)})
    return g.text((size + 3) // 4), lay, size, model


def kernel_c(max_v, max_p, ncomp, payload=0):
    lay, size = layout(max_v, max_p, ncomp); out, prim = lay[2], lay[3]
    g = Gen()
    t = g.t('int', 'int(gl_LocalInvocationID.x)')
    n = g.t('int', 'int(_33[0])')
    mask = g.t('int', f'(({n} & 3) == 0) ? 0 : (({n} * 40503 + 12345) | 16)')
    live = g.t('bool', f'((uint({mask}) >> uint({t})) & 1u) != 0u')
    below = g.t('int', f'bitCount({mask} & ((1 << {t}) - 1))')
    first = g.t('int', f'findLSB({mask})')
    cnt = g.t('int', f'bitCount({mask})')
    g.raw(f'    if ({t} == {first})'); g.raw('    {')
    a = g.t('int', str(prim + 3), '        '); c = g.t('int', cnt, '        ')
    g.raw(f'        dmsh_shared_store8_0({a}, {c});'); g.raw('    }')
    g.raw(f'    if (!{live})'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    v0 = g.t('int', f'({below} << 1) + {below}')
    for k in range(3):
        v = g.t('int', f'{v0} + {k}')
        hi = g.t('int', f'int(uint({v}) >> uint(5)) * {ncomp * 128}')
        lo = g.t('int', f'({v} & 31) << 2')
        base = g.t('int', f'{hi} + {lo}')
        for c in range(ncomp):
            val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}')
            g.store(base, out + c * 128, f'uint({val})')
    for k in range(3):
        g.store8(v0, prim + 4 + k, f'({v0} & 255) + {k}')
    def model(n):
        mask = 0 if n & 3 == 0 else ((n * 40503 + 12345) | 16) & 0xffffffff
        cnt = bin(mask).count('1')
        return (cnt, {(v, c): n * 10000 + v * 16 + c for v in range(3 * cnt) for c in range(ncomp)},
                {(p, k): 3 * p + k for p in range(cnt) for k in range(3)})
    return g.text((size + 3) // 4), lay, size, model


def task_layout(payload):
    out = 16 + (payload + 15 & ~15)
    return [0, 16, out, out + payload], out + (payload + 3 & ~3)


def task_kernel(lay, words, payload):
    """the task kernel of the case T in the translator's idiom: thread 1 stores the launch count (n % 4, payload word 1) and
    the task's own word (40 + n, word 2) at constant words, every thread the words 3 + t + 32 j through 'register + K'"""
    wg, inp, out, _ = lay
    pw = payload // 4
    g = Gen()
    t = g.t('int', 'int(gl_LocalInvocationID.x)')
    n = g.t('int', f'int(_33[{wg // 4}])')
    g.raw(f'    if ({t} == 1)'); g.raw('    {')
    c = g.t('int', f'{n} & 3', '        ')
    g.raw(f'        _33[{out // 4 + 1}] = uint({c});')
    o = g.t('int', f'40 + {n}', '        ')
    g.raw(f'        _33[{out // 4 + 2}] = uint({o});')
    g.raw('    }')
    for j in range((pw - 3 + THREADS - 1) // THREADS):
        w = g.t('int', f'{t} + {3 + THREADS * j}')
        g.raw(f'    if ({w} < {pw})'); g.raw('    {')
        b = g.t('int', f'{w} << 2', '        ')
        v = g.t('int', f'{n} * 1000 + {w}', '        ')
        g.store(b, out, f'uint({v})', '        ')
        g.raw('    }')
    g.raw(f'    if ({t} > 9)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    g.t('int', f'{t} + 1')
    return g.text(words, helper=False)


def task_kernel_plain(lay, words, payload):
    """the same in plain GLSL (a loop, a computed index): no form the wrapper reads, the mode 'payload' is refused"""
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
    for (uint w = 3u + t; w < {payload // 4}u; w += {THREADS}u) dmsh_mem[{out // 4}u + w] = n * 1000u + w;
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


def words_of(path):
    b = path.read_bytes()
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def build(wrapper, d, name, glsl, info, modes):
    r = wrapper(glsl, info, modes)
    text, binds, applied = r
    (d / f'{name}.comp').write_text(text)
    e = mw.compile_module(d / f'{name}.comp', d / f'{name}.spv', 'comp', 'vulkan1.1')
    if e:
        sys.exit(f'{name} {sorted(modes)}: {e}')
    return {kind: b for b, _, kind, _ in binds}, applied


def ctl_file(path, first, cap, tcap, tasks, groups):
    c = [0] * mw.C_WORDS
    c[mw.C_FIRST], c[mw.C_CAP], c[mw.C_TCAP] = first, cap, tcap
    c[mw.C_TDISP:mw.C_TDISP + 3] = [tasks, 1, 1]
    c[mw.C_DISP:mw.C_DISP + 3] = [groups, 1, 1]
    c[mw.C_DRAW:mw.C_DRAW + 5] = [0, 1, 0, 0, 0]
    path.write_bytes(struct.pack(f'<{len(c)}I', *c))


MODESETS = {'none': set(), 'bytes': {'bytes'}, 'inputs': {'inputs'}, 'bytes+inputs': {'bytes', 'inputs'}, 'direct': {'direct'},
            'bytes+direct': {'bytes', 'direct'}, 'inputs+direct': {'inputs', 'direct'}, 'all three': {'bytes', 'inputs', 'direct'},
            'whole': {'bytes', 'inputs', 'direct', 'whole'},                  # the default before oot-a106
            'payload': {'payload'}, 'inputs+payload': {'inputs', 'payload'},  # the task mode alone, and with the mesh kernel reading the payload in place
            'default': set(mw.DEFAULT_MODES.split(','))}
MESH_MODES = {'bytes', 'inputs', 'direct', 'whole'}   # the modes of a mesh kernel; 'payload' is a task kernel's
WHOLE = {}        # case -> whether the mode 'whole' was taken (said at the end: a kernel that fails its check keeps the clear)
TASKREF = {}      # case -> the task step's output with no modes
DIRECT = False   # the mode set under test puts the vertices into the workgroups' own slots: no vertex counter, and the
                 # vertices are compared through the triangles that name them


def canon(vtx, idx, ctl, ncomp):
    """an output, whatever the order of its workgroups: (the draw record, the vertex counter, the vertices as a multiset of
    their words, the triangles as a multiset of their vertices' words, whether anything is written past the counters)"""
    nv, ni = (len(vtx) // ncomp if DIRECT else min(ctl[mw.C_VERT], len(vtx) // ncomp)), min(ctl[mw.C_DRAW], len(idx))
    vert = [tuple(vtx[n * ncomp:(n + 1) * ncomp]) for n in range(nv)]
    tris = Counter(tuple(vert[x] if x < nv else ('?', x) for x in idx[i:i + 3]) for i in range(0, ni - ni % 3, 3))
    if DIRECT:   # (None: not compared)
        return ctl[mw.C_DRAW:mw.C_DRAW + 5], None, None, tris, any(w != FILL for w in idx[ni:])
    past = any(w != FILL for w in vtx[nv * ncomp:]) or any(w != FILL for w in idx[ni:])
    return ctl[mw.C_DRAW:mw.C_DRAW + 5], ctl[mw.C_VERT], Counter(vert), tris, past


def want(seeds, model, dims):
    """the same of what the model's workgroups (their values: seeds) must leave"""
    max_v, max_p, ncomp = dims
    vert, tris, ni = Counter(), Counter(), 0
    for s in seeds:
        cnt, mv, mi = model(s)
        used = min(max((mi[(p, k)] for p in range(cnt) for k in range(3)), default=-1) + 1, max_v)
        words = [tuple(mv.get((v, c), 0) for c in range(ncomp)) for v in range(used)]
        vert.update(words); ni += 3 * cnt
        tris.update(tuple(words[mi[(p, k)]] for k in range(3)) for p in range(cnt))
    return [ni, 1, 0, 0, 0], sum(vert.values()), vert, tris, False


def differ(name, got, exp, what):
    bad = 0
    for label, a, b in zip(('the draw record', 'the vertex counter', 'the vertices', 'the triangles', 'words written past the counters'), got, exp):
        if a is None or b is None:
            continue
        if a != b:
            bad += 1
            d = f'{sum(((a - b) + (b - a)).values())} differ' if isinstance(a, Counter) else f'{a}, want {b}'
            print(f'  {name}: {label} against {what}: {d}')
    return bad


def case_plain(tmp, vkrun, tag, modes, name, kern, dims, first, groups, cap, slack=0, pcomp=0):
    max_v, max_p, ncomp = dims
    text, lay, size, model = kern(max_v, max_p, ncomp, 0, pcomp) if pcomp else kern(max_v, max_p, ncomp)
    info = dict(kind='ms', threads=THREADS, payload=0, task=False, verts=3, max_v=max_v, max_p=max_p,
                vertex=[['pos', c] for c in range(min(ncomp, 4))] + [[0, c] for c in range(max(ncomp - 4, 0))], primitive=[[0, c] for c in range(pcomp)],
                stage_memory=lay, shared=size)
    d = tmp / f'{tag}-{name}'; d.mkdir()
    by, applied = build(mw.wrap_modes, d, 'mesh', text, info, modes)
    if 'whole' in modes:
        WHOLE[name] = WHOLE.get(name, True) and 'whole' in applied
    # per-primitive attributes: direct is not taken, and bytes not without it when it was asked for; whole: by its check
    mm = modes & MESH_MODES
    if applied | {'whole'} != ((mm - {'direct', 'bytes'} if 'direct' in mm else mm) if pcomp else mm) | {'whole'}:
        sys.exit(f'{name} {sorted(modes)}: applied {sorted(applied)}')
    global DIRECT
    DIRECT = 'direct' in applied
    ctl_file(d / 'ctl.bin', first, cap, 0, 0, groups)
    held = cap + slack
    run([str(vkrun), str(d / 'mesh.spv'), str(groups), f'{by[mw.RB_MESH_VTX]}:s:{held * max_v * ncomp * 4}',
         f'{by[mw.RB_MESH_IDX]}:s:{held * max_p * 3 * 4}', f'{by[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}'], cwd=d)
    vtx, idx, ctl = (words_of(d / f'out_{by[k]}.bin') for k in (mw.RB_MESH_VTX, mw.RB_MESH_IDX, mw.RB_MESH_PAR))
    seeds = [first + g for g in range(min(groups, cap))]
    got = canon(vtx, idx, ctl, ncomp)
    bad = differ(name, got, want(seeds, model, dims), 'the model')
    if DIRECT and any(w != FILL for w in vtx[len(seeds) * max_v * ncomp:]):
        bad += 1; print(f'  {name}: vertex words written past the slots of the groups that ran')
    zero = sum(1 for s in seeds if model(s)[0] == 0)
    return bad, got, f'{ctl[mw.C_VERT]} vertices ({ctl[mw.C_VERT] * ncomp} words), {ctl[mw.C_DRAW]} indices, {zero} of {len(seeds)} groups with no primitive'


def case_task(tmp, vkrun, tag, modes, name, dims, first, tasks, cap, payload=72):
    max_v, max_p, ncomp = dims
    pw = (payload + 3) // 4
    d = tmp / f'{tag}-{name}'; d.mkdir()
    tlay, tsize = task_layout(payload)
    tinfo = dict(kind='ts', threads=THREADS, payload=payload, stage_memory=tlay, shared=tsize)
    tby, tapplied = build(mw.wrap_modes, d, 'task', task_kernel(tlay, (tsize + 3) // 4, payload), tinfo, modes)
    if tapplied != modes & {'payload'}:
        sys.exit(f'{name} {sorted(modes)}: the task kernel applied {sorted(tapplied)}')
    text, mlay, msize, model = kernel_p(max_v, max_p, ncomp, payload)
    minfo = dict(kind='ms', threads=THREADS, payload=payload, task=True, verts=3, max_v=max_v, max_p=max_p,
                 vertex=[['pos', c] for c in range(min(ncomp, 4))] + [[0, c] for c in range(max(ncomp - 4, 0))], primitive=[], stage_memory=mlay, shared=msize)
    mby, applied = build(mw.wrap_modes, d, 'mesh', text, minfo, modes)
    if 'whole' in modes:
        WHOLE[name] = WHOLE.get(name, True) and 'whole' in applied
    if applied | {'whole'} != (modes & MESH_MODES) | {'whole'}:
        sys.exit(f'{name} {sorted(modes)}: applied {sorted(applied)}')
    global DIRECT
    DIRECT = 'direct' in applied
    in_bytes = (2 * cap + tasks * pw) * 4
    ctl_file(d / 'ctl.bin', first, cap, tasks, tasks, 0)
    run([str(vkrun), str(d / 'task.spv'), str(tasks), f'{tby[mw.RB_MESH_IN]}:s:{in_bytes}', f'{tby[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}'], cwd=d)
    table, ctl = words_of(d / f'out_{tby[mw.RB_MESH_IN]}.bin'), words_of(d / f'out_{tby[mw.RB_MESH_PAR]}.bin')
    bad = 0
    launches = [(first + g) % 4 for g in range(tasks)]
    total = sum(launches)
    if ctl[mw.C_DISP] != total:
        bad += 1; print(f'  {name}: {ctl[mw.C_DISP]} launches counted, want {total}')
    seen, seeds = set(), []
    for g in range(tasks):
        n = first + g; at = 2 * cap + g * pw
        exp = [0, n % 4, 40 + n] + [n * 1000 + w for w in range(3, pw)]
        if table[at:at + pw] != exp:
            bad += 1; print(f'  {name}: payload of task {g}: {table[at:at + pw][:6]}..., want {exp[:6]}...')
    for gi in range(min(total, cap)):
        at, j = table[2 * gi], table[2 * gi + 1]
        g = (at - 2 * cap) // pw
        if (at - 2 * cap) % pw or not 0 <= g < tasks or j >= launches[g] or (g, j) in seen:
            bad += 1; print(f'  {name}: table entry {gi} = ({at}, {j})')
            continue
        n = first + g
        seen.add((g, j)); seeds.append(j * 100 + 40 + n + (n * 1000 + 3 + j) * 3)
    if len(seen) != min(total, cap):
        bad += 1; print(f'  {name}: {len(seen)} table entries, want {min(total, cap)}')
    # the task step's output against the wrapper with no modes: the payloads and the control block word for word, the
    # table as a set (its order is the order of the atomics; under a capacity below the launches which ones fit is too)
    step = (table[2 * cap:], ctl, sorted(seen) if cap >= total else len(seen))
    if not modes:
        TASKREF[name] = step
    elif TASKREF.get(name) != step:
        bad += 1; print(f'  {name}: the task step\'s buffer DIFFERS from the wrapper with no modes')
    (d / 'ctl2.bin').write_bytes((d / f'out_{tby[mw.RB_MESH_PAR]}.bin').read_bytes())
    (d / 'in2.bin').write_bytes((d / f'out_{tby[mw.RB_MESH_IN]}.bin').read_bytes())
    run([str(vkrun), str(d / 'mesh.spv'), str(total), f'{mby[mw.RB_MESH_IN]}:s:{in_bytes}:{d / "in2.bin"}',
         f'{mby[mw.RB_MESH_VTX]}:s:{cap * max_v * ncomp * 4}', f'{mby[mw.RB_MESH_IDX]}:s:{cap * max_p * 3 * 4}',
         f'{mby[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl2.bin"}'], cwd=d)
    vtx, idx, ctl2 = (words_of(d / f'out_{mby[k]}.bin') for k in (mw.RB_MESH_VTX, mw.RB_MESH_IDX, mw.RB_MESH_PAR))
    got = canon(vtx, idx, ctl2, ncomp)
    bad += differ(name, got, want(seeds, model, dims), 'the model')
    if DIRECT and any(w != FILL for w in vtx[len(seeds) * max_v * ncomp:]):
        bad += 1; print(f'  {name}: vertex words written past the slots of the groups that ran')
    if words_of(d / f'out_{mby[mw.RB_MESH_IN]}.bin') != words_of(d / 'in2.bin'):
        bad += 1; print(f'  {name}: the mesh step changed the task step\'s buffer')
    zero = sum(1 for s in seeds if model(s)[0] == 0)
    # which launches fit under a capacity below the launches is the task step's order (atomics): then only the model is compared
    return bad, (got if cap >= total else None), f'{tasks} tasks, {len(seeds)} mesh groups: {ctl2[mw.C_VERT]} vertices, {ctl2[mw.C_DRAW]} indices, {zero} groups with no primitive'


def suite(tmp, vkrun, tag, modes, ref):
    bad, results = 0, {}
    cases = [
        ('P', lambda: case_plain(tmp, vkrun, tag, modes, 'P', kernel_p, (64, 126, 5), 7, 40, 40)),
        ('P160', lambda: case_plain(tmp, vkrun, tag, modes, 'P160', kernel_p, (160, 128, 23), 1, 12, 12)),
        ('P32', lambda: case_plain(tmp, vkrun, tag, modes, 'P32', kernel_p, (32, 40, 7), 11, 16, 16)),
        ('C', lambda: case_plain(tmp, vkrun, tag, modes, 'C', kernel_c, (96, 32, 6), 3, 40, 40)),
        ('cap', lambda: case_plain(tmp, vkrun, tag, modes, 'cap', kernel_p, (64, 126, 4), 3, 9, 6, slack=2)),
        ('PP', lambda: case_plain(tmp, vkrun, tag, modes, 'PP', kernel_p, (64, 64, 6), 9, 16, 16, pcomp=2)),
        ('T', lambda: case_task(tmp, vkrun, tag, modes, 'T', (64, 126, 4), 2, 11, 64)),
        ('T-cap', lambda: case_task(tmp, vkrun, tag, modes, 'T-cap', (64, 126, 4), 2, 11, 9)),
    ]
    for name, fn in cases:
        b, got, what = fn()
        same = ''
        if ref is not None and ref.get(name) is not None and got is not None:
            d = differ(name, got, ref[name], 'the wrapper with no modes')
            b += d
            same = ', and as the wrapper with no modes wrote' if not d else ', DIFFERS from the wrapper with no modes'
        print(f'{tag:13s} {name:5s}: {what}: {b} wrong against the model{same}', flush=True)
        bad += b; results[name] = got
    return bad, results


def main():
    if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):   # the image names a file its Mesa does not install
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); vkrun = tmp / 'vkrun'
        run(['gcc', '-O1', '-o', str(vkrun), str(HERE / 'vkrun.c'), '-lvulkan'])
        total, ref = 0, None
        for tag, modes in MODESETS.items():
            bad, res = suite(tmp, vkrun, tag, set(modes), ref)
            total += bad
            if tag == 'none':
                ref = res
    print("the mode 'whole' (no clear of the vertex slot) was taken by:", ', '.join(f'{k} {"yes" if v else "no"}' for k, v in WHOLE.items()))
    # these kernels store every vertex whole: the mode must be taken (what must NOT take it: mesh_selftest_whole.py)
    lost = [k for k in ('P', 'P160', 'P32', 'C', 'T') if not WHOLE.get(k)]
    if lost:
        print("FAILED: the mode 'whole' was not taken by", ', '.join(lost)); total += 1
    print('mesh wrapper self-test (stage memory modes):', 'ok' if not total else f'FAILED ({total})')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
