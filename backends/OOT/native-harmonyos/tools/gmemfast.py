"""GLSL post-passes on a kernel's SPIRV-Cross output (exp-gmem experiment; results must not change).

rewrite_raw(glsl) -> (glsl', helpers rewritten, call sites)
  The translator's raw global memory helpers (GlobalToStorage.TryGenerateRawStorageOp: a load through an address the
  shader loaded) keep the pool table entry they chose last in a private word and read that entry's range from the table
  on every call (4 loads), then its delta (2 loads); a miss runs the hint and an unrolled 13-step search (1 + 26 loads,
  then 2), and spirv-opt -O inlines all of it at every call site (35 table loads a call site).
  Here a helper keeps the six words of its last entry {start, end, delta} in private words instead of the entry's number
  (zero at the start, as entry 0 is), so a hit reads the table not at all, and the miss path is ONE function,
  dmsh_gmem_find (the hint, else the same search as a loop), which only reads the table and returns the entry.
  Invariant (the proof): after every call the six private words equal table[last * 8 .. last * 8 + 5] of the old
  helper's 'last', because both are set from the same entry at the same places and the module does not write the table
  (binding kind 16, reflection rw 0; checked below). So every branch of the old helper is taken alike and the same
  address is loaded.
  Only helpers whose text, with its ids renumbered, is exactly a known form (FORMS, learned from a reference kernel the
  differential test gmem_eq.py runs) are rewritten.
  exp-gmem-verify: the pointer load sits in 'if (ok) { ... }' as in the translator's helper (BODY). The first draft
  (BODY_EARLY: 'if (!inside) return 0;' inside the miss branch, the load after it) is the same program on paper, but
  Mesa's lavapipe then performed the load for lookups that found no pool: with an address far from every pool the
  process died (SIGSEGV) where the translator's helper returns 0 (v_debug.py, v_kernel_eq_early.log, the self-test's
  gmem-miss case). The old shape keeps what a driver sees around that load as it was.

rewrite_raw(glsl, ways=W): up to W entries a helper (oot-a109; W from DMSH_GMEM_WAYS when not given)
  DMSH_GMEM_WAYS = 1: the one-entry form above, text for text (the a107 / a108 modules). 2..4: every helper keeps that many
  entries. auto (the default): each helper gets the fewest entries, at most 4, with which its call sites, in program
  order, miss no more often than with 4 (site_bases(): the pointer an address is made of, the memory word it was loaded
  from; access_pattern() reports it). The game's 109 modules: 190 helpers, 141 keep one entry (among them every helper of
  3439aaa0, 9df3c6c0, fed302d7, 604134a5, eb021183, whose sites come in one run per pointer: their misses are the first
  touch of each pointer whatever the size), 9 take 2, 30 take 3, 10 take 4 (the 39 modules whose sites go back to a
  pointer they left, e.g. 17c8ab64's 128-bit helper 8 -> 4 misses a thread).
  The entries are kept most recently used first; a lookup checks entry 0 (the hit path of the one-entry form, no table
  read), then entries 1.. (a hit there moves that entry to the front), else it is the translator's miss path: the least
  recently used entry is dropped, dmsh_gmem_find, the six words, the 'inside' check, the guarded pointer load.
  Invariant: entry 0 equals table[last * 8 .. last * 8 + 5] of the old helper's 'last' after every call, and every entry
  is an exact copy of a table entry (or all zero = entry 0, which holds nothing). The proof needs one fact more than the
  one-entry form: when an entry k > 0 holds the address, dmsh_gmem_find(address) would return that same entry. That is
  so for the table the replay builds (overlay nvn_vk.c gmem_rebuild; exp-gmem's gmem_run.c builds it alike): ranges
  disjoint and ascending, unused entries ~0, a hint only for a 2 MiB chunk its entry holds whole and tagged with the
  chunk. So the search ends on the one entry that holds the address, the hint names only an entry that holds it, and
  both helpers take the same branch and load the same address. With a table that breaks that (overlapping ranges, a hint
  to an entry that does not hold its chunk) only the one-entry form still equals the translator.
  Verified on lavapipe against the translator's helper (gmem_ways_test.py: synthetic lookups including two to five
  pools by turns and addresses in no pool, and the hot kernels whole; gmem_ways_adv.py, the independent check: adjacent
  entries, high words that change, the hint path, a lookup in no pool after a hit, a 64-bit (V2) helper, and the counts
  of searches and of hits per entry equal to a per-thread LRU model exactly).
  Cost: every call site inlines the extra checks and moves of its miss branch: SPIR-V +35 % with 3 entries, +54..73 % with
  4 (wrapped-a109: the 39 modules 16.7 -> 24.1 MB, all modules 99.3 -> 106.7 MB), and 6 words a helper per extra entry
  live wherever the helper is still to be called. The hit path is the one-entry form's.

rewrite_umulx(glsl) -> (glsl', n)
  umulExtended(x, 2^n, hi, lo) -> hi = x >> (32 - n), lo = x << n   (the 64-bit address arithmetic index * 16).

rewrite_raw(glsl, prefetch=P): the dispatch's pointers translated together (oot-a122C; P from DMSH_GMEM_PREFETCH when not
  given, default '0' = off: the text is then exactly the a111 form). 'sg': the pointer words a kernel's call sites add their
  offsets to (single definitions in main's own block, loaded from a buffer: the vertex-stream table at cp_c0 word 120 / 121)
  are taken from the subgroup's first lane, their hint words read together, then their entries; at the first call site of
  each run (a helper's sites in program order with one pointer) the prefetched entry goes into the helper's own entry when
  it holds the call's address. 'lane': the same with each thread's own words. prefetch_apply has the why and the proof;
  prefetch_last() the plan of this thread's last call. Verified: gmem_ways_test.py prefetch / pfmutate / whole.
"""
import os
import re
import threading

