"""Adversarial differential test of gmemfast.rewrite_raw's N-entry cache (oot-a109), written by the independent checker,
on lavapipe (mng-vk-host:lavapipe). Complements gmem_ways_test.py: other pool layouts, a third helper kind, and EXACT
per-helper counts of calls, searches, lookups in no pool and hits of entries 1..3 against a per-thread LRU model.

Kernel: the translator's word (W) and 128-bit (V4) helpers of a game kernel, plus a 64-bit (V2) helper made from the V4
one (the load and the vector words cut to two; the game's 109 modules have no V2 helper, so this one is rewritten with
strict=False). Modules: prod (the translator's text), w1..w4, each with an instrumented twin.
Table (gmem_run.c, as overlay nvn_vk.c gmem_rebuild): disjoint ascending entries, entry 0 empty, unused ~0, hints.
Pools: adjacent chains with different deltas (X|Y as gmem_rebuild leaves an overlap: the later span starts at the
earlier one's end; E1|E2 across 4 GiB; P|Q at a hinted chunk's end), an alias pair (two ranges, one memory), three
pools with the same low words and different high words, one across 8 GiB, hinted pools (whole 2 MiB chunks), a pool
whose chunks take two of them over (hint slots 512, 513), a pool with high word 0x200000 whose truncated chunk tag
equals a real hint (the 'hi < 256' guard), and a pool at the top of the address space.
Patterns (thread mod 12): adjacent boundaries; exact ends; high-word changes; the hint path; a lookup in no pool after a
hit (must give 0, not the cached delta); special addresses; three helpers on the same pools; random walk; five pools by
turns; LRU reuse; alias pair; mostly lookups in no pool.
Every output word must equal the model and prod; the counters must equal the per-thread model exactly (so the cache is
per thread and LRU as documented). 'shared' (the cache words made workgroup-shared) must be noticed by the counts.

  docker run --rm -v <port>:/repo:ro mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/gmem_ways_adv.py
"""
import random, re, struct, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import gmem_ways_test as gwt   # noqa: E402  (cross, helpers_of, word, the runner's build)
import gmemfast                 # noqa: E402

mw = gwt.mw
THREADS, K, NPAT = 32, 32, 12
MiB, M64, M32 = 1 << 20, (1 << 64) - 1, 0xffffffff
DEAD = gwt.DEAD
V2NAME, V2LAST = '_990001', '_990002'


# ---------------------------------------------------------------- the pools and the table model
def pools():
    """[(name, start, end, memory offset)] ascending; memory offsets with a gap, an alias pair sharing memory"""
    spec, off = [], 0

    def add(name, lo, size, mem=None):
        nonlocal off
        o = off if mem is None else mem
        spec.append((name, lo, lo + size, o))
        if mem is None:
            off += size + 0x100
        return o
    for i in range(6):
        add(f'K{i}', 0x08000000 + i * 0x5000, 0x1000)
    m = add('AL1', 0x09000000, 0x1000); add('AL2', 0x09002000, 0x1000, mem=m)
    add('X', 0x0A000000, MiB); add('Y', 0x0A100000, MiB)              # X | Y adjacent (a trimmed overlap)
    add('H1', 0x40000000, 8 * MiB)                                     # chunks 512..515 whole
    add('H2', 0x50100000, 4 * MiB)                                     # chunk 0x281 whole, head and tail not
    add('P', 0x60000000, 2 * MiB); add('Q', 0x60200000, 0x1000)        # P = chunk 0x300 (hinted) | Q
    add('E1', 0xFFFF0000, 0x10000); add('E2', 0x100000000, 0x1000)     # E1 | E2 at 4 GiB
    add('L1', 0x120000000, 0x1000)
    add('S', 0x1FFFF8000, 0x10000)                                     # across 8 GiB
    add('L2', 0x220000000, 0x1000); add('L3', 0x320000000, 0x1000)    # L1 L2 L3: same low words
    add('C', 0x440000000, 4 * MiB)                                     # chunks 8704, 8705: hint slots 512, 513
    add('G2', 0x0020000040400000, 0x1000)                              # hi 0x200000: (hi << 11) wraps, tag 514 matches
    add('T', 0xFFFFFFFFFFFF0000, 0xFFF0)                               # the top, under the unused entries' ~0
    for a, b in zip(spec, spec[1:]):
        assert a[2] <= b[1], (a, b)
    return spec, off + 0x10000


