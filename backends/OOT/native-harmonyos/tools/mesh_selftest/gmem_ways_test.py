"""Differential test of gmemfast.rewrite_raw's N-entry cache (DMSH_GMEM_WAYS, oot-a109) against the translator's raw
global memory helpers, on lavapipe (mng-vk-host:lavapipe). Every comparison is word for word.

synthetic  A compute kernel with two of the translator's own helpers (SPIRV-Cross text of a game kernel: the word helper
           and the 128-bit one) and lookups from an input buffer, over a memory pool table built as the replay builds it
           (gmem_run.c, after overlay nvn_vk.c gmem_rebuild: 2507 pools, hints, entry 0 empty, unused entries ~0). Modules:
           prod (the helpers as the translator wrote them), w1 (the one-entry form, text of gmemfast.py.a108), w2, w3, w4.
           Every output word must be the Python model's ("the pool that holds the address", 0 for none; the 128-bit
           helper's three extra words only on a hit) and prod's. Each thread runs one pattern (the thread's number mod 16):
             0-7   exp-gmem-verify's selftest_gmem.py patterns: one pool, two small / two big pools by turns, any pool,
                   pool edges, the special addresses (0, 4, ~0, a failed lookup then one inside the entry it left), the two
                   pools whose chunks share hint slots, a walk
             8-11  2, 3, 4 and 5 pools by turns, one helper (5 is past the most entries: the least recently used goes)
             12    two pools by turns with an address in NO pool between every two (far from all: a load there kills the run)
             13    three pools by turns, at their edges (start - 4, start, end - 16, end - 4, end, end + 4) and inside
             14    the game kernels' order (sites.py): P2 P2 P3 P4 P4 P1 x 6, twice
             15    the two adjacent pools by turns, at the boundary between them
           Instrumented twins count per helper the calls, the searches (dmsh_gmem_find / the translator's miss branch),
           the lookups in no pool and the hits of entries 1..3: calls and 'none' must be equal in all, and the searches
           must fall as the entries grow.
miss       The same with every lookup in no pool and far from all of them (as selftest_gmem.py's gmem-miss case): every
           module must return 0 everywhere and live (a load there is a SIGSEGV on lavapipe).
mutate     Four wrong N-entry forms (a moved entry keeps the old delta; the range check includes the end; the search's
           result is not range-checked; a hit in entry k does not move entry k's end) must be noticed by 'synthetic'.
whole      The game's kernels whole through the wrapper with its default modes (as exp-gmem-verify v_kernel_eq.py):
           base (no post-pass), w1, auto (DMSH_GMEM_WAYS=auto) where it differs from w1, w4, on synthetic buffers in
           10 scenarios (8 of v_kernel_eq.py: uniform words and storage words of one value; 2 'spread': storage word
           pairs 2j, 2j + 1 are a pointer into pool j, pool memory words below 64), every storage buffer word for word
           after each of 12 dispatches. w1's module must equal prepared/oot/mesh-build/wrapped-a107's byte for byte.
           Instrumented twins (base, w1, auto / w4) in 4 scenarios: calls and 'none' equal, searches per helper.
           oot-a122C (PF=1, the default): also pf1 / pfl1 (f1 with the prefetch, sg / lane), pw1 (sg with FIND) and pfauto
           (where auto differs), on both tables and pf1 on the table without fine hints (@0); every prefetch module must
           equal f1 bit for bit as well ('prefetch vs f1'), and pf1 / pfl1 must be byte for byte the shipped modules
           (wrapped-a122C, exp-par/C/wrapped-a122C-lane). TEX_STUB=1 replaces sampler2D textureLod by a fixed function in
           every module alike (the runner binds no images: eb021183 died at it in 7 of 10 scenarios). NAN_SAME=1 (default):
           a word NaN in both with another sign counts as equal against base (LLVM picks a NaN's sign by code shape) and is
           reported as '(+n NaN sign)'.
prefetch   (oot-a122C) a kernel of the game kernels' shape (pointer words loaded at main's top, 28 call sites at pointer +
           64-bit offset) with the two helper forms: prod, f1, pf1, pfl1, pw1, pf4 and their counted twins against the
           Python model; workgroups with one pointer set, per-lane pointers, lane 0 alone different, pointers in no pool / 0 /
           the top, at pool ends, past the hint tag; addresses inside, at the edges, in other pools, in none. The prefetch
           must take every site's pointer and search less than f1.
pfmutate   three wrong prefetch forms (the delta not copied with the range, the entry read two words off, another
           pointer's delta) must be noticed by 'prefetch'.

  docker run --rm -v <port>:/repo:ro mng-vk-host:lavapipe python3 \\
      /repo/native-harmonyos/tools/mesh_selftest/gmem_ways_test.py synthetic|fine|miss|mutate|whole [k16 ...]|prefetch|pfmutate
  (whole: one kernel a run keeps it under 5 minutes; -e SCEN_ONLY=0,1,2 runs a part of the 10 scenarios, -e SKIP_RUN=w1,w1_cnt
   builds those modules without running them)
(the GLSL of game kernels is game-derived: the shader folder is read from the repo, nothing is written there)"""
import bisect, json, os, random, re, shutil, struct, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import gmemfast                        # noqa: E402
import mesh_wrap_in_container as mw   # noqa: E402

if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):
    os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
B = Path('/repo/prepared/oot/mesh-build')
S = B / 'shaders' / 'oot131072'
A107 = B / 'wrapped-a107'
SHIP = B / 'wrapped-a122C'                                  # oot-a122C: DMSH_GMEM_PREFETCH=sg
SHIP_LANE = B / 'exp-par' / 'C' / 'wrapped-a122C-lane'      # DMSH_GMEM_PREFETCH=lane
CROSS = '/repo/prepared/oot/seed-build/spirv-cross/spirv-cross'
THREADS, K, NPAT = 32, 24, 16
MiB, M64 = 1 << 20, (1 << 64) - 1
DEAD = [0xdead0000, 0xdead0001, 0xdead0002]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)


def cross(spv):
    r = run([CROSS, '--vulkan-semantics', str(spv)])
    if r.returncode:
        sys.exit(f'spirv-cross {spv}: {r.stderr[:200]}')
    return r.stdout


def helpers_of(glsl):
    hs = {}
    for name, start, end in gmemfast.functions(glsl):
        h = gmemfast.helper_info(glsl, start, end)
        if h:
            hs[h['kind']] = h
    return hs


def instrument(glsl, binding_text):
    """counters in the raw helpers, translator's form or rewritten: c[0..5] the word helper's calls, searches, lookups in no
    pool, hits of entries 1, 2, 3; c[8..13] the 128-bit helper's"""
    out = glsl
    for name, start, end in sorted(gmemfast.functions(glsl), key=lambda f: -f[1]):
        src = glsl[start:end]
        if not src.startswith('int ') or 'Pointer(uvec2(' not in src:
            continue
        base = 8 if 'uvec4Pointer(' in src else 0
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
    # c[15]: the steps of dmsh_gmem_find's search (oot-a111: the fine hints shorten it)
    m = re.search(r'    for \(uint step = [^\n]*\n    \{\n', out)
    if m:
        out = out[:m.end()] + '        atomicAdd(dmsh_cnt.c[15], 1u);\n' + out[m.end():]
    out = instrument_pf(out)
    first = re.search(r'^\w[\w ]*\s+\w+\([^;{]*\)\n\{', out, re.M).start()
    return out[:first] + binding_text + '\n' + out[first:]