FN = re.compile(r'^(\w+) (\w+)\(([^)]*)\)\n\{\n', re.M)


def functions(text):
    out = []
    for m in FN.finditer(text):
        end = text.index('\n}\n', m.end() - 1) + 3
        out.append((m.group(2), m.start(), end))
    return out


def helper_info(text, start, end):
    """None, or the facts of a raw load helper: name, arguments, table block, last-entry word, vector memory, kind"""
    src = text[start:end]
    head = FN.match(src)
    if not head or head.group(1) != 'int':
        return None
    args = re.fullmatch(r'int (_\d+), int (_\d+)', head.group(3))
    if not args or 'Pointer(uvec2(' not in src:
        return None
    tables = set(re.findall(r'\b(cp_s\d+_1)\._m0\[', src))
    if len(tables) != 1:
        return None
    last = re.search(r'float _\d+ = uintBitsToFloat\((_\d+)\);', src)
    if not last or not re.search(r'^uint ' + last.group(1) + r' = 0u;$', text, re.M):
        return None
    if 'uvec4Pointer(' in src:
        kind = 'V4'
    elif 'uvec2Pointer(' in src:
        kind = 'V2'
    elif '= uintPointer(uvec2(' in src:
        kind = 'W'
    else:
        return None
    vec = re.search(r'^\s*(_\d+)\[0\] = _\d+\.y;$', src, re.M)
    return dict(name=head.group(2), a=args.group(1), b=args.group(2), table=tables.pop(), last=last.group(1),
                vec=vec.group(1) if vec else None, kind=kind, src=src)


def canonical(h):
    """the helper's text with the names it depends on replaced, then its ids renumbered: equal for equal forms"""
    s = h['src'].replace(h['table'], 'TABLE')
    s = re.sub(r'\b' + h['last'] + r'\b', 'LAST', s)
    if h['vec']:
        s = re.sub(r'\b' + h['vec'] + r'\b', 'VEC', s)
    s = re.sub(r'\b' + h['name'] + r'\(', 'HELPER(', s)
    ids = {}
    return re.sub(r'\b_\d+\b', lambda m: ids.setdefault(m.group(0), f'_i{len(ids)}'), s)


FORMS = {}   # kind -> canonical text of the old helper
# sha256 of the canonical text of the two forms this translator build emits (learned from 73dc0b90c4082e43, the kernel
# gmem_eq.py takes its helpers from; all 10,894 call sites of the 109 kernels with the pool table are of these forms)
KNOWN = {'V4': '22ab9fafaacaadf75305b0ef613af3345cef41b7ab350031fb3c8fe98dd54fad',
         'W': 'ad0eaec3566045a689966acd2eb4dd24be08f693de76a45735cdfd1dca80b98a'}


def known(h):
    import hashlib
    c = canonical(h)
    return FORMS.get(h['kind']) == c or KNOWN.get(h['kind']) == hashlib.sha256(c.encode()).hexdigest()


def learn(glsl):
    for name, start, end in functions(glsl):
        h = helper_info(glsl, start, end)
        if h:
            FORMS.setdefault(h['kind'], canonical(h))


FIND = '''uint dmsh_gmem_find(const uint lo, const uint hi)
{
    uint chunk = (hi << 11u) | (lo >> 21u);
    uint hint = @TABLE@._m0[(chunk & 8191u) * 8u + 6u];
    uint e = hint & 8191u;
    bool tag = (hint >> 13u) == chunk;
    bool low = hi < 256u;
    bool named = e != 0u;
    bool fits = tag && low;
    bool hinted = fits && named;
    if (hinted)
    {
        return e;
    }
    e = 0u;
    for (uint step = 4096u; step != 0u; step >>= 1u)
    {
        uint c = e + step;
        uint sh = @TABLE@._m0[c * 8u + 1u];
        uint sl = @TABLE@._m0[c * 8u];
        bool h_lt = sh < hi;
        bool h_eq = sh == hi;
        bool l_le = sl <= lo;
        bool eq_le = h_eq && l_le;
        bool at = h_lt || eq_le;
        e = at ? c : e;
    }
    return e;
}

'''

# oot-a111: the same lookup, with a bounded search for the chunks no entry holds whole. Word 7 of a slot (never read by
# the translator's helpers, which read words 0..6) holds, when the slot's tag (word 6) names this chunk with entry 0:
# first | (s + 1) << 16, where first = the last entry starting at or below the chunk's start and every entry starting
# inside the chunk is at most first + 2^s - 1 (overlay nvn_vk.c gmem_fine). For an address in the chunk the entry the full
# search returns (the last one starting at or below it) is then in first .. first + 2^s - 1 and starts at or above first,
# so the search from 'first' with steps 2^(s-1) .. 1 ends on the same entry (entries past it start above the address:
# ascending, unused ones ~0). Word 7 = 0 (an older replay, TOTK_OHOS_GMEM_FINE=0) is the full search of FIND.
FIND_FINE = '''uint dmsh_gmem_find(const uint lo, const uint hi)
{
    uint chunk = (hi << 11u) | (lo >> 21u);
    uint slot = (chunk & 8191u) * 8u;
    uint hint = @TABLE@._m0[slot + 6u];
    uint fine = @TABLE@._m0[slot + 7u];
    uint e = hint & 8191u;
    bool tag = (hint >> 13u) == chunk;
    bool low = hi < 256u;
    bool named = e != 0u;
    bool fits = tag && low;
    bool hinted = fits && named;
    if (hinted)
    {
        return e;
    }
    uint wide = fine >> 16u;
    bool narrow = wide != 0u;
    bool bounded = fits && narrow;
    e = bounded ? (fine & 8191u) : 0u;
    uint top = bounded ? ((1u << wide) >> 2u) : 4096u;
    for (uint step = top; step != 0u; step >>= 1u)
    {
        uint c = e + step;
        uint sh = @TABLE@._m0[c * 8u + 1u];
        uint sl = @TABLE@._m0[c * 8u];
        bool h_lt = sh < hi;
        bool h_eq = sh == hi;
        bool l_le = sl <= lo;
        bool eq_le = h_eq && l_le;
        bool at = h_lt || eq_le;
        e = at ? c : e;
    }
    return e;
}

'''