class Table:
    def __init__(self, ps):
        n = 8192
        self.start = [0] + [p[1] for p in ps] + [M64] * (n - 1 - len(ps))
        self.end = [0] + [p[2] for p in ps] + [M64] * (n - 1 - len(ps))
        self.mem = [0] + [p[3] for p in ps] + [0] * (n - 1 - len(ps))
        self.hint = [0] * n
        for k, (_, lo, hi, _o) in enumerate(ps, 1):
            c = (lo + (1 << 21) - 1) >> 21
            while ((c + 1) << 21) <= hi and c < (1 << 19):
                self.hint[c & 8191] = ((c << 13) | k) & M32
                c += 1

    def find(self, a):
        lo, hi = a & M32, a >> 32
        chunk = ((hi << 11) | (lo >> 21)) & M32
        h = self.hint[chunk & 8191]
        e = h & 8191
        if (h >> 13) == chunk and hi < 256 and e != 0:
            return e, True
        e, step = 0, 4096
        while step:
            if self.start[e + step] <= a:
                e += step
            step >>= 1
        return e, False

    def holds(self, k, a):
        return self.start[k] <= a < self.end[k]


def simulate(tab, total_words, seq, n):
    """outputs and counters of one thread: counters[kind] = [calls, searches, none, hit e1, e2, e3]; hinted finds"""
    caches = {k: [0] * n for k in (0, 1, 2)}
    cnt = {k: [0] * 6 for k in (0, 1, 2)}
    vec, out, hinted = list(DEAD), [], 0
    for a, kind in seq:
        c, ct = caches[kind], cnt[kind]
        ct[0] += 1
        if tab.holds(c[0], a):
            e, ok = c[0], True
        else:
            j = next((j for j in range(1, n) if tab.holds(c[j], a)), None)
            if j is not None:
                ct[2 + j] += 1
                e, ok = c[j], True
                c[:] = [c[j]] + c[:j] + c[j + 1:]
            else:
                ct[1] += 1
                e, h = tab.find(a)
                hinted += h
                c[:] = [e] + c[:n - 1]
                ok = a < tab.end[e]
        r = 0
        if ok:
            w = (tab.mem[e] + a - tab.start[e]) // 4
            assert w + 3 < total_words
            r = gwt.word(w)
            if kind == 1:
                vec = [gwt.word(w + 1), gwt.word(w + 2), gwt.word(w + 3)]
            elif kind == 2:
                vec[0] = gwt.word(w + 1)
        else:
            ct[2] += 1
        out.append((r * 3 + vec[0] * 5 + vec[1] * 7 + vec[2] * 11) & M32)
    return out, cnt, hinted