def rewrite(glsl, ways):
    """the module text of a variant: 'prod' / 'base' the translator's helpers, else gmem_pass with that many entries;
    'f1'..'f4' / 'fauto': that many entries with dmsh_gmem_find reading the fine hints (FIND_FINE, oot-a111);
    'pf1' / 'pfauto': f1 / fauto with the subgroup-uniform prefetch (DMSH_GMEM_PREFETCH=sg, oot-a122C); 'pfl1': f1 with the
    per-thread prefetch (lane); 'pw1': 1 entry, FIND (no fine hints) and the sg prefetch"""
    if ways in ('prod', 'base'):
        return glsl
    pf = '0'
    if isinstance(ways, str) and ways.startswith('pfl'):
        pf, ways = 'lane', 'f' + ways[3:]
    elif isinstance(ways, str) and ways.startswith('pf'):
        pf, ways = 'sg', 'f' + ways[2:]
    elif isinstance(ways, str) and ways.startswith('pw'):
        pf, ways = 'sg', ways[2:]
    fine = isinstance(ways, str) and ways.startswith('f')
    g, n, _ = gmemfast.rewrite_raw(glsl, ways=ways[1:] if fine else ways, fine='1' if fine else '0', prefetch=pf)
    assert n, 'no helper rewritten'
    return gmemfast.rewrite_umulx(g)[0]


def instrument_pf(glsl):
    """c[14]: the prefetched entries taken into a helper's entry at a run's first call (oot-a122C)"""
    return re.sub(r"(// the run's first call: the prefetched entry\n(\s*)\{\n)", lambda m: m.group(1) + m.group(2) + '    atomicAdd(dmsh_cnt.c[14], 1u);\n', glsl)


# ---------------------------------------------------------------- synthetic
def synth_kernel(real):
    hs = helpers_of(real)
    assert set(hs) == {'W', 'V4'}, sorted(hs)
    w, v4 = hs['W'], hs['V4']
    table, vec = w['table'], v4['vec']
    return f'''#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = {THREADS}, local_size_y = 1, local_size_z = 1) in;

layout(buffer_reference) buffer uintPointer;
layout(buffer_reference) buffer uvec4Pointer;

layout(buffer_reference, buffer_reference_align = 4) buffer uintPointer
{{
    uint value;
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
uint {vec}[3];

{v4['src']}{w['src']}void main()
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
        else
        {{
            r = {v4['name']}(a_lo, a_hi);
        }}
        t_out.w[id * {K}u + k] = uint(r) * 3u + {vec}[0] * 5u + {vec}[1] * 7u + {vec}[2] * 11u;
    }}
}}
'''


def pools():
    """(guest start, guest end, offset in the pool memory), ascending (selftest_gmem.py's)"""
    ps, off = [], 0
    def add(start, size):
        nonlocal off
        ps.append((start, start + size, off)); off += size
    for i in range(2500):
        add(0x08000000 + i * 0x5000, 0x1000)              # small pools with gaps: no chunk whole, the search
    add(0x30000000, 0x8000); add(0x30008000, 0x8000)      # adjacent
    add(0x7ff00000, 8 * MiB)                              # across 2 GiB, whole chunks: hints
    add(0x100000000 + 7 * 0x200000, 6 * MiB)              # above 4 GiB: chunks 2055..2057
    add(0x500000000 + 7 * 0x200000, 4 * MiB)              # chunks 10247, 10248: the hint slots of 2055, 2056
    add(1 << 41, 2 * MiB + 0x2000)                        # past the hint's tag (high word >= 256): the search
    add(0xffffffffff000000, 0xfff000)                     # the top of the address space, under the unused entries' ~0
    return ps, off + 0x10000


def word(i):
    return (i * 2654435761 ^ 0x5bd1e995) & 0xffffffff


def far(rnd):
    """an address in no pool, far from all (address + any entry's delta is not memory)"""
    return rnd.choice([lambda: rnd.randrange(0, 0x1000) * 16,
                       lambda: 0x0000008000000080 + rnd.randrange(0, 1 << 20) * 16,
                       lambda: 0x0000002000000000 + rnd.randrange(0, 1 << 24) * 16,
                       lambda: (1 << 45) + rnd.randrange(0, 1 << 28) * 16,
                       lambda: 0x0000000200000000 + rnd.randrange(0, 1 << 20) * 16,
                       lambda: M64 - 15 - rnd.randrange(0, 8) * 16])() & M64