def fine_setting(value=None):
    """True: dmsh_gmem_find reads word 7 (FIND_FINE, oot-a111; DMSH_GMEM_FINE=1, the default); False: FIND (=0)"""
    v = str(os.environ.get('DMSH_GMEM_FINE', '1') if value is None else value).strip()
    if v not in ('0', '1'):
        raise ValueError(f'DMSH_GMEM_FINE={v!r}: 0 or 1')
    return v == '1'


BODY = '''int @NAME@(int @A@, int @B@)
{
    uint lo = uint(@A@);
    uint hi = uint(@B@);
    bool s_lt = @LAST@_s.y < hi;
    bool s_eq = @LAST@_s.y == hi;
    bool s_le = @LAST@_s.x <= lo;
    bool s_eq_le = s_eq && s_le;
    bool above = s_lt || s_eq_le;
    bool e_gt = hi < @LAST@_e.y;
    bool e_eq = hi == @LAST@_e.y;
    bool e_lt = lo < @LAST@_e.x;
    bool e_eq_lt = e_eq && e_lt;
    bool below = e_gt || e_eq_lt;
    bool held = above && below;
    bool ok = true;
    if (!held)
    {
        uint e = dmsh_gmem_find(lo, hi) * 8u;
        @LAST@_s = uvec2(@TABLE@._m0[e], @TABLE@._m0[e + 1u]);
        @LAST@_e = uvec2(@TABLE@._m0[e + 2u], @TABLE@._m0[e + 3u]);
        @LAST@_d = uvec2(@TABLE@._m0[e + 4u], @TABLE@._m0[e + 5u]);
        bool n_gt = hi < @LAST@_e.y;
        bool n_eq = hi == @LAST@_e.y;
        bool n_lt = lo < @LAST@_e.x;
        bool n_eq_lt = n_eq && n_lt;
        bool inside = n_gt || n_eq_lt;
        ok = inside;
    }
    if (ok)
    {
        uint hl = lo + @LAST@_d.x;
        uint hh = hi + @LAST@_d.y + uint(hl < lo);
@LOAD@    }
    return 0;
}

'''
# the first draft (dies on lavapipe when a lookup finds no pool): kept for v_debug.py / v_early.py only
BODY_EARLY = '''int @NAME@(int @A@, int @B@)
{
    uint lo = uint(@A@);
    uint hi = uint(@B@);
    bool s_lt = @LAST@_s.y < hi;
    bool s_eq = @LAST@_s.y == hi;
    bool s_le = @LAST@_s.x <= lo;
    bool s_eq_le = s_eq && s_le;
    bool above = s_lt || s_eq_le;
    bool e_gt = hi < @LAST@_e.y;
    bool e_eq = hi == @LAST@_e.y;
    bool e_lt = lo < @LAST@_e.x;
    bool e_eq_lt = e_eq && e_lt;
    bool below = e_gt || e_eq_lt;
    bool held = above && below;
    if (!held)
    {
        uint e = dmsh_gmem_find(lo, hi) * 8u;
        @LAST@_s = uvec2(@TABLE@._m0[e], @TABLE@._m0[e + 1u]);
        @LAST@_e = uvec2(@TABLE@._m0[e + 2u], @TABLE@._m0[e + 3u]);
        @LAST@_d = uvec2(@TABLE@._m0[e + 4u], @TABLE@._m0[e + 5u]);
        bool n_gt = hi < @LAST@_e.y;
        bool n_eq = hi == @LAST@_e.y;
        bool n_lt = lo < @LAST@_e.x;
        bool n_eq_lt = n_eq && n_lt;
        bool inside = n_gt || n_eq_lt;
        if (!inside)
        {
            return 0;
        }
    }
    uint hl = lo + @LAST@_d.x;
    uint hh = hi + @LAST@_d.y + uint(hl < lo);
@LOAD@}

'''
LOADS = {
    'W': '    return int(uintPointer(uvec2(hl, hh)).value);\n',
    'V2': '    uvec2 v = uvec2Pointer(uvec2(hl, hh)).value;\n    @VEC@[0] = v.y;\n    return int(v.x);\n',
    'V4': '    uvec4 v = uvec4Pointer(uvec2(hl, hh)).value;\n    @VEC@[0] = v.y;\n    @VEC@[1] = v.z;\n    @VEC@[2] = v.w;\n    return int(v.x);\n',
}

MAX_WAYS = 4
# an entry {s, e} holds the address (hi:lo): s <= address < e, the comparisons of BODY (spirv-opt inlines it)
IN = '''bool dmsh_gmem_in(uvec2 s, uvec2 e, uint lo, uint hi)
{
    bool s_lt = s.y < hi;
    bool s_eq = s.y == hi;
    bool s_le = s.x <= lo;
    bool s_eq_le = s_eq && s_le;
    bool above = s_lt || s_eq_le;
    bool e_gt = hi < e.y;
    bool e_eq = hi == e.y;
    bool e_lt = lo < e.x;
    bool e_eq_lt = e_eq && e_lt;
    bool below = e_gt || e_eq_lt;
    return above && below;
}

'''