# ---------------------------------------------------------------- the lookups
def sequences(ps, threads, rnd):
    P = {p[0]: p for p in ps}
    plain = [p for p in ps if p[0] not in ('X', 'Y', 'P', 'Q', 'E1', 'E2', 'AL1', 'AL2')]   # no neighbour at the end

    def inside(p, a4=True):
        _, s, e, _o = p
        return s + rnd.randrange(0, (e - s) // 4) * 4

    def far():
        return rnd.choice([lambda: 0x1000 + rnd.randrange(0, 0x1000) * 16,
                           lambda: 0x0000008000000080 + rnd.randrange(0, 1 << 20) * 16,
                           lambda: (1 << 45) + rnd.randrange(0, 1 << 28) * 16,
                           lambda: 0x700000000 + rnd.randrange(0, 1 << 20) * 16,
                           lambda: 0x0020000040500000 + rnd.randrange(0, 1 << 10) * 16])()
    kind3 = lambda: rnd.choice((0, 1, 2))
    seqs = []
    for t in range(threads):
        mode = t % NPAT
        fixed = rnd.choice((0, 1, 2))
        kk = (lambda: fixed) if rnd.random() < 0.6 else kind3
        if mode == 0:      # adjacent boundaries, entries with different deltas side by side
            bs = rnd.sample([P['Y'][1], P['E2'][1], P['Q'][1], P['AL2'][1], P['X'][1]], 3)
            seq = [((rnd.choice([b - 4, b, b + 4, b - 16, b + 16])), kk()) for b in (bs[j % 3] for j in range(K))]
        elif mode == 1:    # exact ends: inside, end, end - 4, start - 4, start, end + 4 of three pools by turns
            qs = rnd.sample(ps, 3)
            seq = []
            for j in range(K):
                _, s, e, _o = qs[j % 3]
                seq.append((rnd.choice([inside(qs[j % 3]), e, e - 4, s - 4, s, e + 4]) & M64, kk()))
        elif mode == 2:    # the high word changes: L1 L2 L3 at the same low words, across 8 GiB, across 4 GiB
            o = rnd.randrange(0, 0x1000 // 4) * 4
            cand = [P['L1'][1] + o, P['L2'][1] + o, P['L3'][1] + o, 0x1FFFFFFF8, 0x1FFFFFFFC, 0x200000000, 0x200000004,
                    P['S'][2] - 4, P['S'][2], P['S'][1], P['E1'][2] - 4, P['E1'][2], 0xFFFFFFFC, 0x100000004]
            seq = [(cand[j % 3] if j % 2 == 0 else rnd.choice(cand), kk()) for j in range(K)]
        elif mode == 3:    # the hint path
            h1, h2, c, p, q, g2 = P['H1'], P['H2'], P['C'], P['P'], P['Q'], P['G2']
            cand = [h1[1] + rnd.randrange(0, 2 * MiB // 4) * 4,                       # chunk 512: its slot is C's
                    h1[1] + 4 * MiB + rnd.randrange(0, 2 * MiB // 4) * 4,             # chunk 514: hinted
                    h1[2] - 4, c[1] + rnd.randrange(0, 4 * MiB // 4) * 4, c[1],     # C: hinted (slots 512, 513)
                    h2[1] + 4, 0x50200000, 0x503FFFFC, 0x50400000, h2[2] - 4,      # H2: head, hinted chunk, tail
                    p[1], p[2] - 4, q[1], q[1] + 4, p[2] + 0x1000,                  # P hinted | Q, then no pool
                    g2[1] + rnd.randrange(0, 0x400) * 4,                            # G2: the tag of slot 514 matches
                    0x0020000040500000, 0x0020000040400000 - 4]                     # near G2, no pool
            seq = [(rnd.choice(cand), kk()) for _ in range(K)]
        elif mode == 4:    # a lookup in no pool after every hit: 0, never the cached entry's delta
            qs = rnd.sample(plain, 4)
            seq = []
            for j in range(K):
                q = qs[(j // 2) % 4]
                k = kk()
                seq.append((inside(q) if j % 2 == 0 else rnd.choice([q[2], q[2] + 0x100, q[1] - 4, q[1] - 16, far()]) & M64, k))
        elif mode == 5:    # special addresses
            k0, tt = P['K0'], P['T']
            seq = [(0, kk()), (4, kk()), (0x10, kk()), (inside(k0), kk()), (0, kk()), (inside(tt), kk()), (tt[2], kk()),
                   (M64 - 3, kk()), (tt[2] - 4, kk()), (tt[1], kk()), (tt[1] - 4, kk()), (M64, kk()), (inside(tt), kk())]
            seq += [(rnd.choice([0, M64, M64 - 3, inside(tt), inside(k0), tt[2]]), kk()) for _ in range(K - len(seq))]
        elif mode == 6:    # the three helpers over the same pools in different orders
            qs = rnd.sample(ps, 3)
            seq = [(inside(qs[j % 2] if j % 3 else qs[2]), j % 3) for j in range(K)]
        elif mode == 7:    # random walk with edges
            qs = rnd.sample(ps, 6)
            cur, seq = qs[0], []
            for _ in range(K):
                if rnd.random() < 0.3:
                    cur = rnd.choice(qs)
                _, s, e, _o = cur
                seq.append((rnd.choice([s, e, e - 4, s - 4, inside(cur), inside(cur)]) & M64, kind3()))
        elif mode == 8:    # five pools by turns (more than four entries)
            qs = rnd.sample(ps, 5)
            seq = [(inside(qs[j % 5]), kk()) for j in range(K)]
        elif mode == 9:    # LRU reuse: mostly one of the last four pools used
            qs = rnd.sample(ps, 6)
            recent, seq = [], []
            for _ in range(K):
                q = rnd.choice(recent[:4]) if recent and rnd.random() < 0.75 else rnd.choice(qs)
                if q in recent:
                    recent.remove(q)
                recent.insert(0, q)
                seq.append((inside(q), kk()))
        elif mode == 10:   # the alias pair (one memory, two ranges) and X | Y
            qs = [P['AL1'], P['AL2'], P['X'], P['Y']]
            seq = [(inside(qs[rnd.randrange(4)]) if rnd.random() < 0.7 else rnd.choice([P['AL1'][2], P['X'][2], P['AL2'][1] - 4]), kk())
                   for _ in range(K)]
        else:              # mostly lookups in no pool, now and then a hit
            qs = rnd.sample(plain, 3)
            seq = [((inside(rnd.choice(qs)) if rnd.random() < 0.3 else far()) & M64, kk()) for _ in range(K)]
        assert len(seq) == K, mode
        seqs.append([(a & M64, k) for a, k in seq])
    return seqs


# ---------------------------------------------------------------- the kernel
def v2_helper(v4):
    """the translator's 128-bit helper cut to a 64-bit load (name and entry word of its own)"""
    s = re.sub(r'\b' + v4['name'] + r'\(', V2NAME + '(', v4['src'])
    s = re.sub(r'\b' + v4['last'] + r'\b', V2LAST, s)
    m = re.search(r'uvec4 (_\d+) = uvec4Pointer\((.*)\)\.value;\n(\s*)(_\d+)\[0\] = \1\.y;\n\s*\4\[1\] = \1\.z;\n\s*\4\[2\] = \1\.w;\n', s)
    assert m, 'V4 load not found'
    return s[:m.start()] + f'uvec2 {m.group(1)} = uvec2Pointer({m.group(2)}).value;\n{m.group(3)}{m.group(4)}[0] = {m.group(1)}.y;\n' + s[m.end():]


def kernel(real):
    hs = gwt.helpers_of(real)
    w, v4 = hs['W'], hs['V4']
    table, vec = w['table'], v4['vec']
    v2 = v2_helper(v4)
    return f'''#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = {THREADS}, local_size_y = 1, local_size_z = 1) in;

layout(buffer_reference) buffer uintPointer;
layout(buffer_reference) buffer uvec2Pointer;
layout(buffer_reference) buffer uvec4Pointer;

layout(buffer_reference, buffer_reference_align = 4) buffer uintPointer
{{
    uint value;
}};

layout(buffer_reference, buffer_reference_align = 4) buffer uvec2Pointer
{{
    uvec2 value;
}};

layout(buffer_reference, buffer_reference_align = 4) buffer uvec4Pointer
{{
    uvec4 value;
}};

layout(set = 0, binding = 5, std430) buffer {table[:-2]}
{{
    uint _m0[];
}} {table};

layout(set = 0, binding = 6, std430) readonly buffer t_in_t
{{
    uint a[];
}} t_in;

layout(set = 0, binding = 7, std430) writeonly buffer t_out_t
{{
    uint w[];
}} t_out;

uint {w['last']} = 0u;
uint {v4['last']} = 0u;
uint {V2LAST} = 0u;
uint {vec}[3];

{v4['src']}{v2}{w['src']}void main()
{{
    uint id = gl_WorkGroupID.x * {THREADS}u + gl_LocalInvocationID.x;
    {vec}[0] = {DEAD[0]}u; {vec}[1] = {DEAD[1]}u; {vec}[2] = {DEAD[2]}u;
    for (uint k = 0u; k < {K}u; k++)
    {{
        uint at = (id * {K}u + k) * 3u;
        int a_lo = int(t_in.a[at]);
        int a_hi = int(t_in.a[at + 1u]);
        uint kind = t_in.a[at + 2u];
        int r;
        if (kind == 0u)
        {{
            r = {w['name']}(a_lo, a_hi);
        }}
        else if (kind == 1u)
        {{
            r = {v4['name']}(a_lo, a_hi);
        }}
        else
        {{
            r = {V2NAME}(a_lo, a_hi);
        }}
        t_out.w[id * {K}u + k] = uint(r) * 3u + {vec}[0] * 5u + {vec}[1] * 7u + {vec}[2] * 11u;
    }}
}}
'''


def instrument(glsl):
    """counters c[base + 0..5]: calls, searches, none, hits of entries 1..3; base W 0, V4 8, V2 16"""
    out = glsl
    for name, start, end in sorted(gmemfast.functions(glsl), key=lambda f: -f[1]):
        src = glsl[start:end]
        if not src.startswith('int ') or 'Pointer(uvec2(' not in src:
            continue
        base = 8 if 'uvec4Pointer(' in src else 16 if 'uvec2Pointer(' in src else 0
        cnt = lambda k: f'atomicAdd(dmsh_cnt.c[{base + k}], 1u);\n'
        ins = [(src.index('{\n') + 2, '    ' + cnt(0))]
        if 'dmsh_gmem_find(' in src:
            at = src.index('uint e = dmsh_gmem_find(')
            ins.append((src.rindex('\n', 0, at) + 1, '        ' + cnt(1)))
            for m in re.finditer(r'if \(dmsh_gmem_in\(dmsh_g_\d+_(\d)s, [^\n]*\n\s*\{\n', src):
                ins.append((m.end(), '            ' + cnt(2 + int(m.group(1)))))
        else:
            ins.append((re.search(r'\n    if \(!_\d+\)\n    \{\n', src).end(), '        ' + cnt(1)))
        ins.append((src.rindex('    return 0;\n'), '    ' + cnt(2)))
        for pos, text in sorted(ins, key=lambda x: -x[0]):
            src = src[:pos] + text + src[pos:]
        out = out[:start] + src + out[end:]
    first = re.search(r'^\w[\w ]*\s+\w+\([^;{]*\)\n\{', out, re.M).start()
    return out[:first] + 'layout(set = 0, binding = 8, std430) buffer dmsh_cnt_t { uint c[24]; } dmsh_cnt;\n\n' + out[first:]


def variant(glsl, v):
    if v == 'prod':
        return glsl
    w = 4 if v == 'shared' else v
    g, n, _ = gmemfast.rewrite_raw(glsl, strict=False, ways=w)
    assert n == 3, n
    if v == 'shared':   # the cache words in workgroup memory: the counts must notice
        g = re.sub(r'^uvec2 (dmsh_g_\d+_\w+) = uvec2\(0u\);$', r'shared uvec2 \1;', g, flags=re.M)
    return gmemfast.rewrite_umulx(g)[0]


def run_one(tmp, runner, kernel16, groups, seed, variants=('prod', 1, 2, 3, 4, 'shared')):
    d = tmp / f'adv-{kernel16}'; d.mkdir()
    real = gwt.cross(sorted(gwt.S.glob(kernel16 + '*-*.spv'))[0])
    glsl = kernel(real)
    ps, pool_bytes = pools()
    tab = Table(ps)
    rnd = random.Random(seed)
    seqs = sequences(ps, groups * THREADS, rnd)
    inw = [x for seq in seqs for a, k in seq for x in (a & M32, a >> 32, k)]
    (d / 'in.bin').write_bytes(struct.pack('<%dI' % len(inw), *inw))
    (d / 'spec.bin').write_bytes(struct.pack('<%dQ' % (1 + 3 * len(ps)), pool_bytes, *[x for p in ps for x in p[1:]]))
    bad = 0
    want_by = {}
    for n in (1, 2, 3, 4):
        outs, tot, hinted = [], {k: [0] * 6 for k in (0, 1, 2)}, 0
        for seq in seqs:
            o, c, h = simulate(tab, pool_bytes // 4, seq, n)
            outs += o; hinted += h
            for k in tot:
                tot[k] = [x + y for x, y in zip(tot[k], c[k])]
        want_by[n] = (outs, tot, hinted)
    want = want_by[1][0]
    assert all(want_by[n][0] == want for n in want_by), 'the model itself differs by entries'
    nz = sum(1 for seq in seqs for a, k in seq if not tab.holds(tab.find(a)[0], a))
    print(f'{kernel16}: {len(seqs)} threads x {K} = {len(want)} lookups, {nz} in no pool, hinted finds (n=1) {want_by[1][2]}', flush=True)
    for v in variants:
        res = {}
        for cnt in (False, True):
            tag = f'{v}{"_cnt" if cnt else ""}'
            g = variant(glsl, v)
            if cnt:
                g = instrument(g)
            (d / f'{tag}.comp').write_text(g)
            e = mw.compile_module(d / f'{tag}.comp', d / f'{tag}.spv', 'comp', 'vulkan1.2')
            if e:
                print(f'  {tag}: did not build: {e}'); bad += 1; continue
            od = d / ('run_' + tag); od.mkdir()
            args = [str(runner), str(d / f'{tag}.spv'), str(groups), '0', f'T:5:{d / "spec.bin"}', f'6:s:{len(inw) * 4}:{d / "in.bin"}',
                    f'7:s:{len(want) * 4}'] + (['8:s:96'] if cnt else [])
            r = gwt.run(args, cwd=od)
            if r.returncode:
                print(f'  {tag}: the run DIED ({(r.stderr.strip() or str(r.returncode))[:160]})')
                res[tag] = -1; bad += v != 'shared'; continue
            got = struct.unpack('<%dI' % len(want), (od / 'out_7.bin').read_bytes())
            wrong = [i for i in range(len(want)) if got[i] != want[i]]
            res[tag] = len(wrong)
            if wrong:
                j = wrong[0]; t, k = divmod(j, K)
                print(f'  {tag}: {len(wrong)} words differ; first thread {t} (pattern {t % THREADS % NPAT}) lookup {k} '
                      f'address {seqs[t][k][0]:#x} kind {seqs[t][k][1]}: {got[j]:#x}, want {want[j]:#x}')
                bad += v != 'shared'
            if cnt:
                c = struct.unpack('<24I', (od / 'out_8.bin').read_bytes())
                n = 1 if v == 'prod' else 4 if v == 'shared' else v
                exp = want_by[n][1]
                gotc = {k: list(c[b:b + 6]) for k, b in ((0, 0), (1, 8), (2, 16))}
                if v == 'prod':   # the translator's helper has no entries 1..3
                    exp = {k: x[:3] + [0, 0, 0] for k, x in exp.items()}
                same = gotc == exp
                res['counts'] = same
                if not same and v != 'shared':
                    bad += 1
                print(f'  {tag}: counts {"EQUAL" if same else "DIFFER"} model n={n}: ' + '; '.join(
                    f'{nm} {gotc[k]}' + ('' if gotc[k] == exp[k] else f' (model {exp[k]})') for k, nm in ((0, 'W'), (1, 'V4'), (2, 'V2'))))
        print(f'  {v}: words wrong {res.get(str(v), "-")}, twin {res.get(str(v) + "_cnt", "-")}, counts {res.get("counts")}', flush=True)
        if v == 'shared':
            noticed = res.get('counts') is False or res.get('shared', 0) or res.get('shared_cnt', 0)
            print(f'  shared cache words: {"NOTICED" if noticed else "NOT NOTICED"}')
            bad += not noticed
    return bad


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        runner = tmp / 'gmem_run'
        r = gwt.run(['gcc', '-O1', '-o', str(runner), str(HERE / 'gmem_run.c'), '-lvulkan'])
        if r.returncode:
            sys.exit(r.stderr)
        import os
        groups, seed = int(os.environ.get('ADV_GROUPS', '24')), int(os.environ.get('ADV_SEED', '7109'))   # more: -e ADV_GROUPS=96
        bad = run_one(tmp, runner, '73dc0b90c4082e43', groups, seed)
        bad += run_one(tmp, runner, '3439aaa071d868ae', max(groups * 2 // 3, 1), seed + 1000)
        print('adversarial:', 'PASS' if not bad else 'FAIL', bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