def sequences(ps, threads, rnd, miss=False):
    small, big = ps[:2500], ps[2500:]
    kind = lambda: rnd.choice((0, 1, 1))
    def inside(p, k):
        s, e, _ = p
        return s + rnd.randrange(0, (e - s) // 16) * 16 if k == 1 else s + rnd.randrange(0, (e - s) // 4) * 4
    def edge(p):
        s, e, _ = p
        return rnd.choice([s - 4, s - 16, s, s + 4, e - 16, e - 4, e, e + 4, e + 0x1000]) & M64
    def turns(n, k):
        qs = rnd.sample(ps, n) if rnd.random() < 0.5 else rnd.sample(small, n - 1) + [rnd.choice(big)]
        return [(inside(qs[j % n], k), k) for j in range(K)]
    seqs = []
    for i in range(threads):
        mode, k1 = i % NPAT, rnd.choice((0, 1))
        if miss:
            seq = [(far(rnd), kind()) for _ in range(K)]
        elif mode == 0:      # one pool
            p = rnd.choice(ps); seq = [(inside(p, k), k) for k in (kind() for _ in range(K))]
        elif mode == 1:      # two small pools by turns
            p, q = rnd.choice(small), rnd.choice(small)
            seq = [(inside(p if j % 2 else q, k), k) for j, k in enumerate(kind() for _ in range(K))]
        elif mode == 2:      # two big pools by turns of 3
            p, q = rnd.choice(big), rnd.choice(big)
            seq = [(inside(p if (j // 3) % 2 else q, k), k) for j, k in enumerate(kind() for _ in range(K))]
        elif mode == 3:      # any pool
            seq = [(inside(rnd.choice(ps), k), k) for k in (kind() for _ in range(K))]
        elif mode == 4:      # the edges of pools
            seq = [(edge(rnd.choice(ps if j % 3 else big)), kind()) for j in range(K)]
        elif mode == 5:      # the special addresses, a failed lookup followed by one inside the entry the failure left
            p = rnd.choice(small); q = rnd.choice(big)
            k2 = kind()
            seq = [(0, k1), (0, 1 - k1), (4, 0), (M64, 0), (M64, 1), (0xfffffffffffffff0, kind()),
                   (p[1] + 0x100, k1), (inside(p, k1), k1), (p[1], k1), (inside(p, k1), k1),
                   (q[1], k2), (inside(q, k2), k2), (ps[0][0] - 16, k2), (inside(ps[0], k2), k2), (M64, k2), (inside(ps[-1], k2), k2)]
            seq += [(inside(rnd.choice((p, q)), k), k) for k in (kind() for _ in range(K - len(seq)))]
        elif mode == 6:      # the two pools whose chunks share hint slots, and the chunk only one of them has
            d, e = ps[2503], ps[2504]
            seq = []
            for j in range(K):
                k = kind()
                seq.append((inside(d if j % 2 else e, k) if j % 5 else d[0] + 2 * 0x200000 + rnd.randrange(0, 0x1000) * 16, k))
        elif mode == 7:      # a walk
            cur, seq = rnd.choice(ps), []
            for _ in range(K):
                k = kind()
                if rnd.random() < 0.2: cur = rnd.choice(ps)
                seq.append((edge(cur) if rnd.random() < 0.25 else inside(cur, k), k))
        elif mode in (8, 9, 10, 11):   # 2, 3, 4, 5 pools by turns through one helper
            seq = turns(mode - 6, k1)
        elif mode == 12:     # two pools by turns, an address in no pool between every two
            p, q = rnd.sample(ps, 2)
            seq = [((inside(p if (j // 2) % 2 else q, k1), k1) if j % 2 == 0 else (far(rnd), k1)) for j in range(K)]
        elif mode == 13:     # three pools by turns at their edges
            qs = rnd.sample(ps, 3)
            seq = [((rnd.choice([qs[j % 3][0] - 4, qs[j % 3][0], qs[j % 3][1] - 16, qs[j % 3][1] - 4, qs[j % 3][1], qs[j % 3][1] + 4]) & M64)
                    if rnd.random() < 0.6 else inside(qs[j % 3], k1), k1) for j in range(K)]
        elif mode == 14:     # the game kernels' order
            p1, p2, p3, p4 = rnd.sample(ps, 4)
            order = [p2, p2, p3, p4, p4] + [p1] * 7
            seq = [(inside(order[j % 12], 1), 1) for j in range(K)]
        else:                # the adjacent pools by turns, at the boundary
            a, b = ps[2500], ps[2501]
            assert a[1] == b[0]
            seq = [(rnd.choice([a[1] - 16, a[1] - 4, a[1], b[0] + 4, inside(a, k1), inside(b, k1)]), k1) for _ in range(K)]
        assert len(seq) == K
        seqs.append(seq)
    return seqs


def model(ps, total_words, seq):
    starts = [p[0] for p in ps]
    vec, out, hit = list(DEAD), [], 0
    for a, k in seq:
        r = 0
        j = bisect.bisect_right(starts, a) - 1
        if j >= 0 and a < ps[j][1]:
            w = (ps[j][2] + a - ps[j][0]) // 4
            assert w + 3 < total_words
            r = word(w)
            hit += 1
            if k == 1:
                vec = [word(w + 1), word(w + 2), word(w + 3)]
        out.append((r * 3 + vec[0] * 5 + vec[1] * 7 + vec[2] * 11) & 0xffffffff)
    return out, hit


CNT_DECL = 'layout(set = 0, binding = 8, std430) buffer dmsh_cnt_t { uint c[16]; } dmsh_cnt;\n'
CNT_NAMES = ['calls', 'searches', 'none', 'hit e1', 'hit e2', 'hit e3']


def synthetic(tmp, runner, name, kernel16, groups, seed, miss=False, variants=('prod', 1, 2, 3, 4), quiet=False, fine_table='0'):
    d = tmp / name; d.mkdir()
    real = cross(sorted(S.glob(kernel16 + '*-*.spv'))[0])
    glsl = synth_kernel(real)
    mods = {}
    for v in variants:
        for cnt in (False, True):
            tag = f'{v}{"_cnt" if cnt else ""}'
            g = rewrite(glsl, v)
            if cnt:
                g = instrument(g, CNT_DECL)
            (d / f'{tag}.comp').write_text(g)
            e = mw.compile_module(d / f'{tag}.comp', d / f'{tag}.spv', 'comp', 'vulkan1.2')
            if e:
                sys.exit(f'{name}/{tag}: {e}')
            mods[tag] = d / f'{tag}.spv'
    rnd = random.Random(seed)
    ps, pool_bytes = pools()
    seqs = sequences(ps, groups * THREADS, rnd, miss)
    inw = [x for seq in seqs for a, k in seq for x in (a & 0xffffffff, a >> 32, k)]
    (d / 'in.bin').write_bytes(struct.pack('<%dI' % len(inw), *inw))
    (d / 'spec.bin').write_bytes(struct.pack('<%dQ' % (1 + 3 * len(ps)), pool_bytes, *[x for p in ps for x in p]))
    want, hits = [], 0
    for seq in seqs:
        o, h = model(ps, pool_bytes // 4, seq)
        want += o; hits += h
    bad, outs, counts = 0, {}, {}
    for tag, path in mods.items():
        od = d / ('run_' + tag); od.mkdir()
        args = [str(runner), str(path), str(groups), '0', f'T:5:{d / "spec.bin"}', f'6:s:{len(inw) * 4}:{d / "in.bin"}', f'7:s:{len(want) * 4}']
        if tag.endswith('_cnt'):
            args.append('8:s:64')
        r = run(args, cwd=od, env=dict(os.environ, GMEM_FINE=fine_table))
        if r.returncode:
            bad += 1; outs[tag] = None
            print(f'  {name}/{tag}: the run DIED ({(r.stderr.strip() or str(r.returncode))[:120]})')
            continue
        got = struct.unpack('<%dI' % len(want), (od / 'out_7.bin').read_bytes())
        outs[tag] = got
        wrong = sum(1 for x, y in zip(got, want) if x != y)
        if wrong:
            j = next(i for i in range(len(want)) if got[i] != want[i])
            t, k = divmod(j, K)
            print(f'  {name}/{tag}: {wrong} words differ from the model; first: thread {t} (pattern {t % THREADS % NPAT}) lookup {k} '
                  f'address {seqs[t][k][0]:#x} kind {seqs[t][k][1]}: {got[j]:#x}, want {want[j]:#x}')
        bad += wrong
        if tag.endswith('_cnt'):
            counts[tag[:-4]] = struct.unpack('<16I', (od / 'out_8.bin').read_bytes())
    for tag in outs:
        if tag != 'prod' and outs[tag] is not None and outs.get('prod') is not None and outs[tag] != outs['prod']:
            bad += 1; print(f'  {name}: {tag} != prod')
    lines = []
    for v in variants:
        c = counts.get(str(v))
        if c is None:
            continue
        lines.append(f'{v}: ' + '; '.join(f'{h} ' + ' '.join(f'{n} {c[b + i]}' for i, n in enumerate(CNT_NAMES) if i < 3 or c[b + i])
                                          for h, b in (('word', 0), ('128-bit', 8))))
    ref = counts.get('prod')
    for v, c in counts.items():
        if ref and (c[0], c[2], c[8], c[10]) != (ref[0], ref[2], ref[8], ref[10]):
            bad += 1; lines.append(f'  {v}: calls / none differ from prod')
        if v in ('1', 'f1') and ref and c[:3] + c[8:11] != ref[:3] + ref[8:11]:
            bad += 1; lines.append(f'  {v}: searches differ from prod (the one-entry form takes the same branches)')
    if counts:
        lines.append('search steps (dmsh_gmem_find): ' + ', '.join(f'{v} {c[15]}' for v, c in counts.items() if v != 'prod'))
    if not quiet:
        print(f'{name} (helpers of {kernel16}): {groups * THREADS} threads x {K} lookups = {len(want)}, {hits} in a pool ({len(ps)} pools); '
              f'table GMEM_FINE={fine_table}; modules {", ".join(str(v) for v in variants)} and their instrumented twins: {bad} wrong')
        for l in lines:
            print('    ' + l)
    return bad


def mutations():
    """(name, gmemfast attribute, text replaced, replacement): each must make 'synthetic' fail"""
    return [
        ('moved entry keeps the old delta', 'body_ways', "out += [ind + f'    {word(0, c)} = t{c};' for c in 'sed']",
         "out += [ind + f'    {word(0, c)} = t{c};' for c in 'se']"),
        ('range check includes the end', 'IN', 'bool e_lt = lo < e.x;', 'bool e_lt = lo <= e.x;'),
        ('search result not range-checked', 'body_ways', "ind + 'ok = inside;'", "ind + 'ok = true;'"),
        ('hit in entry k does not move its end', 'body_ways', "out += [ind + f'    uvec2 t{c} = {word(k, c)};' for c in 'sed']",
         "out += [ind + f'    uvec2 t{c} = {word(k if c != \"e\" else 0, c)};' for c in 'sed']"),
    ]


def mutate(tmp, runner):
    import inspect
    noticed = 0
    muts = mutations()
    for i, (what, attr, old, new) in enumerate(muts):
        if attr == 'IN':
            saved = gmemfast.IN
            assert old in saved
            gmemfast.IN = saved.replace(old, new)
            restore = lambda: setattr(gmemfast, 'IN', saved)
        else:
            src = inspect.getsource(gmemfast.body_ways)
            assert old in src, what
            saved = gmemfast.body_ways
            ns = dict(vars(gmemfast))
            exec(src.replace(old, new), ns)
            gmemfast.body_ways = ns['body_ways']
            restore = lambda: setattr(gmemfast, 'body_ways', saved)
        try:
            bad = synthetic(tmp, runner, f'mut{i}', '73dc0b90c4082e43', 16, 4242 + i, variants=('prod', 2, 4), quiet=True)
        finally:
            restore()
        noticed += bad > 0
        print(f'mutation "{what}": {bad} wrong -> {"noticed" if bad else "NOT NOTICED"}')
    print(f'mutate: {noticed} of {len(muts)} mutations noticed:', 'PASS' if noticed == len(muts) else 'FAIL')
    return 0 if noticed == len(muts) else 1


# ---------------------------------------------------------------- whole kernels
info = None
PF = os.environ.get('PF', '1') != '0'   # oot-a122C: build and run the prefetch modules (PF=0: the a111 plan only)
PF_PLAN = {}
SWEEP, CNT_BINDING = 11, 40
SCEN = [('u=1.0 s=0x40', 0x3f800000, 0x40), ('u=0 s=0x40', 0, 0x40), ('u=3 s=0x40', 3, 0x40), ('u=0.5 s=0x40', 0x3f000000, 0x40),
        ('u=0x101 s=0x40', 0x101, 0x40), ('u=1.0 s=0x80', 0x3f800000, 0x80), ('u=1.0 s=0x44', 0x3f800000, 0x44), ('u=2.0 s=0x4c', 0x40000000, 0x4c),
        ('spread u=0', 0, 'spread'), ('spread u=3', 3, 'spread')]
CNT_SCEN = (5, 6, 8, 9)
SCEN_OF = {'99280b7b8aae78df': [('u=0 s=0x40', 0, 0x40), ('u=3 s=0x40', 3, 0x40), ('u=0x101 s=0x40', 0x101, 0x40), ('u=3 s=0x80', 3, 0x80),
                                ('u=3 s=0x44', 3, 0x44), ('u=7 s=0x4c', 7, 0x4c), ('spread u=0', 0, 'spread'), ('spread u=3', 3, 'spread')]}
CNT_OF = {'99280b7b8aae78df': (1, 4, 6, 7)}
SKIP_RUN = set(os.environ.get('SKIP_RUN', '').split(','))   # modules built (and byte-compared) but not run, e.g. w1,w1_cnt,w4
WCNT_DECL = f'layout(binding = {CNT_BINDING}, set = 0, std430) buffer dmsh_cnt_t {{ uint c[16]; }} dmsh_cnt;\n'


def spread_words():
    """storage words: pair (2j, 2j + 1) is the address 0x40_00000100 + j * 8 MiB (pool j of the table below)"""
    w = []
    for j in range(MiB // 8):
        a = 0x4000000100 + j * 8 * MiB
        w += [a & 0xffffffff, a >> 32]
    return struct.pack('<%dI' % len(w), *w)


TEX_STUB = os.environ.get('TEX_STUB', '0') == '1'


def stub_textures(glsl):
    """(TEX_STUB=1, oot-a122C) every sampler2D textureLod(s, c, lod) -> dmsh_tex_stub(c), a fixed function of the coordinate,
    and the sampler's declaration gone: the runner binds no images (a sampler binding got a storage buffer, and eb021183
    died at its textureLod in 7 of the 10 scenarios). Applied to every module of a kernel alike (base too), so the comparison
    of the raw memory forms stays exact; w1 is then not wrapped-a107's module (not checked)."""
    names = re.findall(r'^layout\(set = 0, binding = \d+\) uniform sampler2D (\w+);$', glsl, re.M)
    if not names:
        return glsl
    if re.search(r'^layout\(set = 0, binding = \d+\) uniform (?!sampler2D )\w*sampler\w*', glsl, re.M):
        raise ValueError('a sampler other than sampler2D: no stub')
    for n in names:
        glsl = re.sub(r'^layout\(set = 0, binding = \d+\) uniform sampler2D ' + n + r';\n', '', glsl, flags=re.M)
        while True:
            m = re.search(r'\btextureLod\(' + n + r', ', glsl)
            if not m:
                break
            depth, i, args, start = 1, m.end(), [], m.end()
            while depth:
                ch = glsl[i]
                if ch in '([':
                    depth += 1
                elif ch in ')]':
                    depth -= 1
                elif ch == ',' and depth == 1:
                    args.append(glsl[start:i]); start = i + 1
                i += 1
            args.append(glsl[start:i - 1])
            glsl = glsl[:m.start()] + f'dmsh_tex_stub({args[0].strip()})' + glsl[i:]
        if re.search(r'\b' + n + r'\b', glsl):
            raise ValueError(f'{n} used other than by textureLod: no stub')
    fn = 'vec4 dmsh_tex_stub(vec2 c)\n{\n    return vec4(fract(c * 7.25 + 0.125), 0.5, 1.0);\n}\n\n'
    first = re.search(r'^\w[\w ]*\s+\w+\([^;{]*\)\n\{', glsl, re.M).start()
    return glsl[:first] + fn + glsl[first:]


def build_whole(d, k16, key, name, tag, ways, cnt):
    src = cross(S / name)
    glsl = rewrite(stub_textures(src) if TEX_STUB else src, ways)
    plan = gmemfast.prefetch_last() if isinstance(ways, str) and ways.startswith(('pf', 'pw')) else None
    if plan and not cnt:
        PF_PLAN[(k16, tag)] = plan
    if cnt:
        glsl = instrument(glsl, WCNT_DECL)
    text, binds = mw.wrap(glsl, info[key])
    g = d / f'{k16}_{tag}.comp'; g.write_text(text)
    spv = d / f'{k16}_{tag}.spv'
    ver = struct.unpack_from('<I', (S / name).read_bytes(), 4)[0]
    e = mw.compile_module(g, spv, 'comp', 'vulkan1.1' if ver <= 0x10300 else 'vulkan1.2')
    if e:
        return tag, None, binds, e
    g.unlink()
    return tag, spv, binds, None


def differ(a, b):
    """(words that differ, words that are NaN in both with another sign / payload: counted apart when NAN_SAME)"""
    other = nans = 0
    for fn, x in a.items():
        y = b.get(fn, b'')
        if x == y:
            continue
        if len(x) != len(y):
            other += 1; continue
        for i in range(0, len(x) // 4, 1 << 16):
            cx, cy = x[i * 4:(i + (1 << 16)) * 4], y[i * 4:(i + (1 << 16)) * 4]
            if cx != cy:
                for p, q in zip(struct.unpack('<%dI' % (len(cx) // 4), cx), struct.unpack('<%dI' % (len(cy) // 4), cy)):
                    if p != q:
                        if NAN_SAME and is_nan(p) and is_nan(q):
                            nans += 1
                        else:
                            other += 1
    return other, nans


# (oot-a122C) A float NaN's sign and payload are not defined by SPIR-V for arithmetic results, and lavapipe's LLVM picks
# them by code shape: with TEX_STUB, eb021183's vertex words on garbage inputs come out 0xffc00000 in the translator's form
# and 0x7fc00000 in the a107 / a111 forms (logs/eb02-diff-s0.log: each module is deterministic run to run; f1 == pf1 exactly).
# NAN_SAME=1 (the default) counts a word that is NaN in both as equal and reports how many there were.
NAN_SAME = os.environ.get('NAN_SAME', '1') == '1'


def is_nan(w):
    return (w & 0x7f800000) == 0x7f800000 and (w & 0x007fffff) != 0


def one_kernel(d, runner, k16):
    key = [k for k in info if k.startswith(k16)][0]
    name = sorted(p.name for p in S.glob(key + '-*.spv'))[0]
    i = info[key]
    kd = d / k16; kd.mkdir()
    out = []
    # the modules: auto only where it differs from w1 (the helpers' entries: access_pattern)
    pat = gmemfast.access_pattern(cross(S / name))
    auto_differs = any(x['ways'] > 1 for x in pat.values())
    plan = [('base', 'base', False), ('w1', 1, False), ('w4', 4, False)] + ([('auto', 'auto', False)] if auto_differs else [])
    plan += [('base', 'base', True), ('w1', 1, True), (('auto', 'auto', True) if auto_differs else ('w4', 4, True))]
    plan += [('f1', 'f1', False), ('f1', 'f1', True)]   # oot-a111: run on the split table below (fine hints) as well
    if PF:   # oot-a122C: the prefetch (sg, lane; with 2..4 entries where auto differs; with FIND)
        plan += [('pf1', 'pf1', False), ('pf1', 'pf1', True), ('pfl1', 'pfl1', False), ('pfl1', 'pfl1', True), ('pw1', 'pw1', False)]
        plan += [('pfauto', 'pfauto', False)] if auto_differs else []
    mods, binds = {}, None
    for tag, ways, cnt in plan:
        t, spv, b, e = build_whole(kd, k16, key, name, tag + ('_cnt' if cnt else ''), ways, cnt)
        if e:
            print(f'{k16}: {t} did not build: {e}', flush=True)
            return k16, [f'{k16}: {t} did not build: {e}'], 1
        mods[t] = spv
        if t == 'w1':
            binds = b
    a107 = A107 / name
    same107 = a107.is_file() and mods['w1'].read_bytes() == a107.read_bytes()
    if TEX_STUB:
        same107 = 'not checked (TEX_STUB)'
    out.append(f'{k16} {i["kind"]}: helpers ' + ', '.join(f'{x["kind"]} {x["sites"]} sites {x["pointers"]} pointers misses n=1..4 {x["misses"]} -> {x["ways"]}'
                                                       for x in pat.values())
               + f'; w1 == wrapped-a107 byte for byte: {same107}; sizes base {mods["base"].stat().st_size} w1 {mods["w1"].stat().st_size} '
               + f'w4 {mods["w4"].stat().st_size}' + (f' auto {mods["auto"].stat().st_size}' if 'auto' in mods else ''))
    print(out[-1], flush=True)   # each line as it comes (a run that hits its time limit keeps what it did)
    if PF:
        p = PF_PLAN.get((k16, 'pf1'))
        out.append(f'{k16}: prefetch (pf1): ' + (f"groups (line, pointers) {[(g['at'], sorted(g['keys'].values())) for g in p['groups']]}, "
                                                 f"{len(p['take'])} run starts take an entry, {p['covered']} of {p['sites']} sites read "
                                                 f"through a prefetched pointer; sizes pf1 {mods['pf1'].stat().st_size} pfl1 {mods['pfl1'].stat().st_size}"
                                                 if p else 'nothing to prefetch'))
        print(out[-1], flush=True)
    bad = 0 if same107 else 1
    for tag, folder in (('pf1', SHIP), ('pfl1', SHIP_LANE)):   # the module verified here is the one the seed gets
        shipped = folder / name
        if tag in mods and shipped.is_file() and not TEX_STUB:
            same = mods[tag].read_bytes() == shipped.read_bytes()
            bad += not same
            out.append(f'{k16}: {tag} == {folder.name} byte for byte: {same}'); print(out[-1], flush=True)
    vk = (S / (name[:-4] + '.vk')).read_text().split('\n')
    kb = [(int(l.split()[1]), int(l.split()[3]), int(l.split()[4])) for l in vk if l.startswith('b ')]
    kb += [(b, dtype, kind) for b, dtype, kind, rw in binds]
    ctl = [0] * mw.C_WORDS
    ctl[mw.C_CAP], ctl[mw.C_TCAP], ctl[mw.C_DISP] = 1, 1, (0 if i['kind'] == 'ts' else 1)
    (kd / 'ctl.bin').write_bytes(struct.pack('<%dI' % len(ctl), *ctl))
    spec = [8 * MiB + 65536]
    for k in range(8190):
        s = 0x4000000000 + k * 8 * MiB
        spec += [s, s + (6 if k % 5 == 4 else 8) * MiB, 0]
    (kd / 'spec.bin').write_bytes(struct.pack('<%dQ' % len(spec), *spec))
    # the split table (oot-a111): pools 0..4094 of the one above, each cut in two ranges at 1.5 MiB + (k % 7) pages, the
    # same memory behind every address; the chunk of each cut holds two range starts, so it gets a fine hint (GMEM_FINE=1)
    split = [8 * MiB + 65536]
    for k in range(4095):
        s, cut = 0x4000000000 + k * 8 * MiB, 0x180000 + (k % 7) * 0x1000
        split += [s, s + cut, 0, s + cut, s + (6 if k % 5 == 4 else 8) * MiB, cut]
    (kd / 'spec_split.bin').write_bytes(struct.pack('<%dQ' % len(split), *split))
    spread = spread_words()
    only = [int(x) for x in os.environ.get('SCEN_ONLY', '').split(',') if x.strip()]   # a part of the scenarios (big kernels: runs under 5 min)
    for si, (sc, uword, sword) in enumerate(SCEN_OF.get(k16, SCEN)):
        if only and si not in only:
            continue
        (kd / 'ubo.bin').write_bytes(struct.pack('<I', uword) * 16384)
        if sword == 'spread':
            (kd / 'ssbo.bin').write_bytes(spread)
            (kd / 'in.bin').write_bytes(struct.pack('<2I', 2, 0) + spread[8:65536])
            env = dict(os.environ, GMEM_POOL_MASK='0x3f')
        else:
            (kd / 'ssbo.bin').write_bytes(struct.pack('<I', sword) * (MiB // 4))
            (kd / 'in.bin').write_bytes(struct.pack('<2I', 2, 0) + struct.pack('<I', sword) * 4094)
            env = dict(os.environ)
        a, ctl_b = [], None
        for b, dtype, kind in kb:
            if dtype == 6:
                a.append(f'{b}:u:65536:{kd / "ubo.bin"}')
            elif kind == 16:
                a.append(f'T:{b}:{kd / "spec.bin"}')
            elif kind == mw.RB_MESH_PAR:
                a.append(f'{b}:s:{mw.C_WORDS * 4}:{kd / "ctl.bin"}'); ctl_b = b
            elif kind == mw.RB_MESH_IN:
                a.append(f'{b}:s:65536:{kd / "in.bin"}')
            elif kind == mw.RB_MESH_VTX:
                a.append(f'{b}:s:{max(i["max_v"], 1) * len(i["vertex"]) * 4}')
            elif kind == mw.RB_MESH_IDX:
                a.append(f'{b}:s:{max(i["max_p"], 1) * 12}')
            else:
                a.append(f'{b}:s:{MiB}:{kd / "ssbo.bin"}')
        a.append(f'S:{ctl_b}:{SWEEP}')
        outs, cnts, err = {}, {}, None
        # (run name, module, table, GMEM_FINE): every module on the table above; base, w1, f1 and their twins on the split one
        runs = [(t, spv, 'spec.bin', '1' if t.startswith(('f1', 'pf')) else '0') for t, spv in mods.items()]
        runs += [(t + '@s', mods[t], 'spec_split.bin', '1') for t in ('base', 'w1', 'f1', 'base_cnt', 'w1_cnt', 'f1_cnt',
                                                                        'pf1', 'pf1_cnt', 'pfl1', 'pfl1_cnt') if t in mods]
        # oot-a122C: the prefetch on the table without fine hints (word 7 = 0: dmsh_gmem_find's full search)
        runs += [(t + '@0', mods[t], 'spec.bin', '0') for t in ('pf1', 'pf1_cnt') if t in mods]
        for tag, spv, table, fine in runs:
            t0 = tag.split('@')[0]
            if t0.endswith('_cnt') and si not in CNT_OF.get(k16, CNT_SCEN) or tag in SKIP_RUN:
                continue
            od = kd / f'{tag}_{si}'; od.mkdir()
            ta = [x.replace('/spec.bin', '/' + table) if x.startswith('T:') else x for x in a]
            try:
                r = run([str(runner), str(spv), '1', '0'] + ta + ([f'{CNT_BINDING}:s:64'] if t0.endswith('_cnt') else []), cwd=od, timeout=420,
                        env=dict(env, GMEM_FINE=fine))
            except subprocess.TimeoutExpired:
                err = tag + ' timed out'; break
            if r.returncode:
                err = f'{tag} died ({(r.stderr.strip() or str(r.returncode))[:80]})'; break
            outs[tag] = {p.name: p.read_bytes() for p in sorted(od.glob('out_*.bin')) if p.name != f'out_{CNT_BINDING}.bin'}
            if t0.endswith('_cnt'):
                c = struct.unpack('<%dI' % (16 * (SWEEP + 1)), (od / f'out_{CNT_BINDING}.bin').read_bytes())
                cnts[tag.replace('_cnt', '')] = c[-16:]
            shutil.rmtree(od)
        if err:
            out.append(f'{k16} {sc}: NOT COMPARED: {err}'); print(out[-1], flush=True)
            continue
        refof = lambda m: 'base@s' if m.endswith('@s') else 'base'
        missing = [m for m in outs if refof(m) not in outs]
        if missing:   # (SKIP_RUN took the reference away: nothing to compare those with)
            out.append(f'{k16} {sc}: NOT COMPARED: no reference for {missing}'); print(out[-1], flush=True)
            bad += 1
            continue
        pairs = {m: differ(outs[refof(m)], outs[m]) for m in outs if m not in ('base', 'base@s')}
        nb = sum(o for o, _ in pairs.values())
        text = f'{k16} {sc}: words that differ from base: ' + ', '.join(f'{m} {o}' + (f' (+{n} NaN sign)' if n else '') for m, (o, n) in pairs.items())
        # oot-a122C: a prefetch module against the a111 form (f1) on the same table: word for word, NaNs included. (Not the
        # counted twins: their counters sit in other places than f1_cnt's, which changes LLVM's NaN signs; they are held
        # to base like every other module above.)
        f1of = lambda m: 'f1@s' if m.endswith('@s') else 'f1'
        strict = {m: differ(outs[f1of(m)], outs[m]) for m in outs
                  if m.split('@')[0] in ('pf1', 'pfl1') and f1of(m) in outs}
        if strict:
            text += '; prefetch vs f1, every bit: ' + ', '.join(f'{m} {o + n}' for m, (o, n) in strict.items())
            nb += sum(o + n for o, n in strict.values())
        if cnts:
            for m, c in cnts.items():
                ref = cnts.get(refof(m))
                if ref is None:
                    continue
                same_branches = m.split('@')[0] in ('w1', 'f1')
                if (c[0], c[2], c[8], c[10]) != (ref[0], ref[2], ref[8], ref[10]) or (same_branches and c[:15] != ref[:15]):
                    nb += 1; text += f' COUNTERS of {m} differ'
            ref = cnts['base']
            text += '; totals of the 12 dispatches (word / 128-bit helper): calls ' + f'{ref[0]} / {ref[8]}, in no pool {ref[2]} / {ref[10]}, searches ' + \
                    ', '.join(f'{m} {c[1]} / {c[9]}' for m, c in cnts.items()) + '; search steps ' + \
                    ', '.join(f'{m} {c[15]}' for m, c in cnts.items() if not m.startswith('base'))
            if any(c[14] for c in cnts.values()):
                text += '; prefetched entries taken ' + ', '.join(f'{m} {c[14]}' for m, c in cnts.items() if c[14])
        bad += nb > 0
        out.append(text); print(text, flush=True)
    return k16, out, bad


def whole(tmp, runner, only):
    global info
    info = json.loads((B / 'mesh-info.json').read_text())
    ks = only or ['3439aaa071d868ae', 'fed302d791a4ba43', 'eb021183bf9b0092', '9df3c6c01fec0193', '604134a5262ff670',
                  '17c8ab6419068d4b', '7fd36d22b1f0fd14', '0207d3440d0e8dda', '626fbf48041cb82b', '99280b7b8aae78df']
    total = 0
    with ThreadPoolExecutor(max_workers=int(os.environ.get('JOBS', '4'))) as pool:
        for k16, lines, bad in pool.map(lambda k: one_kernel(tmp, runner, k), ks):
            total += bad
    print(f'whole: {len(ks)} kernels, {total} with a difference:', 'PASS' if not total else 'FAIL')
    return 1 if total else 0


# ---------------------------------------------------------------- prefetch (oot-a122C)
PF_NP, PF_K = 4, 28
# the call sites in program order: (pointer, helper kind 0 = word / 1 = 128-bit). Runs of one pointer per helper, pointers
# coming back, the two helpers interleaved (as 3439aaa0 / fed302d7 / eb021183 read their streams)
PF_SITES = [(0, 0), (0, 0), (1, 1), (1, 1), (2, 1), (0, 0), (2, 1), (3, 1), (3, 1), (3, 0), (1, 1), (3, 1), (2, 0), (2, 0),
            (0, 1), (0, 1), (3, 0), (1, 0), (1, 1), (2, 1), (3, 1), (3, 1), (3, 1), (0, 0), (2, 1), (1, 0), (3, 1), (3, 0)]
assert len(PF_SITES) == PF_K


def pf_kernel(real):
    """a kernel of the game kernels' shape for the prefetch: the pointers' words loaded at main's top from a storage buffer,
    then PF_K call sites at pointer + offset (64-bit add with the translator's carry), each result stored. The thread's
    record in t_in: 2 * PF_NP pointer words, then 2 * PF_K offset words."""
    hs = helpers_of(real)
    assert set(hs) == {'W', 'V4'}, sorted(hs)
    w, v4 = hs['W'], hs['V4']
    table, vec = w['table'], v4['vec']
    rec = 2 * PF_NP + 2 * PF_K
    ids = iter(range(30000, 40000))
    t = lambda: f'_{next(ids)}'
    body = []
    tid, base = t(), t()
    body += [f'    uint {tid} = gl_WorkGroupID.x * {THREADS}u + gl_LocalInvocationID.x;', f'    uint {base} = {tid} * {rec}u;']
    ptr = []
    for j in range(PF_NP):
        il, ih, pl, ph = t(), t(), t(), t()
        body += [f'    uint {il} = {base} + {2 * j}u;', f'    float {pl} = uintBitsToFloat(t_in._m0[int({il})]);',
                 f'    uint {ih} = {base} + {2 * j + 1}u;', f'    float {ph} = uintBitsToFloat(t_in._m0[int({ih})]);']
        ptr.append((pl, ph))
    body += [f'    {vec}[0] = {DEAD[0]}u;', f'    {vec}[1] = {DEAD[1]}u;', f'    {vec}[2] = {DEAD[2]}u;']
    for k, (j, kind) in enumerate(PF_SITES):
        pl, ph = ptr[j]
        a0, a1, ol, oh, lo, c, ci, h, hh, r, o = (t() for _ in range(11))
        body += [f'    uint {a0} = {base} + {2 * PF_NP + 2 * k}u;', f'    int {ol} = int(t_in._m0[int({a0})]) & -1;',
                 f'    uint {a1} = {base} + {2 * PF_NP + 2 * k + 1}u;', f'    int {oh} = int(t_in._m0[int({a1})]) & -1;',
                 f'    int {lo} = floatBitsToInt({pl}) + {ol};',
                 f'    bool {c} = uint({lo}) < floatBitsToUint({pl});',
                 f'    int {ci} = int({c});',
                 f'    int {h} = floatBitsToInt({ph}) + {oh};',
                 f'    int {hh} = {h} + {ci};',
                 f'    int {r} = {(v4 if kind else w)["name"]}({lo}, {hh});',
                 f'    uint {o} = {tid} * {PF_K}u + {k}u;',
                 f'    t_out._m0[int({o})] = uint({r}) * 3u + {vec}[0] * 5u + {vec}[1] * 7u + {vec}[2] * 11u;']
    return f'''#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = {THREADS}, local_size_y = 1, local_size_z = 1) in;

layout(buffer_reference) buffer uintPointer;
layout(buffer_reference) buffer uvec4Pointer;

layout(buffer_reference, buffer_reference_align = 4) buffer uintPointer
{{
    uint value;
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
    uint _m0[];
}} t_in;

layout(set = 0, binding = 7, std430) writeonly buffer t_out_t
{{
    uint _m0[];
}} t_out;

uint {w['last']} = 0u;
uint {v4['last']} = 0u;
uint {vec}[3];

{v4['src']}{w['src']}void main()
{{
''' + '\n'.join(body) + '\n}\n\n'


def pf_threads(ps, groups, rnd, lanes):
    """[(pointers [(lo, hi)] * PF_NP, addresses [PF_K])] of every thread. A workgroup takes one way of choosing its pointers:
    0-2 one set for the workgroup (all lanes agree: what the game does), 3 every lane its own, 4 one set but lane 0 (the
    first active lane sg mode broadcasts) has its own, 5 one set but the lanes of every other 'lanes' group their own,
    6 pointers in no pool / 0 / the top of the address space, 7 pointers at the end of a pool (offsets run into the next pool
    or a gap), 8 the pools past the hint tag (2^41) and above 4 GiB. Addresses: mostly inside the pointer's pool (16-byte
    steps), else its edges, another pool, or no pool."""
    small, big = ps[:2500], ps[2500:]

    def in_pool(p):
        s, e, _ = p
        return s + rnd.randrange(0, max(1, (e - s - 16) // 16)) * 16

    def pick():
        r = rnd.random()
        return rnd.choice(small) if r < 0.4 else rnd.choice(big)

    def pointer_set(way):
        out = []
        for _ in range(PF_NP):
            if way == 6:
                c = rnd.random()
                out.append((far(rnd) if c < 0.5 else (0 if c < 0.75 else M64 - 15), None))
            elif way == 7:
                p = rnd.choice([ps[2500], small[rnd.randrange(2499)], big[rnd.randrange(len(big))]])
                out.append(((p[1] - 16 * rnd.randrange(1, 4)) & M64, p))
            elif way == 8:
                p = rnd.choice([ps[2505], ps[2503], ps[2504], ps[2502]])
                out.append((in_pool(p), p))
            else:
                p = pick()
                out.append((in_pool(p), p))
        return out

    def addresses(pset):
        out = []
        for j, kind in PF_SITES:
            P, p = pset[j]
            r = rnd.random()
            if p is not None and r < 0.7:
                a = in_pool(p)
            elif p is not None and r < 0.8:
                s, e, _ = p
                a = rnd.choice([s - 4, s - 16, s, e - 16, e - 4, e, e + 4, e + 16]) & M64
            elif r < 0.9:
                a = in_pool(rnd.choice(ps))
            else:
                a = far(rnd)
            out.append(a & M64)
        return out

    threads = []
    for g in range(groups):
        way = g % 9
        shared = pointer_set(way)
        for lane in range(THREADS):
            if way == 3 or (way == 4 and lane == 0) or (way == 5 and (lane // lanes) % 2):
                pset = pointer_set(rnd.choice((0, 7, 8)))
            else:
                pset = shared
            threads.append((pset, addresses(pset)))
    return threads


def prefetch_test(tmp, runner, name, kernel16, groups, seed, fine_table, variants=('prod', 'f1', 'pf1', 'pfl1', 'pw1', 'pf4')):
    d = tmp / name; d.mkdir()
    real = cross(sorted(S.glob(kernel16 + '*-*.spv'))[0])
    glsl = pf_kernel(real)
    (d / 'kernel.comp').write_text(glsl)
    mods, bad = {}, 0
    for v in variants:
        for cnt in (False, True):
            tag = f'{v}{"_cnt" if cnt else ""}'
            g = rewrite(glsl, v)
            if v.startswith(('pf', 'pw')) and not cnt:
                p = gmemfast.prefetch_last()
                if not p or len(p['take']) < 8 or p['covered'] != PF_K:
                    print(f'  {name}/{tag}: the prefetch did not take the kernel\'s pointers: {p and (len(p["take"]), p["covered"])}')
                    bad += 1
                elif v == 'pf1':
                    print(f'  {name}: plan: groups {[(gg["at"], sorted(gg["keys"].values())) for gg in p["groups"]]}, '
                          f'{len(p["take"])} run starts take an entry, {p["covered"]} of {p["sites"]} sites')
            if cnt:
                g = instrument(g, CNT_DECL)
            (d / f'{tag}.comp').write_text(g)
            e = mw.compile_module(d / f'{tag}.comp', d / f'{tag}.spv', 'comp', 'vulkan1.2')
            if e:
                sys.exit(f'{name}/{tag}: {e}')
            mods[tag] = d / f'{tag}.spv'
    rnd = random.Random(seed)
    ps, pool_bytes = pools()
    threads = pf_threads(ps, groups, rnd, lanes=2)
    inw = []
    for pset, addrs in threads:
        for P, _ in pset:
            inw += [P & 0xffffffff, P >> 32]
        for (j, kind), a in zip(PF_SITES, addrs):
            off = (a - pset[j][0]) & M64
            inw += [off & 0xffffffff, off >> 32]
    (d / 'in.bin').write_bytes(struct.pack('<%dI' % len(inw), *inw))
    (d / 'spec.bin').write_bytes(struct.pack('<%dQ' % (1 + 3 * len(ps)), pool_bytes, *[x for p in ps for x in p]))
    want, hits = [], 0
    for pset, addrs in threads:
        o, h = model(ps, pool_bytes // 4, [(a, kind) for a, (j, kind) in zip(addrs, PF_SITES)])
        want += o; hits += h
    outs, counts = {}, {}
    for tag, path in mods.items():
        od = d / ('run_' + tag); od.mkdir()
        args = [str(runner), str(path), str(groups), '0', f'T:5:{d / "spec.bin"}', f'6:s:{len(inw) * 4}:{d / "in.bin"}', f'7:s:{len(want) * 4}']
        if tag.endswith('_cnt'):
            args.append('8:s:64')
        r = run(args, cwd=od, env=dict(os.environ, GMEM_FINE=fine_table))
        if r.returncode:
            bad += 1; outs[tag] = None
            print(f'  {name}/{tag}: the run DIED ({(r.stderr.strip() or str(r.returncode))[:120]})')
            continue
        got = struct.unpack('<%dI' % len(want), (od / 'out_7.bin').read_bytes())
        outs[tag] = got
        wrong = sum(1 for x, y in zip(got, want) if x != y)
        if wrong:
            j = next(i for i in range(len(want)) if got[i] != want[i])
            t, k = divmod(j, PF_K)
            print(f'  {name}/{tag}: {wrong} words differ from the model; first: thread {t} (way {(t // THREADS) % 9}) site {k} '
                  f'address {threads[t][1][k]:#x}: {got[j]:#x}, want {want[j]:#x}')
        bad += wrong
        if tag.endswith('_cnt'):
            counts[tag[:-4]] = struct.unpack('<16I', (od / 'out_8.bin').read_bytes())
    for tag in outs:
        if tag != 'prod' and outs[tag] is not None and outs.get('prod') is not None and outs[tag] != outs['prod']:
            bad += 1; print(f'  {name}: {tag} != prod')
    ref = counts.get('prod')
    lines = []
    for v, c in counts.items():
        if ref and (c[0], c[2], c[8], c[10]) != (ref[0], ref[2], ref[8], ref[10]):
            bad += 1; lines.append(f'{v}: calls / none differ from prod')
        lines.append(f'{v}: calls {c[0]} / {c[8]}, in no pool {c[2]} / {c[10]}, searches {c[1]} / {c[9]}, prefetched entries taken {c[14]}, '
                     f'search steps {c[15]}')
    f1, pf = counts.get('f1'), counts.get('pf1')
    if f1 and pf and not (pf[1] + pf[9] < f1[1] + f1[9] and pf[14] > 0):
        bad += 1; lines.append('pf1 does not search less than f1')
    print(f'{name} (helpers of {kernel16}): {groups * THREADS} threads x {PF_K} sites = {len(want)}, {hits} in a pool ({len(ps)} pools); '
          f'table GMEM_FINE={fine_table}; modules {", ".join(variants)} and their instrumented twins: {bad} wrong')
    for l in lines:
        print('    ' + l)
    return bad


def pf_mutations():
    """(name, text of gmemfast.prefetch_apply replaced, replacement): each breaks 'an exact copy of one table entry' and
    must make 'prefetch' fail. (A wrong hint test, an entry other than the pointer's, or an install whose range check is
    off by one are NOT mistakes: any exact table entry that holds the address gives the same load, and the helper checks
    the range itself; those keep the results by design.)"""
    return [
        ("the delta is not taken with the range", "            f'{sind}    {g}_d = dmsh_pf{i}_d;',\n", ''),
        ('the entry read two words off', "pf += [f'{ind}uint dmsh_pf{i}_w = dmsh_pf{i}_x * 8u;',",
         "pf += [f'{ind}uint dmsh_pf{i}_w = dmsh_pf{i}_x * 8u + 2u;',"),
        ("the delta of another pointer's entry", "f'{sind}    {g}_d = dmsh_pf{i}_d;',", "f'{sind}    {g}_d = dmsh_pf{max(i - 1, 0)}_d;',"),
    ]


def pf_mutate(tmp, runner):
    import inspect
    noticed, muts = 0, pf_mutations()
    src = inspect.getsource(gmemfast.prefetch_apply)
    saved = gmemfast.prefetch_apply
    for i, (what, old, new) in enumerate(muts):
        assert src.count(old) == 1, what
        ns = vars(gmemfast)
        exec(src.replace(old, new), ns)   # rebinds gmemfast.prefetch_apply (rewrite_raw looks it up at call time)
        try:
            bad = prefetch_test(tmp, runner, f'pfmut{i}', '73dc0b90c4082e43', 18, 5122 + i, '1', variants=('prod', 'pf1', 'pfl1'))
        finally:
            gmemfast.prefetch_apply = saved
        noticed += bad > 0
        print(f'mutation "{what}": {bad} wrong -> {"noticed" if bad else "NOT NOTICED"}')
    print(f'pfmutate: {noticed} of {len(muts)} mutations noticed:', 'PASS' if noticed == len(muts) else 'FAIL')
    return 0 if noticed == len(muts) else 1


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'synthetic'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        runner = tmp / 'gmem_run'
        r = run(['gcc', '-O1', '-o', str(runner), str(HERE / 'gmem_run.c'), '-lvulkan'])
        if r.returncode:
            sys.exit(r.stderr)
        if what == 'synthetic':
            bad = synthetic(tmp, runner, 'gmem-ways', '73dc0b90c4082e43', 32, 1109)
            bad += synthetic(tmp, runner, 'gmem-ways-3439', '3439aaa071d868ae', 24, 2109)
            print('synthetic:', 'PASS' if not bad else 'FAIL')
        elif what == 'fine':   # oot-a111: the fine hints in the table; the translator's helpers and the a107 form must not notice
            bad = synthetic(tmp, runner, 'gmem-fine', '73dc0b90c4082e43', 32, 1111, variants=('prod', 1, 'f1', 'f2', 'f4'), fine_table='1')
            bad += synthetic(tmp, runner, 'gmem-fine-3439', '3439aaa071d868ae', 24, 2111, variants=('prod', 1, 'f1', 'f4'), fine_table='1')
            bad += synthetic(tmp, runner, 'gmem-fine-off', '73dc0b90c4082e43', 16, 4111, variants=('prod', 'f1', 'f4'), fine_table='0')
            bad += synthetic(tmp, runner, 'gmem-fine-miss', '73dc0b90c4082e43', 16, 3111, miss=True, variants=('prod', 1, 'f1', 'f4'), fine_table='1')
            print('fine:', 'PASS' if not bad else 'FAIL')
        elif what == 'miss':
            bad = synthetic(tmp, runner, 'gmem-ways-miss', '73dc0b90c4082e43', 16, 3109, miss=True)
            print('miss:', 'PASS' if not bad else 'FAIL')
        elif what == 'mutate':
            bad = mutate(tmp, runner)
        elif what == 'whole':
            bad = whole(tmp, runner, sys.argv[2:])
        elif what == 'prefetch':   # oot-a122C: the prefetch on a kernel of the game kernels' shape, both tables
            bad = prefetch_test(tmp, runner, 'pf-fine', '73dc0b90c4082e43', 36, 1122, '1')
            bad += prefetch_test(tmp, runner, 'pf-nofine', '73dc0b90c4082e43', 27, 2122, '0')
            bad += prefetch_test(tmp, runner, 'pf-3439', '3439aaa071d868ae', 27, 3122, '1')
            print('prefetch:', 'PASS' if not bad else 'FAIL')
        elif what == 'pfmutate':
            bad = pf_mutate(tmp, runner)
        else:
            sys.exit('synthetic | fine | miss | mutate | whole [k16 ...] | prefetch | pfmutate')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