def ways_setting(value=None):
    """(entries, sized per helper) of a DMSH_GMEM_WAYS value: '1'..'4' that many for every helper, 'auto' at most 4"""
    # default 1 (oot-a109): the hot kernels' misses are each pointer's first use, which no cache size removes; 'auto'
    # changes 39 rarely used modules (+7.5 MB of SPIR-V) for 3-10 fewer searches a thread, unmeasured on the phone
    v = str(os.environ.get('DMSH_GMEM_WAYS', '1') if value is None else value).strip().lower()
    if v == 'auto':
        return MAX_WAYS, True
    if v not in [str(n) for n in range(1, MAX_WAYS + 1)]:
        raise ValueError(f'DMSH_GMEM_WAYS={v!r}: 1..{MAX_WAYS} or auto')
    return int(v), False


def body_ways(w):
    """the helper's text with w entries: BODY itself for one; else entry 0 is BODY's (@LAST@_s/_e/_d), entry k @LAST@_ks..."""
    if w == 1:
        return BODY
    word = lambda k, c: f'@LAST@_{c}' if k == 0 else f'@LAST@_{k}{c}'
    out = ['int @NAME@(int @A@, int @B@)', '{', '    uint lo = uint(@A@);', '    uint hi = uint(@B@);',
           '    bool held = dmsh_gmem_in(@LAST@_s, @LAST@_e, lo, hi);', '    bool ok = true;', '    if (!held)', '    {']
    ind = '        '
    for k in range(1, w):
        out += [ind + f'if (dmsh_gmem_in({word(k, "s")}, {word(k, "e")}, lo, hi))', ind + '{']
        # entry k to the front, entries 0 .. k-1 one place down
        out += [ind + f'    uvec2 t{c} = {word(k, c)};' for c in 'sed']
        for j in range(k, 0, -1):
            out += [ind + f'    {word(j, c)} = {word(j - 1, c)};' for c in 'sed']
        out += [ind + f'    {word(0, c)} = t{c};' for c in 'sed']
        out += [ind + '}', ind + 'else', ind + '{']
        ind += '    '
    # no entry holds it: the last one is dropped, the rest one place down, then the translator's miss path into entry 0
    for j in range(w - 1, 0, -1):
        out += [ind + f'{word(j, c)} = {word(j - 1, c)};' for c in 'sed']
    out += [ind + 'uint e = dmsh_gmem_find(lo, hi) * 8u;',
            ind + '@LAST@_s = uvec2(@TABLE@._m0[e], @TABLE@._m0[e + 1u]);',
            ind + '@LAST@_e = uvec2(@TABLE@._m0[e + 2u], @TABLE@._m0[e + 3u]);',
            ind + '@LAST@_d = uvec2(@TABLE@._m0[e + 4u], @TABLE@._m0[e + 5u]);',
            ind + 'bool n_gt = hi < @LAST@_e.y;',
            ind + 'bool n_eq = hi == @LAST@_e.y;',
            ind + 'bool n_lt = lo < @LAST@_e.x;',
            ind + 'bool n_eq_lt = n_eq && n_lt;',
            ind + 'bool inside = n_gt || n_eq_lt;',
            ind + 'ok = inside;']
    for k in range(1, w):
        ind = ind[:-4]
        out.append(ind + '}')
    out += ['    }', '    if (ok)', '    {', '        uint hl = lo + @LAST@_d.x;', '        uint hh = hi + @LAST@_d.y + uint(hl < lo);']
    return '\n'.join(out) + '\n@LOAD@    }\n    return 0;\n}\n\n'


# ---- the access pattern: which pointer each call site's address is made of (sizes the entries; never the results)
_DEF = re.compile(r'^\s*(?:(?:int|uint|float|bool)\s+)?(_\d+) = (.*);$')
_CAST = re.compile(r'^(?:floatBitsToInt|floatBitsToUint|int|uint|uintBitsToFloat|intBitsToFloat)\((.*)\)$')
_LOADX = re.compile(r'^(\w+(?:\._m\d+)?)\[(.*)\](?:\.[xyzw])?$')
OFF = ('off',)   # an offset: a constant, a product, a shift, a mask (index * 16 and the like)


def _balanced(s):
    d = 0
    for ch in s:
        d += (ch in '([') - (ch in ')]')
        if d < 0:
            return False
    return d == 0


def _split(expr, op):
    d = 0
    for i, ch in enumerate(expr):
        d += (ch in '([') - (ch in ')]')
        if d == 0 and expr.startswith(op, i):
            return expr[:i].strip(), expr[i + len(op):].strip()
    return None


def site_bases(glsl, hs):
    """[(helper name, key)] of the helpers' call sites in program order. The key is what the address's low word is made
    of past offsets: ('load', the load's text with its temporaries written out) for a pointer read from memory,
    ('chase', temporary) for one read through a lookup, else ('site', temporary): a key of its own."""
    spans = [(h['start'], h['end']) for h in hs]
    names = {h['name'] for h in hs}
    defs, sites, at = {}, [], 0
    for line in glsl.split('\n'):
        pos, at = at, at + len(line) + 1
        if any(s <= pos < e for s, e in spans):
            continue
        m = _DEF.match(line)
        if not m:
            continue
        var, rhs = m.groups()
        c = re.fullmatch(r'(_\d+)\((_\d+), (_\d+)\)', rhs)
        if c and c.group(1) in names:
            sites.append((c.group(1), var, c.group(2)))
            rhs = 'CALL'
        defs.setdefault(var, []).append(rhs)

    def expand(expr, depth=0):
        def sub(m):
            d = defs.get(m.group(0))
            return '(' + expand(d[0], depth + 1) + ')' if d and len(d) == 1 and d[0] != 'CALL' and depth < 12 else m.group(0)
        return re.sub(r'\b_\d+\b', sub, expr) if len(expr) < 2000 else expr

    def base(expr, depth=0):
        expr = expr.strip()
        while True:
            m = _CAST.match(expr)
            if m and _balanced(m.group(1)):
                expr = m.group(1).strip()
            elif expr.startswith('(') and expr.endswith(')') and _balanced(expr[1:-1]):
                expr = expr[1:-1].strip()
            else:
                break
        if depth > 48:
            return None
        if re.fullmatch(r'-?\d+u?', expr):
            return OFF
        if re.fullmatch(r'_\d+', expr):
            d = defs.get(expr)
            if not d:
                return None
            if len(d) > 1:
                bs = {base(x, depth + 1) for x in d}
                return bs.pop() if len(bs) == 1 else None
            return ('chase', expr) if d[0] == 'CALL' else base(d[0], depth + 1)
        m = _LOADX.match(expr)
        if m and _balanced(m.group(2)):
            return ('load', expand(expr))
        for op in (' + ', ' - '):
            p = _split(expr, op)
            if p:
                a, b = base(p[0], depth + 1), base(p[1], depth + 1)
                if b == OFF:
                    return a
                return b if a == OFF and op == ' + ' else None
        if any(_split(expr, op) for op in (' * ', ' << ', ' >> ', ' & ', ' | ', ' ^ ')) or re.fullmatch(r'_\d+\._m[01]', expr) \
                or re.match(r'(?:bitfieldExtract|min|max|bitCount|findMSB|findLSB)\(', expr):
            return OFF
        return None

    out = []
    for name, var, lo in sites:
        b = base(lo)
        out.append((name, b if b not in (None, OFF) else ('site', var)))
    return out


def lru_misses(seq, n):
    """the misses of a cache of n entries, most recently used first, over a sequence of keys"""
    cache, miss = [], 0
    for x in seq:
        if x in cache:
            cache.remove(x)
        else:
            miss += 1
            del cache[n - 1:]
        cache.insert(0, x)
    return miss


def access_pattern(glsl, hs=None, cap=MAX_WAYS):
    """helper name -> dict(kind, sites, pointers, misses by entries 1..cap of a thread that runs every site, ways: the fewest
    entries with the fewest misses)"""
    if hs is None:
        hs = []
        for name, start, end in functions(glsl):
            h = helper_info(glsl, start, end)
            if h:
                h['start'], h['end'] = start, end
                hs.append(h)
    sb = site_bases(glsl, hs)
    out = {}
    for h in hs:
        seq = [k for n, k in sb if n == h['name']]
        m = [lru_misses(seq, n) for n in range(1, cap + 1)]
        out[h['name']] = dict(kind=h['kind'], sites=len(seq), pointers=len(set(seq)), misses=m,
                              ways=next(n for n in range(1, cap + 1) if m[n - 1] == m[-1]))
    return out


# ---- oot-a122C: the dispatch's pointers translated once, all at once, at the kernel's start (DMSH_GMEM_PREFETCH)
# The hot mesh kernels (3439aaa0, fed302d7, eb021183, ...) read every vertex stream through 5 - 6 pointers that the shader
# loads at its start from a storage buffer at a uniform place (cp_c0 word 120 / 121 + 0, 8, ..., 32). Each helper keeps one
# entry, so each thread misses once per (helper, pointer) run: 6 - 7 lookups a thread, each a chain of dependent reads
# (hint, [search], entry, data) that sits in the middle of the kernel and stalls it. Here the pointers are translated
# right after the shader loaded them, all together (the hint words of every pointer, then every entry: their latencies
# overlap), and at the first call site of each run the entry is put into the helper's own entry when it holds the
# address, so the run's first call hits.
# Why the results do not change: the prefetched entry is table[dmsh_gmem_find(p)] for some 64-bit p, an exact copy of a
# table entry, and it goes into the helper's entry only when it holds the call's address. For the table the replay builds
# (ranges disjoint and ascending, unused entries ~0, a hint only for a chunk its entry holds whole: the fact the 2..4-entry
# forms rest on, see rewrite_raw(ways)) the one entry that holds an address is the entry dmsh_gmem_find(address) returns,
# so the helper takes the hit path with the delta its miss path would have read, and loads the same address. That holds
# for ANY p: neither the pairing of the pointer's words nor the subgroup's agreement on them matters for the results, only
# for how often the run's first call hits. The module does not write the table (rewrite_raw checks), so a read of it at
# the start equals one later.
# 'sg' (the default when on): the pointer words are taken from the subgroup's first active lane (subgroupBroadcastFirst):
#   the prefetch is then subgroup-uniform by construction (a driver may keep it in uniform registers and do it once per
#   subgroup); for a lane whose pointer differs the entry just does not hold its addresses. Needs subgroup ballot (the
#   game's own kernels use subgroupBallot on the phone; Vulkan 1.1 modules).
# 'lane': each thread translates its own pointer words (no subgroup operation).
PF_MAX = 8   # pointers a module prefetches at most (6 words each stay live until their first call site)
_PF_ASSIGN = re.compile(r'(?<![\w.])(_\d+)\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)|(?:\+\+|--)\s*(_\d+)\b')
_PF_OUTFN = re.compile(r'^\w[\w ]*?\b(\w+)\(([^()]*\b(?:out|inout)\b[^()]*)\)\s*$', re.M)
_PF_DEF = re.compile(r'^(\s*)(int|uint|float|bool)\s+(_\d+)\s*=\s*(.*);$')
_PF_LOAD = re.compile(r'^\w+\._m\d+\[[^\[\]]*\](?:\.[xyzw])?$')


def prefetch_setting(value=None):
    """'0' off (the default), 'sg' the subgroup-uniform prefetch, 'lane' per thread (DMSH_GMEM_PREFETCH; '1' = 'sg')"""
    v = str(os.environ.get('DMSH_GMEM_PREFETCH', '0') if value is None else value).strip().lower()
    v = {'1': 'sg', 'on': 'sg'}.get(v, v)
    if v not in ('0', 'sg', 'lane'):
        raise ValueError(f'DMSH_GMEM_PREFETCH={v!r}: 0, sg (1) or lane')
    return v


def _pf_single_defs(glsl):
    """temporary -> (line, type, expression) of the temporaries given a value exactly once (mesh_wrap_in_container.py
    ssa_defs: an out / inout argument and a compound assignment count as a second value)"""
    count = {}
    for a, b in _PF_ASSIGN.findall(glsl):
        count[a or b] = count.get(a or b, 0) + 1
    twice = {n for n, c in count.items() if c > 1}
    outs = {}
    for m in _PF_OUTFN.finditer(glsl):
        params = m.group(2).split(',')
        outs[m.group(1)] = [len(params) - k for k, p in enumerate(params) if re.match(r'\s*(?:out|inout)\b', p)]
    outs.update({'umulExtended': [1, 2], 'imulExtended': [1, 2], 'uaddCarry': [1], 'usubBorrow': [1], 'frexp': [1], 'modf': [1]})
    for fn, places in outs.items():
        for m in re.finditer(r'\b' + re.escape(fn) + r'\((.*)$', glsl, re.M):
            if ';' not in m.group(1):
                continue
            args = [a.strip() for a in m.group(1).rsplit(')', 1)[0].split(',')]
            last = [args[-k] for k in places if k <= len(args)]
            if all(re.fullmatch(r'[\w.\[\]]+', a) for a in last):
                twice.update(a for a in last if re.fullmatch(r'_\d+', a))
            else:
                twice.update(re.findall(r'(?<![\w.])_\d+\b', m.group(1)))
    defs = {}
    for n, line in enumerate(glsl.split('\n')):
        m = _PF_DEF.match(line)
        if m:
            if m.group(3) in defs:
                twice.add(m.group(3))
            defs[m.group(3)] = (n, m.group(2), m.group(4))
    return {k: v for k, v in defs.items() if k not in twice}


def _pf_pointer(defs, expr, depth=0):
    """the temporary a pointer word of an address comes from: the address minus offsets (constants, products, shifts, masks,
    the carry of a 64-bit add, the high word of a multiply) is a temporary loaded from a buffer. OFF for an offset, None
    when it is neither (a pointer read through a lookup, a sum of two pointers, a value assigned twice)"""
    expr = expr.strip()
    while True:
        m = _CAST.match(expr)
        if m and _balanced(m.group(1)):
            expr = m.group(1).strip()
        elif expr.startswith('(') and expr.endswith(')') and _balanced(expr[1:-1]):
            expr = expr[1:-1].strip()
        else:
            break
    if depth > 48:
        return None
    if re.fullmatch(r'-?\d+u?', expr):
        return OFF
    if re.fullmatch(r'_\d+', expr):
        d = defs.get(expr)
        if d is None:
            return None
        rhs = d[2].strip()
        inner = rhs
        while True:
            m = _CAST.match(inner)
            if m and _balanced(m.group(1)):
                inner = m.group(1).strip()
            else:
                break
        if _PF_LOAD.match(inner) and _balanced(inner):
            return ('ptr', expr)
        if re.fullmatch(r'_\d+\(.*\)', rhs):   # a call (a lookup's result: a chased pointer)
            return None
        return _pf_pointer(defs, rhs, depth + 1)
    for op in (' + ', ' - '):
        p = _split(expr, op)
        if p:
            a, b = _pf_pointer(defs, p[0], depth + 1), _pf_pointer(defs, p[1], depth + 1)
            if b == OFF:
                return a
            return b if a == OFF and op == ' + ' else None
    if any(_split(expr, op) for op in (' * ', ' << ', ' >> ', ' & ', ' | ', ' ^ ', ' < ', ' <= ', ' > ', ' >= ', ' == ', ' != ')) \
            or re.fullmatch(r'_\d+\._m[01]', expr) or re.match(r'(?:bitfieldExtract|min|max|bitCount|findMSB|findLSB)\(', expr):
        return OFF
    return None


def _pf_word(t, ty):
    return {'float': f'floatBitsToUint({t})', 'int': f'uint({t})', 'uint': t}[ty]


def prefetch_plan(glsl, hs, cap=PF_MAX):
    """None, or what the prefetch does in this module: main's lines, the pointers {(lo temp, hi temp): number}, the line
    after which the prefetch goes, and the call sites that take the prefetched entry [(line, helper, pointer number, lo, hi)]"""
    mains = [(s, e) for name, s, e in functions(glsl) if name == 'main']
    if len(mains) != 1:
        return None
    m0, m1 = mains[0]
    lines = glsl.split('\n')
    first_line = glsl.count('\n', 0, m0)
    last_line = glsl.count('\n', 0, m1)
    # depth of each line of main: 1 = main's own block
    depth, d = {}, 0
    for n in range(first_line, last_line):
        depth[n] = d
        d += lines[n].count('{') - lines[n].count('}')
    defs = _pf_single_defs(glsl)
    names = {h['name'] for h in hs}
    call = re.compile(r'^(\s*)int (_\d+) = (' + '|'.join(re.escape(x) for x in names) + r')\((_\d+), (_\d+)\);$')
    sites = []
    for n in range(first_line, last_line):
        m = call.match(lines[n])
        if not m:
            continue
        lo, hi = _pf_pointer(defs, m.group(4)), _pf_pointer(defs, m.group(5))
        key = (lo[1], hi[1]) if isinstance(lo, tuple) and lo != OFF and isinstance(hi, tuple) and hi != OFF else None
        sites.append((n, m.group(3), key, m.group(4), m.group(5)))
    # a pointer's words: single definitions in main's own block (they then hold one value from there to main's end)
    ok = lambda t: t in defs and first_line < defs[t][0] < last_line and depth.get(defs[t][0]) == 1
    keys = []
    for n, h, key, _, _ in sites:
        if key and key not in keys and ok(key[0]) and ok(key[1]):
            keys.append(key)
    keys = keys[:cap]
    if not keys:
        return None
    # Groups: a pointer can be translated anywhere after its words' definitions and before its first site, in main's own
    # block; the pointers are taken in the order of their first sites and a group grows while its last definition still
    # comes before its first site (the fewest places: the translations of a group overlap). A group goes after the line of
    # its last definition.
    born = {k: max(defs[t][0] for t in k) for k in keys}
    first = {k: min(n for n, h, kk, _, _ in sites if kk == k) for k in keys}
    groups = []
    for k in sorted(keys, key=lambda k: (first[k], born[k])):
        if born[k] >= first[k]:
            continue   # (cannot happen: a site reads the words after their definition)
        g = groups[-1] if groups else None
        if g and max(g['at'], born[k]) < g['first']:
            g['keys'].append(k); g['at'] = max(g['at'], born[k])
        else:
            groups.append(dict(keys=[k], at=born[k], first=first[k]))
    num, at = {}, {}
    for g in groups:
        for k in g['keys']:
            num[k] = len(num); at[k] = g['at']
    # the sites that take the entry: the first site of each run (a helper's sites in program order with one pointer)
    prev, take = {}, []
    for n, h, key, lo, hi in sites:
        if key in num and prev.get(h) != key and n > at[key]:
            take.append((n, h, num[key], lo, hi))
        prev[h] = key
    if not take:
        return None
    used = {i for _, _, i, _, _ in take}
    groups = [dict(at=g['at'], keys={k: num[k] for k in g['keys'] if num[k] in used}) for g in groups]
    groups = [g for g in groups if g['keys']]
    return dict(keys={k: num[k] for k in num if num[k] in used}, groups=groups, take=take,
                types={t: defs[t][1] for k in num for t in k}, sites=len(sites), covered=sum(1 for s in sites if s[2] in num))


def prefetch_apply(glsl, hs, table, mode):
    """(the text with the prefetch, the plan) or (the text, None) when the module has no pointer it can take"""
    plan = prefetch_plan(glsl, hs)
    if plan is None:
        return glsl, None
    lines = glsl.split('\n')
    last = {h['name']: 'dmsh_g' + h['last'] for h in hs}
    word = lambda t: _pf_word(t, plan['types'][t])
    inserts = {}
    for g in plan['groups']:
        ind = re.match(r'\s*', lines[g['at']]).group(0)
        pf = [ind + '// oot-a122C: pointers this kernel reads through, translated together (gmemfast.py prefetch_apply)']
        order = sorted(g['keys'].items(), key=lambda kv: kv[1])
        for (tl, th), i in order:
            if mode == 'sg':
                pf.append(f'{ind}uint dmsh_pf{i}_lo = subgroupBroadcastFirst({word(tl)});')
                pf.append(f'{ind}uint dmsh_pf{i}_hi = subgroupBroadcastFirst({word(th)});')
            else:
                pf.append(f'{ind}uint dmsh_pf{i}_lo = {word(tl)};')
                pf.append(f'{ind}uint dmsh_pf{i}_hi = {word(th)};')
        # the hint of each (the first thing dmsh_gmem_find reads; when it names the entry, dmsh_gmem_find returns that entry)
        for _, i in order:
            pf.append(f'{ind}uint dmsh_pf{i}_c = (dmsh_pf{i}_hi << 11u) | (dmsh_pf{i}_lo >> 21u);')
        for _, i in order:
            pf.append(f'{ind}uint dmsh_pf{i}_h = {table}._m0[(dmsh_pf{i}_c & 8191u) * 8u + 6u];')
        for _, i in order:
            pf += [f'{ind}uint dmsh_pf{i}_x = dmsh_pf{i}_h & 8191u;',
                   f'{ind}bool dmsh_pf{i}_tag = (dmsh_pf{i}_h >> 13u) == dmsh_pf{i}_c;',
                   f'{ind}bool dmsh_pf{i}_low = dmsh_pf{i}_hi < 256u;',
                   f'{ind}bool dmsh_pf{i}_named = dmsh_pf{i}_x != 0u;',
                   f'{ind}bool dmsh_pf{i}_fits = dmsh_pf{i}_tag && dmsh_pf{i}_low;',
                   f'{ind}bool dmsh_pf{i}_hinted = dmsh_pf{i}_fits && dmsh_pf{i}_named;',
                   f'{ind}if (!dmsh_pf{i}_hinted)', f'{ind}{{',
                   f'{ind}    dmsh_pf{i}_x = dmsh_gmem_find(dmsh_pf{i}_lo, dmsh_pf{i}_hi);', f'{ind}}}']
        for _, i in order:
            pf += [f'{ind}uint dmsh_pf{i}_w = dmsh_pf{i}_x * 8u;',
                   f'{ind}uvec2 dmsh_pf{i}_s = uvec2({table}._m0[dmsh_pf{i}_w], {table}._m0[dmsh_pf{i}_w + 1u]);',
                   f'{ind}uvec2 dmsh_pf{i}_e = uvec2({table}._m0[dmsh_pf{i}_w + 2u], {table}._m0[dmsh_pf{i}_w + 3u]);',
                   f'{ind}uvec2 dmsh_pf{i}_d = uvec2({table}._m0[dmsh_pf{i}_w + 4u], {table}._m0[dmsh_pf{i}_w + 5u]);']
        inserts[g['at']] = inserts.get(g['at'], []) + pf
    for n, h, i, lo, hi in plan['take']:
        sind = re.match(r'\s*', lines[n]).group(0)
        g = last[h]
        inserts[n - 1] = inserts.get(n - 1, []) + [
            f'{sind}if (dmsh_gmem_in(dmsh_pf{i}_s, dmsh_pf{i}_e, uint({lo}), uint({hi})))   // the run\'s first call: the prefetched entry',
            f'{sind}{{',
            f'{sind}    {g}_s = dmsh_pf{i}_s;',
            f'{sind}    {g}_e = dmsh_pf{i}_e;',
            f'{sind}    {g}_d = dmsh_pf{i}_d;',
            f'{sind}}}']
    out = []
    for n, l in enumerate(lines):
        out.append(l)
        out += inserts.get(n, [])
    text = '\n'.join(out)
    if mode == 'sg' and 'GL_KHR_shader_subgroup_ballot' not in text:
        text = text.replace('#version 450\n', '#version 450\n#extension GL_KHR_shader_subgroup_ballot : require\n', 1)
    return text, plan


def rewrite_raw(glsl, strict=True, ways=None, fine=None, prefetch=None):
    _TLS.plan = None
    hs = []
    for name, start, end in functions(glsl):
        h = helper_info(glsl, start, end)
        if not h:
            continue
        if strict and not known(h):
            continue   # not a form the differential test covers: left as it is
        h['start'], h['end'] = start, end
        hs.append(h)
    if not hs or len({h['table'] for h in hs}) != 1:
        return glsl, 0, 0
    table = hs[0]['table']
    # the module must not write the table (a store or an atomic on it)
    if re.search(r'\b' + table + r'\._m0\[[^;=]*\] = ', glsl) or re.search(r'atomic\w+\(' + table, glsl):
        return glsl, 0, 0
    cap, auto = ways_setting(ways)
    if auto and cap > 1:
        try:
            pattern = access_pattern(glsl, hs, cap)
            w = {h['name']: pattern[h['name']]['ways'] for h in hs}
        except Exception:   # the sizing never changes results: the most entries then
            w = {h['name']: cap for h in hs}
    else:
        w = {h['name']: cap for h in hs}
    out, calls = glsl, 0
    for h in sorted(hs, key=lambda h: -h['start']):
        calls += len(re.findall(r'\b' + h['name'] + r'\(', glsl)) - 1
        body = body_ways(w[h['name']]).replace('@LOAD@', LOADS[h['kind']])
        for k, v in (('@NAME@', h['name']), ('@A@', h['a']), ('@B@', h['b']), ('@LAST@', 'dmsh_g' + h['last']), ('@TABLE@', table),
                     ('@VEC@', h['vec'] or '')):
            body = body.replace(k, v)
        out = out[:h['start']] + body + out[h['end']:]
    for h in hs:
        out = out.replace('uint ' + h['last'] + ' = 0u;\n', '', 1)   # nothing reads the entry's number any more
    first = min(out.index('int ' + h['name'] + '(int ') for h in hs)
    byst = sorted(hs, key=lambda h: h['start'])
    decl = ''.join(f'uvec2 dmsh_g{h["last"]}_{c} = uvec2(0u);\n' for h in byst for c in 'sed')
    decl += ''.join(f'uvec2 dmsh_g{h["last"]}_{k}{c} = uvec2(0u);\n' for h in byst for k in range(1, w[h['name']]) for c in 'sed')
    pmode = prefetch_setting(prefetch)
    extra = IN if max(w.values()) > 1 else ''
    find = FIND_FINE if fine_setting(fine) else FIND
    out = out[:first] + decl + '\n' + find.replace('@TABLE@', table) + extra + out[first:]
    if pmode != '0':
        out, plan = prefetch_apply(out, hs, table, pmode)
        if plan:
            _TLS.plan = plan
            if not extra:   # the install's range check (dmsh_gmem_in), after dmsh_gmem_find
                at = out.index('uint dmsh_gmem_find(')
                at = out.index('\n}\n', at) + 3
                out = out[:at] + '\n' + IN + out[at:]
    return out, len(hs), calls


_TLS = threading.local()


def prefetch_last():
    """this thread's last rewrite_raw's prefetch plan (groups, take, sites, covered), None when it prefetched nothing"""
    return getattr(_TLS, 'plan', None)


UMULX = re.compile(r'umulExtended\(([^;]+?), (\d+)u, (_\d+)\._m1, (_\d+)\._m0\);')


def rewrite_umulx(glsl):
    n = 0

    def sub(m):
        nonlocal n
        k = int(m.group(2))
        if m.group(3) != m.group(4) or k < 2 or k & (k - 1) or k >= 1 << 31:
            return m.group(0)
        sh = k.bit_length() - 1
        n += 1
        x = m.group(1)
        return f'{m.group(3)}._m1 = ({x}) >> {32 - sh}u; {m.group(3)}._m0 = ({x}) << {sh}u;'
    return UMULX.sub(sub, glsl), n


def dont_inline(spv_bytes, name=b'dmsh_gmem_find('):
    """the module with FunctionControl DontInline on the function whose OpName starts with name (so spirv-opt -O keeps
    the one copy); None if there is no such function"""
    import struct
    words = list(struct.unpack('<%dI' % (len(spv_bytes) // 4), spv_bytes))
    i, target, done = 5, None, False
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if op == 5:   # OpName
            s = b''.join(struct.pack('<I', w) for w in words[i + 2:i + n]).split(b'\0')[0]
            if s.startswith(name):
                target = words[i + 1]
        elif op == 54 and words[i + 2] == target:   # OpFunction: result type, result id, function control, function type
            words[i + 3] |= 2   # DontInline
            done = True
        i += n
    return struct.pack('<%dI' % len(words), *words) if done else None
