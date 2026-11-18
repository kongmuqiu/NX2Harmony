"""Container half of the mesh emulation's wrap step (nativegames-tools:1: python3, spirv-cross from the seed build,
glslangValidator, spirv-opt, spirv-val). docs/DESIGN-OOT.md, "网格 / 任务着色器在 Maleoon 上的做法".

Maleoon has no VK_EXT_mesh_shader. A task / mesh draw is replayed as compute dispatches and one indexed draw
(overlay android/gfx/vk_draw.c mesh_draw). The translator already gives each task and mesh shader as a compute kernel
whose stage memory is its shared memory (oot/oot_mesh.py). Here each kernel <key>-<suffix>.spv of <kernels> becomes the
compute shader the replay dispatches, and a mesh kernel also gets the vertex shader of the draw:

  1. SPIRV-Cross turns the kernel into Vulkan GLSL;
  2. its main() is renamed dmsh_kernel() and a new main() is put around it, as nxnative's dmsh_meshstage.py does with the
     Metal kernel (the stage memory starts at zero; thread 0 writes the workgroup's number; barrier, kernel, barrier);
  3. glslangValidator compiles it, spirv-opt -O takes its thousands of function variables away (glslang makes one for
     every temporary of the GLSL; Maleoon's compiler took minutes on a module with 8258), spirv-val checks it;
  4. <key>-<suffix>.vk is the kernel's reflection plus the new bindings and a "mesh" line of the numbers the replay needs.

The draw's control block (binding kind RB_MESH_PAR, a storage buffer of words; the replay fills it, vk_draw.c):
  c[0]        the draw's first workgroup
  c[1]        the mesh workgroups the draw's vertex and index buffers hold (a group past it does nothing)
  c[2]        the task workgroups the payload buffer holds
  c[4..6]     the task dispatch's indirect record {tasks, 1, 1}
  c[8..10]    the mesh dispatch's indirect record {groups, 1, 1}; with a task shader c[8] counts the launches (atomic)
  c[12..16]   the draw's indirect record {indices, 1, 0, 0, 0}: every mesh group adds its primitives' indices (atomic)
  c[17]       the vertices the mesh groups have taken (atomic)

A task kernel (kind RB_MESH_IN written):
  the payload buffer's words [0, 2 * c[1]) are the table of the mesh groups, the payloads follow, a task's PW words each;
  task g copies its payload out, takes n = its launch count (payload word 1) numbers from c[8], and writes for each of
  its mesh groups (number j of its launches) the table's two words: the payload's place and j.
A mesh kernel:
  with a task (RB_MESH_IN read): its payload from the table, and j into the workgroup's number and the payload's word 0;
  without: the workgroup's number is c[0] + its group.
  Dense output (what a mesh shader's SetMeshOutputs gives the rasterizer: the vertices and primitives in use, nothing
  else). After the kernel a workgroup knows its primitive count (the byte at the primitive area + 3) and the vertices
  those primitives use (the highest index + 1); it takes that many vertices from c[17] and count * 3 indices from c[12]:
  vertices   <vtx>.w[(v0 + v) * NCOMP + c] = the word of component c of vertex v (stage memory: blocks of 32 vertices,
             a component 0x80 bytes, a vertex a word), v0 the workgroup's first vertex
  indices    <idx>.w[i0 + p * 3 + k] = v0 + index byte k of primitive p, i0 the workgroup's first index
  The buffers still hold capacity * MAX_V vertices and capacity * MAX_P * 3 indices (the most there can be); the draw is
  c[12] indices. Fixed slots a workgroup (oot-a70 .. a79) made the draw shade every slot's vertex and take MAX_P
  triangles a workgroup, degenerate ones for the primitives not there: on this tile-based GPU the mesh draws' geometry
  cost 0.5 - 1.3 GiB of GPU memory at Kokiri Forest's gate (GL 3.6 GiB without the draws, 4.1 - 4.9 with them: the
  system's GPU_THRESHOLD_KILLER closes the process near 5). The order of the workgroups in the buffers is not fixed.
<key>-meshvs.spv / .vk: the pass-through vertex shader: gl_VertexIndex picks the vertex's words in <vtx> (binding 0), the
position gets the depth conversion a translated vertex shader has (z = z / 2 + w / 2), generic attribute n goes out at
location n as a vec4 (0, 0, 0, 1 where the mesh shader writes no component), as the translated stages link.
Not yet: per-primitive attributes (4 shaders: wrapped, the attributes are not passed on), point meshes (5: refused).

Stage memory modes (wrap_modes(); the 6th argument, comma separated, 'none' for none; default DEFAULT_MODES). A mode is
applied to a mesh kernel only when its GLSL shows what the mode needs (every use of the stage memory is a constant word or
the translator's 'register + K' form, and the K say which area it is); a kernel that does not show it keeps the path above.
  bytes    the primitive area (count byte, index bytes) lives in its own shared array of one WORD a byte (dmsh_pb): the
           translator's byte store helper (a load + atomicCompSwap loop on the word) becomes one plain word store, and the
           count and the index bytes are read as words
  inputs   the workgroup's number and the payload words are read where they come from (a private global, the task's
           buffer): no payload copy, no number store, no barriers for them, no clear below the attribute words
  direct   a mesh kernel's attribute word stores go straight to the vertex buffer, into the workgroup's own slot
           ((group * MAX_V + v) * NCOMP + c: the layout <key>-meshvs reads does not change); the slot is cleared first
           (the words a kernel leaves are zero, as the cleared stage memory gave), the copy-out of vertices and the
           vertex counter go away, the indices stay dense (they name group * MAX_V + index byte). With bytes the stage
           memory array shrinks to the input area, with inputs as well to one word.
           WHY IT MATTERS (measured on the phone, oot-a96, kernel eb021183bf9b0092, 1620 workgroups a dispatch): a
           workgroup's time on Maleoon follows the SIZE of its shared memory: 22.8 us with the 11.4 KB stage memory,
           43 us with the array declared twice as long and nothing else changed, 7 - 9 us with 3.6 KB. The driver runs
           fewer workgroups at a time the more shared memory each one has.
  whole    with direct: the slot is NOT cleared first when every vertex the kernel stores is stored whole: its attribute
           stores, grouped by the register they add their constants to (one group a vertex), each hold every component
           once, at one place of the control flow, with no return between them. A vertex no thread stores keeps what
           the buffer held; a triangle names only vertices its kernel stored (as a mesh shader's outputs: what it did not
           write is undefined on the console too), so nothing reads those words. A kernel that does not pass the check
           keeps the clear. On the phone the clear was a quarter of the largest kernel's time (8.0 -> 6.0 us a workgroup:
           the GPU is bound by memory traffic there, bandwidth load 10 of 10 by the system's own figure).
  redux    REDUX's 32 constant-lane shuffles -> subgroupAdd / subgroupMax (needs VK_SUBGROUP_FEATURE_ARITHMETIC_BIT); not
           in the default (measured: no gain)
  payload  a TASK kernel (oot-a106; the modes above are a mesh kernel's): its stage memory goes away. What it held:
           word 0 the workgroup's number (the kernel's only read), an input area a task does not have (as long as the
           payload, nothing uses it), and the payload the kernel stores. Now the number is a private global each thread
           sets itself (dmsh_wg), a payload word store goes straight to the task's place of the payload buffer
           (dmsh_mesh_in.w[dmsh_pay + word]; the place and the buffer's layout do not change, the replay neither), and the
           payload's word 1, the launch count the wrapper itself needs after the kernel, stays shared (dmsh_x[1]; thread 0
           copies it out after the kernel, so the buffer's word and the count used are one value, as before). The place is
           cleared first (the words a kernel leaves are zero, as the cleared stage memory gave): one clear of the payload's
           words instead of the clear of the stage memory and the copy-out, and two barriers fewer. Shared memory:
           2 words (dmsh_x) instead of 272 .. 6416 bytes.
           Taken only when the kernel's GLSL shows it is sound (task_payload()): no byte / half-word store helper and no
           atomic on the stage memory; every use is a constant word or 'register + K'; every store lies in the payload
           (a constant word of it, or K in it: a register is not negative, as the mesh modes read it); every load is the
           workgroup's number - or, in a kernel of ONE thread, a payload word (that thread reads back what it stored or
           the cleared word, in its own order; with more threads a read of a word another thread stores would have to
           keep the order shared memory and the kernel's barriers give: such a kernel keeps the stage memory). The 20
           task kernels of the corpus all pass (18 of 32 threads that only store, 2 of one thread, one of which reads a
           word back). A kernel that does not pass keeps the path above.

usage: mesh_wrap_in_container.py <kernels dir> <out dir> <mesh-info.json> <spirv-cross> [jobs] [modes]"""
import json, os, re, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

kernels = out = info = spirv_cross = None   # set by main(); tools/mesh_selftest imports wrap() and vertex_shader()
RB_MESH_IN, RB_MESH_VTX, RB_MESH_IDX, RB_MESH_PAR = 21, 22, 23, 24
UNIFORM_BUFFER, STORAGE_BUFFER = 6, 7
C_FIRST, C_CAP, C_TCAP, C_TDISP, C_DISP, C_DRAW, C_VERT, C_WORDS = 0, 1, 2, 4, 8, 12, 17, 20   # the control block's words
SHARED = re.compile(r'^shared uint (\w+)\[(\d+)\];$', re.M)
MAIN = re.compile(r'^void main\(\)\n\{', re.M)
BINDING = re.compile(r'layout\(set = 0, binding = (\d+)')
# the translator's byte / half-word store of shared memory (HelperFunctionManager.GenerateSharedStore)
HELPER = re.compile(r'^void (dmsh_shared_store(\d+)_\d+)\(int (_\d+), int (_\d+)\)\n\{\n(.*?)\n\}\n', re.M | re.S)
DEF = re.compile(r'^\s*(?:int|uint|float|bool)\s+(_\d+)\s*=\s*(.*);$', re.M)
# Every place a temporary gets a value, anywhere in a line: its definition with an initialiser counts once, so a
# temporary with more than one is assigned after its definition (also by a compound operator or ++ / --) and is not a
# single definition.
ASSIGN = re.compile(r'(?<![\w.])(_\d+)\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)|(?:\+\+|--)\s*(_\d+)\b')
KNOWN_MODES = frozenset('bytes inputs direct whole redux noclear payload none'.split())
OUTFN = re.compile(r'^\w[\w ]*?\b(\w+)\(([^()]*\b(?:out|inout)\b[^()]*)\)\s*$', re.M)   # the signature of a function with an out / inout parameter
CONTROL = re.compile(r'\b(?:if|else|for|while|do|switch)\b')
# what a register may be made of to have a value that does not depend on when it was computed (register_key)
PURE_WORDS = frozenset(('int uint min max floatBitsToUint floatBitsToInt uintBitsToFloat intBitsToFloat '
                        'gl_SubgroupInvocationID gl_LocalInvocationIndex gl_LocalInvocationID gl_WorkGroupID gl_GlobalInvocationID x y z').split())
TOKEN = re.compile(r'_\d+\b|[A-Za-z_]\w*|\d[\w.]*|\S')
DEFAULT_MODES = 'bytes,inputs,direct,whole,payload'   # the vertex buffer's and the payload buffer's layouts do not change
MODES = frozenset(DEFAULT_MODES.split(','))


sys.path.insert(0, str(Path(__file__).resolve().parent))
import gmemfast   # noqa: E402  the raw global memory post-passes (gmemfast.py, next to this file)
# Which of them run (DMSH_GMEM): 'base' none, 'umulx' the multiply only, 'fastI' the lookups with the miss function inlined
# by spirv-opt, 'fastN' with it kept out of line (FunctionControl DontInline), 'all' = fastN + umulx (the default: the
# 108 kernels that read through the pool table go from 90 to 32 MB of SPIR-V, a lookup that hits from 92 to 55
# instructions and from 6 table loads to none; results equal on lavapipe, prepared/oot/mesh-build/exp-gmem-verify).
GMEM = os.environ.get('DMSH_GMEM', 'all')
# DMSH_GMEM_PREFETCH (oot-a122C; read by gmemfast.rewrite_raw, so only with fastI / fastN / all): '0' (the default) none;
# 'sg' (or 1) the pointers a kernel reads its streams through are translated together right after the kernel loaded them,
# from the subgroup's first lane, and the first call of each run takes that entry (gmemfast.py prefetch_apply: the 6 - 7
# lookups a thread of 3439aaa0 / fed302d7 / eb021183 become 2 - 3 overlapped ones); 'lane' the same per thread.
# wrapped.json 'prefetch': module -> [pointers, run starts that take an entry, sites through them, sites].
PREFETCHED = {}


def gmem_pass(glsl):
    """SPIRV-Cross's GLSL of a kernel with the raw global memory helpers and the 64-bit address multiplies rewritten."""
    if GMEM in ('fastI', 'fastN', 'all'):
        glsl, _, _ = gmemfast.rewrite_raw(glsl)
    if GMEM in ('umulx', 'all'):
        glsl, _ = gmemfast.rewrite_umulx(glsl)
    return glsl


def div_const(n, limit):
    """(m, shift) with (q * m) >> shift == q // n for every q < limit (a power of two: m 1, its shift)"""
    if n & (n - 1) == 0:
        return 1, n.bit_length() - 1
    for shift in range(8, 24):
        m = (1 << shift) // n + 1
        if all((q * m) >> shift == q // n for q in range(limit)) and (limit - 1) * m < 1 << 32:
            return m, shift
    raise ValueError(f'no multiplier for a division by {n} below {limit}')


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)


def ssa_defs(glsl):
    """name -> expression, of the temporaries defined once and never assigned again (SPIRV-Cross declares a value that
    control flow joins as 'int _79 = 0;' and assigns it later: its first value says nothing)"""
    count = {}
    for a, b2 in ASSIGN.findall(glsl):
        count[a or b2] = count.get(a or b2, 0) + 1
    defs, twice = {}, {n for n, c in count.items() if c > 1}
    # a function with an out / inout parameter can write the temporaries it is called with: every temporary on the rest of
    # a line that calls one counts as assigned again (the corpus passes only uninitialised declarations there)
    outs = {}   # function -> the places of its out / inout parameters, counted from the END of the argument list
    for m in OUTFN.finditer(glsl):
        params = m.group(2).split(',')
        outs[m.group(1)] = [len(params) - k for k, p in enumerate(params) if re.match(r'\s*(?:out|inout)\b', p)]
    outs.update({'umulExtended': [1, 2], 'imulExtended': [1, 2], 'uaddCarry': [1], 'usubBorrow': [1], 'frexp': [1], 'modf': [1]})
    for fn, places in outs.items():
        for m in re.finditer(r'\b' + re.escape(fn) + r'\((.*)$', glsl, re.M):
            if ';' not in m.group(1):   # the function's own signature line
                continue
            args = [a.strip() for a in m.group(1).rsplit(')', 1)[0].split(',')]
            last = [args[-k] for k in places if k <= len(args)]
            if all(re.fullmatch(r'[\w.\[\]]+', a) for a in last):
                twice.update(a for a in last if re.fullmatch(r'_\d+', a))
            else:   # not read: every temporary on the rest of the line
                twice.update(re.findall(r'(?<![\w.])_\d+\b', m.group(1)))
    for name, e in DEF.findall(glsl):
        if name in defs:
            twice.add(name)
        defs[name] = e
    return {n: e for n, e in defs.items() if n not in twice}


def split_sum(e):
    """(register, K) of 'register + K' or 'K + register'; None"""
    m = re.fullmatch(r'(_\d+) \+ (\d+)', e)
    if m:
        return m.group(1), int(m.group(2))
    m = re.fullmatch(r'(\d+) \+ (_\d+)', e)
    return (m.group(2), int(m.group(1))) if m else None


def resolve_sum(defs, var):
    """(register or None, K) of the byte address in a temporary: the constants of 'r + K' are added up while r is itself a
    single definition of that form (a register that carries an area's offset is classed by the whole sum, oot-a103);
    register None: the address is the constant K. None when it is not understood."""
    k, reg = 0, None
    for _ in range(16):
        e = defs.get(var)
        if e is None:
            return (reg, k) if reg else None
        if re.fullmatch(r'\d+', e):
            return None, k + int(e)
        m = split_sum(e)
        if m:
            reg, var, k = m[0], m[0], k + m[1]
        elif re.fullmatch(r'_\d+', e):
            var = e
            if reg:
                reg = e
        else:
            return (reg, k) if reg else None
    return None


def resolve_byte(defs, var):
    """the byte address in a temporary: ('const', byte) | ('dyn', K) for 'register + K', K >= 0 | None (not understood)"""
    r = resolve_sum(defs, var)
    if r is None:
        return None
    return ('dyn', r[1]) if r[0] else ('const', r[1])


def resolve_word(defs, idx):
    """the same for the index text of MEM[...]: a constant word, or the translator's 'uint(int(uint(byte) >> uint(2)))'"""
    idx = idx.strip()
    m = re.fullmatch(r'(\d+)u?', idx)
    if m:
        return ('const', int(m.group(1)) * 4)
    m = re.fullmatch(r'int\((_\d+)\)', idx) or re.fullmatch(r'(_\d+)', idx)
    e = defs.get(m.group(1)) if m else None
    m = re.fullmatch(r'uint\(int\(uint\((_\d+)\) >> uint\(2\)\)\)', e) if e else None
    return resolve_byte(defs, m.group(1)) if m else None


def store_register(defs, idx):
    """(register, K) of the index text of a word store 'MEM[int(_w)]' with _w = uint(int(uint(_b) >> uint(2))) and
    _b = _register + K; None when it is not that"""
    m = re.fullmatch(r'int\((_\d+)\)', idx.strip()) or re.fullmatch(r'(_\d+)', idx.strip())
    e = defs.get(m.group(1)) if m else None
    m = re.fullmatch(r'uint\(int\(uint\((_\d+)\) >> uint\(2\)\)\)', e) if e else None
    r = resolve_sum(defs, m.group(1)) if m else None
    return r if r and r[0] else None


class NotSingle(Exception):
    pass


def register_key(defs, reg, depth=16):
    """What a store's register IS, as text. A single definition is computed once per pass through the block that
    declares it, so within one pass its NAME is one value, whatever it is made of. Where its definition is pure
    (constants, the invocation's built-ins, casts, min / max) and made of single definitions alone, it is written out
    instead: two registers with the same text then hold the same value wherever they were computed (the 53 large
    kernels store one vertex through two such registers). A definition that reads memory, calls a function or uses a
    temporary assigned more than once stays as its name. A register that is itself assigned more than once has no such
    text (NotSingle): the same name can hold two values in one pass."""
    e = defs.get(reg)
    if e is None:
        raise NotSingle(reg)
    toks = TOKEN.findall(e)
    if (depth == 0 or '[' in toks
            or any(re.match(r'[A-Za-z_]', t) and not re.fullmatch(r'_\d+', t) and t not in PURE_WORDS for t in toks)
            or any(re.fullmatch(r'_\d+', t) and t not in defs for t in toks)):
        return reg
    return '(' + ' '.join(register_key(defs, t, depth - 1) if re.fullmatch(r'_\d+', t) else t for t in toks) + ')'


def column_register(defs, reg, ncomp):
    """the register is block * (ncomp * 128) + lane * 4 with lane below 32, whatever block and lane are made of: only then is
    'register + outputs + c * 128' component c of ONE vertex. The translator's forms, and the self-tests' one-line forms."""
    m = re.fullmatch(r'(_\d+) \+ (_\d+)', defs.get(reg, ''))
    if not m:
        return False

    def lane(t):
        e = defs.get(t, '')
        if re.fullmatch(r'124 & _\d+', e) or re.fullmatch(r'\(_\d+ & 31\) (?:<< 2|\* 4)', e):
            return True
        x = re.fullmatch(r'(_\d+) (?:<< 2|\* 4)', e)
        return bool(x and re.fullmatch(r'(?:_\d+|int\(_\d+\)) & 31', defs.get(x.group(1), '')))

    def block(t):
        x = re.fullmatch(r'int\((?:_\d+|uint\(_\d+\) >> uint\(5\))\) (?:\* (\d+)|<< (\d+))', defs.get(t, ''))
        return bool(x) and (int(x.group(1)) if x.group(1) else 1 << int(x.group(2))) == ncomp * 128

    a, b = m.groups()
    return (block(a) and lane(b)) or (block(b) and lane(a))


def block_paths(lines):
    """for each line the braces open around it, as the tuple of the lines that opened them"""
    stack, out = [], []
    for n, l in enumerate(lines):
        for _ in range(l.count('}')):
            if stack:
                stack.pop()
        out.append(tuple(stack))
        for _ in range(l.count('{')):
            stack.append(n)
    return out


def stores_whole(glsl, acc, outp, ncomp):
    """every vertex the kernel stores is stored whole (the mode 'whole'): see the head of this file"""
    defs = ssa_defs(glsl)
    lines = glsl.split('\n')
    paths = block_paths(lines)
    # "one place of the control flow" is read from the braces: every if / else / loop / switch line must open a block on
    # the next line, as SPIRV-Cross writes them (a statement without braces would be a place of its own)
    if any(CONTROL.search(l) and not l.rstrip().endswith((';', '{')) and lines[n + 1].strip() != '{' for n, l in enumerate(lines[:-1])):
        return False
    groups = {}
    for k, c, n, idx in acc:
        if k != 'store':
            continue
        rk = store_register(defs, idx)
        if rk is None or rk[1] < outp or (rk[1] - outp) % 128:
            return False
        q = (rk[1] - outp) // 128
        if not column_register(defs, rk[0], ncomp):
            return False
        try:
            key = register_key(defs, rk[0])   # by value: a vertex stored through two names of one value is one vertex (oot-a103)
        except NotSingle:
            return False
        groups.setdefault((key, q // ncomp), []).append((q % ncomp, n))
    if not groups:
        return False
    for items in groups.values():
        ns = [n for _, n in items]
        if {c for c, _ in items} != set(range(ncomp)) or len({paths[n] for n in ns}) != 1:
            return False
        # nothing between a vertex's stores may leave the block or enter it halfway (a case label)
        if any(re.search(r'\b(return|break|continue|discard|case|default|switch)\b', lines[x]) for x in range(min(ns), max(ns) + 1)):
            return False
    return True


def stage_accesses(glsl, mem):
    """[(kind, class, line, index text)] of every use of the stage memory outside the byte store helpers: kind 'load', 'store',
    'atomic', or 'byte' (a call of the byte store helper, the class is its address); class as resolve_byte. None when a line
    names the array in a way this does not read."""
    body = HELPER.sub(lambda m: '\n' * m.group(0).count('\n'), glsl)
    defs = ssa_defs(glsl)
    acc = []
    name = re.escape(mem)
    for n, l in enumerate(body.split('\n')):
        s = l.strip()
        for c in re.finditer(r'\bdmsh_shared_store(\d+)_\d+\((_\d+), _\d+\);', s):
            acc.append(('byte' if c.group(1) == '8' else 'half', resolve_byte(defs, c.group(2)), n, c.group(2)))
        if not re.search(r'\b' + name + r'\b', s) or s.startswith('shared uint '):
            continue
        rest = s
        for a in re.finditer(r'\batomic\w+\(' + name + r'\[([^\[\]]*)\]', s):
            acc.append(('atomic', resolve_word(defs, a.group(1)), n, a.group(1))); rest = rest.replace(a.group(0), '')
        st = re.match(name + r'\[([^\[\]]*)\] = (.*);$', rest)
        if st:
            acc.append(('store', resolve_word(defs, st.group(1)), n, st.group(1))); rest = st.group(2)
        for ld in re.finditer(r'\b' + name + r'\[([^\[\]]*)\]', rest):
            acc.append(('load', resolve_word(defs, ld.group(1)), n, ld.group(1)))
        if re.search(r'\b' + name + r'\b', re.sub(r'\b' + name + r'\[[^\[\]]*\]', '', rest)):
            return None   # the array without an index, or an index with brackets in it
    if len(re.findall(r'\bdmsh_shared_store\d+_\d+\(', body)) != sum(1 for k, _, _, _ in acc if k in ('byte', 'half')):
        return None       # a call of the byte store helper in a form this does not read
    return acc


def helper_is_cas8(m, mem):
    """the helper found is the byte store this file knows: the compare-and-swap loop on the word of the stage memory"""
    a, v = m.group(3), m.group(4)
    t = r'_\d+'
    want = (rf'    uint ({t}) = uint\(int\(uint\({a}\) >> uint\(2\)\)\);\n    int ({t}) = {a} & 3;\n    int ({t}) = \2 << 3;\n    bool ({t});\n    do\n    \{{\n'
            rf'        float ({t}) = uintBitsToFloat\({re.escape(mem)}\[int\(\1\)\]\);\n'
            rf'        int ({t}) = int\(bitfieldInsert\(floatBitsToUint\(\5\), uint\({v}\), int\(uint\(\3\)\), int\(8u\)\)\);\n'
            rf'        uint ({t}) = atomicCompSwap\({re.escape(mem)}\[int\(\1\)\], floatBitsToUint\(\5\), uint\(\6\)\);\n'
            rf'        uint ({t}) = \7;\n        \4 = int\(\8\) != floatBitsToInt\(\5\);\n    \}} while \(\4\);')
    return m.group(2) == '8' and re.fullmatch(want, m.group(5)) is not None


# ---- REDUX ----
def redux(glsl):
    """(the GLSL with each REDUX chain InstEmitAmpereTensor.ARedux emits replaced by one subgroup reduction, the chains
    replaced). A chain: 'uint M = subgroupBallot(true).x;', then for lane 0 four lines (shuffle, the lane's bit of M, the
    test, the start: the lane's value or the operation's identity), for each of lanes 1..31 six (shuffle, the operation with
    the running value, M >> lane, its bit, the test, the select). Integer sum and unsigned maximum are what the corpus has."""
    lines = glsl.split('\n')
    t = r'(_\d+)'
    first = re.compile(r'^(\s*)float ' + t + r' = subgroupShuffle\((.+), 0u\);$')
    res, i, done = [], 0, 0
    while i < len(lines):
        m = first.match(lines[i])
        b = lines[i:i + 190]
        mb = re.match(r'^\s*uint ' + t + r' = subgroupBallot\(true\)\.x;$', lines[i - 1]) if m and i else None
        if not (m and mb and len(b) == 190):
            res.append(lines[i]); i += 1; continue
        ind, x, M = m.group(1), m.group(3), mb.group(1)
        ex = re.escape(x)
        def line(k, pat):
            return re.match(r'^\s*' + pat + r'$', b[k])
        ok, op, names = True, None, [m.group(2)]
        m1, m2 = line(1, rf'int {t} = int\({M}\) & 1;'), line(2, rf'bool {t} = {t} != 0;')
        m3 = line(3, rf'(int|uint) {t} = (uint\()?{t} \? floatBitsToInt\({t}\) : 0\)?;')
        ok = bool(m1 and m2 and m3) and m2.group(2) == m1.group(1) and m3.group(4) == m2.group(1) and m3.group(5) == names[0]
        if ok:
            names += [m1.group(1), m2.group(1)]
            acc = m3.group(2)
        for k in range(1, 32):
            if not ok:
                break
            o = 4 + (k - 1) * 6
            s = line(o, rf'float {t} = subgroupShuffle\({ex}, {k}u\);')
            add = line(o + 1, rf'int {t} = {t} \+ floatBitsToInt\({t}\);')
            umax = line(o + 1, rf'uint {t} = max\({t}, floatBitsToUint\({t}\)\);')
            r = add or umax
            sh = line(o + 2, rf'uint {t} = uint\(int\(uint\(int\({M}\)\) >> uint\({k}\)\)\);')
            bit, tst = line(o + 3, rf'int {t} = int\({t}\) & 1;'), line(o + 4, rf'bool {t} = {t} != 0;')
            sel = (line(o + 5, rf'(?:int|uint) {t} = (?:uint\()?{t} \? (?:int\()?{t}\)? : (?:int\()?{t}\)?\)?;'))
            this = 'add' if add else 'umax'
            ok = (bool(s and r and sh and bit and tst and sel) and (op is None or op == this) and r.group(2) == acc and r.group(3) == s.group(1)
                  and bit.group(2) == sh.group(1) and tst.group(2) == bit.group(1) and sel.group(2) == tst.group(1)
                  and sel.group(3) == r.group(1) and sel.group(4) == acc)
            if ok:
                op = this
                names += [s.group(1), r.group(1), sh.group(1), bit.group(1), tst.group(1), acc]
                acc = sel.group(1)
        fin = re.match(r'^\s*(int|uint) ' + t + ' = ', b[189]) if ok else None
        # the chain's temporaries are not used after it (its last select is the result)
        rest = '\n'.join(lines[i + 190:])
        if not (ok and fin) or any(re.search(r'\b' + n + r'\b', rest) for n in set(names)):
            res.append(lines[i]); i += 1; continue
        e = f'subgroupAdd(floatBitsToInt({x}))' if op == 'add' else f'subgroupMax(floatBitsToUint({x}))'
        if (fin.group(1) == 'int') != (op == 'add'):
            e = f'{fin.group(1)}({e})'
        res.append(f'{ind}{fin.group(1)} {fin.group(2)} = {e};   // REDUX.{op} of the active lanes')
        i += 190; done += 1
    text = '\n'.join(res)
    if done and 'GL_KHR_shader_subgroup_arithmetic' not in text:
        text = text.replace('#version 450\n', '#version 450\n#extension GL_KHR_shader_subgroup_arithmetic : require\n', 1)
    return text, done


def task_payload(glsl, mem, i, pay_words):
    """The mode 'payload' of a task kernel: (the GLSL without its stage memory, the declarations that replace it), or None
    when the kernel does not show what the mode needs (the head of this file says what, and why)."""
    wg, _, outp, _ = i['stage_memory']
    end = outp + pay_words * 4
    if pay_words < 2 or HELPER.search(glsl):
        return None
    name0 = re.escape(mem)
    # (oot-a107, from the review) a read-modify-write of a stage word is read as a load only, and with the stores in a
    # buffer a barrier() of the kernel's own no longer orders them: such kernels keep the stage memory
    if re.search(name0 + r'\[[^\[\]]*\]\s*(?:[-+*/%&|^]=|<<=|>>=|\+\+|--)|(?:\+\+|--)\s*' + name0 + r'\[', glsl):
        return None
    if re.search(r'\b(?:barrier|memoryBarrier\w*|groupMemoryBarrier)\s*\(', glsl):
        return None
    acc = stage_accesses(glsl, mem)
    if acc is None or any(c is None or k not in ('load', 'store') for k, c, _, _ in acc):
        return None
    inside = lambda c: outp <= c[1] < end
    if not all(inside(c) for k, c, _, _ in acc if k == 'store'):
        return None
    if not all(c == ('const', wg) or (i['threads'] == 1 and inside(c)) for k, c, _, _ in acc if k == 'load'):
        return None
    defs = ssa_defs(glsl)
    name = re.escape(mem)
    place = lambda c: 'dmsh_x[1]' if c[1] == outp + 4 else f'dmsh_mesh_in.w[dmsh_pay + {(c[1] - outp) // 4}u]'
    used = set()

    def load(mm):
        c = resolve_word(defs, mm.group(1))
        if c == ('const', wg):
            return 'dmsh_wg'
        if c[0] == 'const':
            return place(c)
        used.add('load')
        return f'dmsh_pload(uint({mm.group(1)}))'

    lines = glsl.split('\n')
    for n in sorted({n for _, _, n, _ in acc}):
        l = lines[n]
        ind, s = l[:len(l) - len(l.lstrip())], l.strip()
        if any(k == 'store' and x == n for k, _, x, _ in acc):
            st = re.match(name + r'\[([^\[\]]*)\] = (.*);$', s)   # as stage_accesses read it
            value = re.sub(r'\b' + name + r'\[([^\[\]]*)\]', load, st.group(2))
            c = resolve_word(defs, st.group(1))
            if c[0] == 'const':
                lines[n] = f'{ind}{place(c)} = {value};'
            else:
                used.add('store')
                lines[n] = f'{ind}dmsh_pstore(uint({st.group(1)}), {value});'
        else:
            lines[n] = ind + re.sub(r'\b' + name + r'\[([^\[\]]*)\]', load, s)
    text = re.sub(r'^shared uint ' + name + r'\[\d+\];\n', '', '\n'.join(lines), count=1, flags=re.M)
    if re.search(r'\b' + name + r'\b', text):
        return None   # a use of the array that was not rewritten
    extra = ['uint dmsh_wg;    // the workgroup\'s number', 'uint dmsh_pay;   // the place of this task\'s payload in the payload buffer',
             'shared uint dmsh_x[2];   // [0] the first number of its launches, [1] the payload\'s word 1: the launch count', '']
    # a word outside the payload (no kernel of the corpus has one) is not stored and reads 0: the copy-out of the stage
    # memory copied the payload's words only (until oot-a107 it was clamped onto the payload's last word)
    idx = f'    uint s = w - {outp // 4}u;'
    if 'store' in used:
        extra += ['void dmsh_pstore(uint w, uint v)   // a payload word, straight to the payload buffer', '{', idx,
                  f'    if (s >= {pay_words}u) return;',
                  '    if (s == 1u) dmsh_x[1] = v; else dmsh_mesh_in.w[dmsh_pay + s] = v;', '}', '']
    if 'load' in used:
        extra += ['uint dmsh_pload(uint w)   // (a kernel of one thread: what it stored itself, or the cleared word)', '{', idx,
                  f'    if (s >= {pay_words}u) return 0u;',
                  '    return s == 1u ? dmsh_x[1] : dmsh_mesh_in.w[dmsh_pay + s];', '}', '']
    return text, extra


def wrap(glsl, i):
    """(the wrapped GLSL, the new bindings [(binding, dtype, kind, rw)]) of a task or mesh kernel, with the default modes."""
    return wrap_modes(glsl, i)[:2]


def wrap_modes(glsl, i, modes=None):
    """(the wrapped GLSL, the new bindings [(binding, dtype, kind, rw)], the modes applied) of a task or mesh kernel."""
    modes = MODES if modes is None else frozenset(modes)
    applied = set()
    m = SHARED.search(glsl)
    if not m or len(SHARED.findall(glsl)) != 1:
        raise ValueError('not exactly one shared array (the stage memory)')
    mem, words = m.group(1), int(m.group(2))
    if words * 4 < i['shared']:
        raise ValueError(f'shared memory of {words * 4} bytes under the stage memory\'s {i["shared"]}')
    # Ten of Ocarina of Time's mesh shaders (position only, 64 vertices, 126 primitives: 1a0ac6f99265b586 ...) keep workgroup
    # variables in stage memory past what their SPHs account for: stores with the payload mode at outputs + 0x1b60, + 0x1b64
    # and + 0x1b84 (words 1820, 1821, 1829 of a 421-word array). The kernel's SPIR-V indexes past its array there (no
    # static check), glslang refuses the constant index. The array is made as long as the highest constant index needs.
    top = max((int(x) for x in re.findall(r'\b' + re.escape(mem) + r'\[(\d+)\]', glsl)), default=0)
    if top >= words:
        glsl = glsl.replace(m.group(0), f'shared uint {mem}[{top + 1}];')
        words = top + 1
    # The same for a thread's local memory (a private array): the kernel is translated as compute with its SPH taken off, so
    # the array has the compute default (1024 words), while those ten shaders' SPHs ask for more (they reach word 2313)
    for decl in re.finditer(r'^uint (\w+)\[(\d+)\];$', glsl, re.M):
        name, n = decl.group(1), int(decl.group(2))
        t = max((int(x) for x in re.findall(r'\b' + re.escape(name) + r'\[(\d+)\]', glsl)), default=0)
        if t >= n:
            glsl = glsl.replace(decl.group(0), f'uint {name}[{t + 1}];')
    if len(MAIN.findall(glsl)) != 1:
        raise ValueError('no single main()')
    wg, inp, outp, prim = i['stage_memory']
    if outp % 4 or prim % 4 or inp % 4 or wg % 4:
        raise ValueError('stage memory offsets not on words')
    if wg != 0 or inp < 16:
        raise ValueError('stage memory without the free words 1 .. 3 the dense output uses')
    threads = i['threads']
    T = f'{threads}u'
    pay_words = (i['payload'] + 3) // 4
    mesh = i['kind'] == 'ms'
    if 'redux' in modes:
        glsl, n = redux(glsl)
        if n:
            applied.add('redux')

    # The stage memory modes of a mesh kernel: only when every use of the stage memory is understood (stage_accesses)
    extra = []        # declarations and functions that go after the new buffers
    inputs = bytes_mode = direct = whole = False
    pb = i['shared'] - prim if mesh else 0   # the primitive area's bytes
    acc = stage_accesses(glsl, mem) if mesh and modes & {'bytes', 'inputs', 'direct'} else None
    if acc is not None and all(c is not None for _, c, _, _ in acc) and not any(k == 'half' for k, _, _, _ in acc):
        helpers = list(HELPER.finditer(glsl))
        at = lambda c: c[1]
        calls = [c for k, c, _, _ in acc if k == 'byte']
        # the primitive area is the byte stores' alone, and they store nowhere else
        prim_private = (all(prim <= at(c) and (c[0] == 'dyn' or at(c) < i['shared']) for c in calls)
                        and all(at(c) < prim for k, c, _, _ in acc if k != 'byte'))
        ncomp_d, max_v_d = len(i['vertex']), max(i['max_v'], 1)
        nvw = max_v_d * ncomp_d
        # Not with per-primitive attributes (3 triangle kernels of the corpus): their words lie after the vertex blocks at
        # addresses whose constant is a vertex component's, the copy-out leaves them in the stage memory, and here they
        # would land in the vertex slot.
        if ('direct' in modes and prim_private and ncomp_d and max_v_d % 32 == 0 and i.get('verts', 3) == 3 and not i.get('primitive')
                and all(at(c) < outp for k, c, _, _ in acc if k in ('load', 'atomic'))      # nothing reads the attribute words
                and all(outp <= at(c) for k, c, _, _ in acc if k == 'store')):              # and every word store is one of them
            if 'whole' in modes and stores_whole(glsl, acc, outp, ncomp_d):
                applied.add('whole'); whole = True
            lines = glsl.split('\n')
            stre = re.compile(r'^(\s*)' + re.escape(mem) + r'\[([^\[\]]*)\] = (.*);$')
            for k, c, n, _ in acc:
                if k == 'store':
                    st = stre.match(lines[n])
                    lines[n] = f'{st.group(1)}dmsh_vstore(uint({st.group(2)}), {st.group(3)});'
            glsl = '\n'.join(lines)
            extra += ['uint dmsh_vbase;   // this workgroup\'s slot of the vertex buffer', 'shared uint dmsh_x[1];   // the first index of its triangles', '',
                      'void dmsh_vstore(uint w, uint v)', '{']
            # a word past the attribute area (no kernel has one) lands on the area's last word, inside the group's slot
            if max_v_d == 32:
                # the stage memory's c * 32 + lane -> the buffer's lane * NCOMP + c
                extra += [f'    uint s = min(w - {outp // 4}u, {nvw - 1}u);',
                          f'    dmsh_mesh_vtx.w[dmsh_vbase + (s & 31u) * {ncomp_d}u + (s >> 5u)] = v;']
            else:
                # the stage memory's (block * NCOMP + c) * 32 + lane -> the buffer's (block * 32 + lane) * NCOMP + c, which is
                # q + lane * NCOMP + block * 31 * NCOMP with q = block * NCOMP + c
                mul, shift = div_const(ncomp_d, nvw // 32)
                blk = f'(q * {mul}u) >> {shift}u' if mul != 1 else f'q >> {shift}u'
                extra += [f'    uint s = min(w - {outp // 4}u, {nvw - 1}u), q = s >> 5u, b = {blk};',
                          f'    dmsh_mesh_vtx.w[dmsh_vbase + q + (s & 31u) * {ncomp_d}u + b * {31 * ncomp_d}u] = v;']
            extra += ['}', '']
            applied.add('direct'); direct = True
        if ('inputs' in modes
                and all(outp <= at(c) for k, c, _, _ in acc if k != 'load')                  # the kernel writes nothing below its outputs
                and all(c == ('const', wg) or inp <= at(c) < outp for k, c, _, _ in acc if k == 'load')):
            lines = glsl.split('\n')
            defs = ssa_defs(glsl)
            def sub(mm):
                c = resolve_word(defs, mm.group(1))
                if c == ('const', wg):
                    return 'dmsh_wg'
                return f'dmsh_in(uint({mm.group(1)}))' if c and inp <= c[1] < outp else mm.group(0)   # a store's own place stays
            for k, c, n, _ in acc:
                if k == 'load':
                    lines[n] = re.sub(r'\b' + re.escape(mem) + r'\[([^\[\]]*)\]', sub, lines[n])
            glsl = '\n'.join(lines)
            extra += ['uint dmsh_wg;   // the workgroup\'s number (with a task: its number among its task\'s launches)']
            if i.get('task') and pay_words:
                extra += ['uint dmsh_at;   // the place of its payload', '', 'uint dmsh_in(uint w)   // a word of the input area', '{',
                          f'    uint q = w - {inp // 4}u, p = dmsh_mesh_in.w[dmsh_at + min(q, {pay_words - 1}u)];',
                          f'    return q == 0u ? dmsh_wg : (q < {pay_words}u ? p : 0u);   // word 0 is the launch\'s number, past the payload the cleared memory',
                          '}', '']
            elif i.get('task'):
                extra += ['', 'uint dmsh_in(uint w)', '{', f'    return w == {inp // 4}u ? dmsh_wg : 0u;', '}', '']
            else:
                extra += ['', 'uint dmsh_in(uint w)', '{', '    return 0u;   // no task: the input area is the cleared stage memory', '}', '']
            applied.add('inputs'); inputs = True
        # not when direct was asked for and refused: the attribute words stay in the stage memory then, and the byte array
        # only adds to it (3 kernels: 4436 -> 5224 bytes, oot-a103)
        if ('bytes' in modes and (direct or 'direct' not in modes) and prim_private and pb > 0 and len(helpers) == 1
                and helper_is_cas8(helpers[0], mem)):
            h = HELPER.search(glsl)
            name, a, v = h.group(1), h.group(3), h.group(4)
            body = [f'shared uint dmsh_pb[{pb}];   // the primitive area, a word a byte', '',
                    # an address outside the primitive area (no kernel has one) lands on the area's last byte, inside the array
                    f'void {name}(int {a}, int {v})', '{', f'    dmsh_pb[min(uint({a}) - {prim}u, {pb - 1}u)] = uint({v}) & 255u;', '}', '']
            glsl = glsl[:h.start()] + '\n'.join(body) + glsl[h.end():]
            applied.add('bytes'); bytes_mode = True
        if direct and bytes_mode:
            # nothing is left above the inputs (the attribute words go to the vertex buffer, the primitive area is dmsh_pb),
            # and with inputs nothing at all: the array is as short as that. The size is what a workgroup costs.
            left = outp // 4
            if inputs and not re.search(r'\b' + re.escape(mem) + r'\[', re.sub(r'^shared uint ' + re.escape(mem) + r'\[\d+\];$', '', glsl, flags=re.M)):
                left = 1
            glsl = re.sub(r'^shared uint ' + re.escape(mem) + r'\[\d+\];$', f'shared uint {mem}[{left}];', glsl, count=1, flags=re.M)
            words = left

    payload_mode = False
    if i['kind'] == 'ts' and 'payload' in modes:
        r = task_payload(glsl, mem, i, pay_words)
        if r is not None:
            glsl, extra = r
            applied.add('payload'); payload_mode = True

    b0 = max((int(x) for x in BINDING.findall(glsl)), default=-1) + 1
    binds, decl = [], []
    ctl = lambda k: f'dmsh_ctl.c[{k}]'
    head = [
        'void main()', '{',
        '    uint dmsh_t = gl_LocalInvocationID.x, dmsh_g = gl_WorkGroupID.x;',
    ]
    if not (inputs or bytes_mode or direct):
        clear = [
            f'    for (uint w = dmsh_t; w < {words}u; w += {T}) {mem}[w] = 0u;   // the stage memory starts at zero',
            '    barrier();',
        ]
    else:
        # what must start at zero: the attribute words always (the copy-out reads them); the words below them unless the
        # kernel's reads of them were taken to their sources (the dense output sets its words 1 .. 3 itself); the primitive
        # area unless it is dmsh_pb
        ranges = []
        for lo, hi in ([] if inputs else [(0, min(outp // 4, words))]) + ([] if direct else [(outp // 4, prim // 4)]) + ([] if bytes_mode else [(prim // 4, words)]):
            if ranges and ranges[-1][1] == lo:
                ranges[-1] = (ranges[-1][0], hi)
            elif hi > lo:
                ranges.append((lo, hi))
        clear = [f'    for (uint w = {f"{lo}u + " if lo else ""}dmsh_t; w < {hi}u; w += {T}) {mem}[w] = 0u;   // the stage memory starts at zero' for lo, hi in ranges]
        if bytes_mode:
            clear.append(f'    for (uint w = dmsh_t; w < {pb}u; w += {T}) dmsh_pb[w] = 0u;')
        if direct and not whole and 'noclear' not in modes:
            clear += [f'    for (uint w = dmsh_t; w < {max(i["max_v"], 1) * len(i["vertex"])}u; w += {T}) dmsh_mesh_vtx.w[dmsh_vbase + w] = 0u;   // as the cleared stage memory gave',
                      '    memoryBarrierBuffer();']
        elif direct and not whole:
            applied.add('noclear')   # (timing experiments only: a word the kernel leaves keeps what the buffer held)
        clear.append('    barrier();')
    if i['kind'] == 'ts':
        decl.append(f'layout(set = 0, binding = {b0}, std430) buffer dmsh_mesh_in_t {{ uint w[]; }} dmsh_mesh_in;')
        binds.append((b0, STORAGE_BUFFER, RB_MESH_IN, 1)); b0 += 1
        decl.append(f'layout(set = 0, binding = {b0}, std430) buffer dmsh_ctl_t {{ uint c[{C_WORDS}]; }} dmsh_ctl;')
        binds.append((b0, STORAGE_BUFFER, RB_MESH_PAR, 1))
        table = [
            f'    for (uint j = dmsh_t; j < dmsh_n; j += {T})',
            '    {',
            '        uint gi = dmsh_base + j;',
            f'        if (gi < {ctl(C_CAP)}) {{ dmsh_mesh_in.w[2u * gi] = dmsh_pay; dmsh_mesh_in.w[2u * gi + 1u] = j; }}',
            '    }',
            '}', '']
        if payload_mode:
            # the kernel's payload stores go to the buffer themselves (task_payload): no stage memory, no copy-out
            body = head + [
                f'    if (dmsh_g >= {ctl(C_TCAP)}) return;   // past the payload buffer',
                f'    dmsh_wg = {ctl(C_FIRST)} + dmsh_g;   // each thread keeps the workgroup\'s number itself',
                f'    dmsh_pay = 2u * {ctl(C_CAP)} + dmsh_g * {pay_words}u;   // this task\'s payload, after the mesh groups\' table',
                f'    for (uint w = dmsh_t; w < {pay_words}u; w += {T}) dmsh_mesh_in.w[dmsh_pay + w] = 0u;   // as the cleared stage memory gave',
                '    if (dmsh_t == 0u) dmsh_x[1] = 0u;',
                '    memoryBarrierBuffer();',
                '    barrier();',
                '    dmsh_kernel();',
                '    barrier();',
                f'    uint dmsh_n = min(dmsh_x[1], {ctl(C_CAP)});   // the mesh workgroups it launches: the payload\'s word 1',
                '    if (dmsh_t == 0u)',
                '    {',
                '        dmsh_mesh_in.w[dmsh_pay + 1u] = dmsh_x[1];   // the one payload word that stayed shared',
                f'        dmsh_x[0] = atomicAdd({ctl(C_DISP)}, dmsh_n);   // their numbers in the mesh dispatch',
                '    }',
                '    barrier();',
                '    uint dmsh_base = dmsh_x[0];'] + table
        else:
            body = head + [f'    if (dmsh_g >= {ctl(C_TCAP)}) return;   // past the payload buffer'] + clear + [
                f'    if (dmsh_t == 0u) {mem}[{wg // 4}] = {ctl(C_FIRST)} + dmsh_g;   // the workgroup\'s number',
                '    barrier();',
                '    dmsh_kernel();',
                '    barrier();',
                f'    uint dmsh_pay = 2u * {ctl(C_CAP)} + dmsh_g * {pay_words}u;   // this task\'s payload, after the mesh groups\' table',
                f'    for (uint w = dmsh_t; w < {pay_words}u; w += {T}) dmsh_mesh_in.w[dmsh_pay + w] = {mem}[{outp // 4}u + w];',
                f'    uint dmsh_n = min({mem}[{outp // 4 + 1}], {ctl(C_CAP)});   // the mesh workgroups it launches: the payload\'s word 1',
                f'    if (dmsh_t == 0u) {mem}[{wg // 4}] = atomicAdd({ctl(C_DISP)}, dmsh_n);   // their numbers in the mesh dispatch',
                '    barrier();',
                f'    uint dmsh_base = {mem}[{wg // 4}];'] + table
    else:
        ncomp, max_v, max_p, verts = len(i['vertex']), max(i['max_v'], 1), max(i['max_p'], 1), i.get('verts', 3)
        if verts != 3:
            raise ValueError(f'a primitive of {verts} vertices (only triangles yet)')
        if not ncomp:
            raise ValueError('no vertex components')
        if i.get('task'):
            decl.append(f'layout(set = 0, binding = {b0}, std430) readonly buffer dmsh_mesh_in_t {{ uint w[]; }} dmsh_mesh_in;')
            binds.append((b0, STORAGE_BUFFER, RB_MESH_IN, 0)); b0 += 1
        decl.append(f'layout(set = 0, binding = {b0}, std430) buffer dmsh_mesh_vtx_t {{ uint w[]; }} dmsh_mesh_vtx;')
        binds.append((b0, STORAGE_BUFFER, RB_MESH_VTX, 1)); b0 += 1
        decl.append(f'layout(set = 0, binding = {b0}, std430) buffer dmsh_mesh_idx_t {{ uint w[]; }} dmsh_mesh_idx;')
        binds.append((b0, STORAGE_BUFFER, RB_MESH_IDX, 1)); b0 += 1
        decl.append(f'layout(set = 0, binding = {b0}, std430) buffer dmsh_ctl_t {{ uint c[{C_WORDS}]; }} dmsh_ctl;')
        binds.append((b0, STORAGE_BUFFER, RB_MESH_PAR, 1))
        body = head + [f'    if (dmsh_g >= {ctl(C_CAP)}) return;   // past the draw\'s vertex and index buffers']
        if direct:
            body.append(f'    dmsh_vbase = dmsh_g * {max_v * ncomp}u;')
        body += clear
        if inputs:
            # each thread keeps the workgroup's number (and its payload's place) itself: nothing to share, no barrier
            if i.get('task'):
                body += (['    dmsh_at = dmsh_mesh_in.w[2u * dmsh_g];'] if pay_words else []) + ['    dmsh_wg = dmsh_mesh_in.w[2u * dmsh_g + 1u];']
            else:
                body += [f'    dmsh_wg = {ctl(C_FIRST)} + dmsh_g;']
        elif i.get('task'):
            # the table the task step filled: word 2g the place of this group's payload, word 2g + 1 its number among its
            # task's launches, which the hardware puts into the workgroup's number and the payload's word 0
            body += [
                '    uint dmsh_at = dmsh_mesh_in.w[2u * dmsh_g];',
                f'    for (uint w = dmsh_t; w < {pay_words}u; w += {T}) {mem}[{inp // 4}u + w] = dmsh_mesh_in.w[dmsh_at + w];',
                '    barrier();',
                f'    if (dmsh_t == 0u) {{ uint j = dmsh_mesh_in.w[2u * dmsh_g + 1u]; {mem}[{wg // 4}] = j; {mem}[{inp // 4}] = j; }}',
                '    barrier();',
            ]
        else:
            body += [f'    if (dmsh_t == 0u) {mem}[{wg // 4}] = {ctl(C_FIRST)} + dmsh_g;   // the workgroup\'s number', '    barrier();']
        # an index byte of primitive p: a word of dmsh_pb, or the byte of the stage memory's word
        index = 'dmsh_pb[4u + p * 3u + k]' if bytes_mode else f'(({mem}[at >> 2] >> (8u * (at & 3u))) & 255u)'
        at_line = [] if bytes_mode else [f'            uint at = {prim + 4}u + p * 3u + k;']
        count = 'dmsh_pb[3]' if bytes_mode else f'{mem}[{prim // 4}] >> 24'
        if direct:
            # the vertices are where the kernel put them (the workgroup's slot); its triangles take their indices
            body += [
                '    dmsh_kernel();',
                '    barrier();',
                f'    uint dmsh_count = min({count}, {max_p}u);   // the primitive count: the byte at the primitive area + 3',
                f'    if (dmsh_t == 0u) dmsh_x[0] = atomicAdd({ctl(C_DRAW)}, dmsh_count * 3u);   // its first index; the counter is the draw\'s index count',
                '    barrier();',
                f'    uint dmsh_v0 = dmsh_g * {max_v}u, dmsh_i0 = dmsh_x[0];',
                f'    for (uint p = dmsh_t; p < dmsh_count; p += {T})',
                '        for (uint k = 0u; k < 3u; k++)',
                '        {'] + at_line + [
                f'            dmsh_mesh_idx.w[dmsh_i0 + p * 3u + k] = dmsh_v0 + {index};',
                '        }',
                '}', '']
        else:
            body += [
                '    dmsh_kernel();',
                '    barrier();',
                f'    uint dmsh_count = min({count}, {max_p}u);   // the primitive count: the byte at the primitive area + 3',
                '    // the vertices its primitives use: the highest index + 1 (each thread over its primitives, then the most of all).',
                '    // Words 1 .. 3 of the stage memory are free (the workgroup\'s number is word 0, the input starts at word 4)',
                '    uint dmsh_top = 0u;',
                f'    for (uint p = dmsh_t; p < dmsh_count; p += {T})',
                '        for (uint k = 0u; k < 3u; k++)',
                '        {'] + at_line + [
                f'            dmsh_top = max(dmsh_top, {index} + 1u);',
                '        }',
                f'    if (dmsh_t == 0u) {mem}[1] = 0u;',
                '    barrier();',
                f'    atomicMax({mem}[1], dmsh_top);',
                '    barrier();',
                '    if (dmsh_t == 0u)',
                '    {',
                f'        uint used = min({mem}[1], {max_v}u);',
                f'        {mem}[1] = atomicAdd({ctl(C_VERT)}, used);                 // its first vertex in the draw\'s vertex buffer',
                f'        {mem}[2] = atomicAdd({ctl(C_DRAW)}, dmsh_count * 3u);      // its first index; the counter is the draw\'s index count',
                f'        {mem}[3] = used;',
                '    }',
                '    barrier();',
                f'    uint dmsh_v0 = {mem}[1], dmsh_i0 = {mem}[2], dmsh_used = {mem}[3];',
                f'    for (uint v = dmsh_t; v < dmsh_used; v += {T})',
                f'        for (uint c = 0u; c < {ncomp}u; c++)',
                f'            dmsh_mesh_vtx.w[(dmsh_v0 + v) * {ncomp}u + c] = {mem}[{outp // 4}u + ((v >> 5) * {ncomp}u + c) * 32u + (v & 31u)];',
                f'    for (uint p = dmsh_t; p < dmsh_count; p += {T})',
                '        for (uint k = 0u; k < 3u; k++)',
                '        {'] + at_line + [
                f'            dmsh_mesh_idx.w[dmsh_i0 + p * 3u + k] = dmsh_v0 + {index};',
                '        }',
                '}', '']
    text = MAIN.sub('void dmsh_kernel()\n{', glsl)
    at = text.index('void dmsh_kernel()')
    # the new buffers go before the first function (after the kernel's own declarations)
    first_fn = re.search(r'^\w[\w ]*\s+\w+\([^;{]*\)\n\{', text, re.M)
    cut = first_fn.start() if first_fn else at
    sh = text.find('shared uint dmsh_pb[')   # declared with its helper, which is the first function
    if sh != -1 and sh < cut:
        cut = sh
    text = text[:cut] + '\n'.join(decl + ([''] + extra if extra else [])) + '\n\n' + text[cut:] + '\n' + '\n'.join(body)
    return text, binds, applied


def mesh_line(i):
    """The numbers the replay needs (vk_shader.c): the NVN2 stage (6 task, 7 mesh), whether a mesh shader has a task, its
    most vertices and primitives, a vertex's words, the payload's words, a primitive's vertices."""
    if i['kind'] == 'ts':
        return 'mesh 6 0 0 0 0 %d 0' % ((i['payload'] + 3) // 4)
    return 'mesh 7 %d %d %d %d %d %d' % (1 if i.get('task') else 0, max(i['max_v'], 1), max(i['max_p'], 1), len(i['vertex']),
                                         (i['payload'] + 3) // 4, i.get('verts', 3))


def vertex_shader(i):
    comps = i['vertex']   # [['pos' | n | 'psiz', c], ...] in the order of the attribute area
    ncomp = len(comps)
    def word(n, c, default):
        for k, (an, ac) in enumerate(comps):
            if an == n and ac == c:
                return f'uintBitsToFloat(dmsh_mesh_vtx.w[b + {k}u])'
        return default
    attrs = sorted({n for n, _ in comps if isinstance(n, int)})
    lines = ['#version 450', 'invariant gl_Position;',
             'layout(set = 0, binding = 0, std430) readonly buffer dmsh_mesh_vtx_t { uint w[]; } dmsh_mesh_vtx;']
    lines += [f'layout(location = {n}) out vec4 dmsh_a{n};' for n in attrs]
    lines += ['void main()', '{', f'    uint b = uint(gl_VertexIndex) * {ncomp}u;',
              '    gl_Position = vec4(' + ', '.join(word('pos', c, '1.0' if c == 3 else '0.0') for c in range(4)) + ');',
              '    gl_Position.z = fma(gl_Position.z, 0.5, gl_Position.w * 0.5);   // as a translated vertex shader (depth_minus_w)']
    lines += [f'    dmsh_a{n} = vec4(' + ', '.join(word(n, c, '1.0' if c == 3 else '0.0') for c in range(4)) + ');' for n in attrs]
    lines += ['}', '']
    return '\n'.join(lines)


def compile_module(glsl_path, spv_path, stage, env):
    """glslang, spirv-opt -O, spirv-val; an error text or None."""
    r = run(['glslangValidator', '-V', '--target-env', env, '-S', stage, str(glsl_path), '-o', str(spv_path)])
    if r.returncode:
        return 'glslang: ' + ' | '.join(l for l in r.stdout.splitlines() if 'ERROR' in l)[:200]
    raw = spv_path.with_suffix('.raw.spv'); spv_path.replace(raw)
    if stage == 'comp' and GMEM in ('fastN', 'all'):
        # the raw lookups' miss function stays ONE function (spirv-opt -O inlines every other one); None: the module has none
        patched = gmemfast.dont_inline(raw.read_bytes())
        if patched is not None:
            raw.write_bytes(patched)
    r = run(['spirv-opt', '-O', str(raw), '-o', str(spv_path)])
    raw.unlink()
    if r.returncode:
        return 'spirv-opt: ' + r.stderr.strip()[:200]
    r = run(['spirv-val', '--target-env', 'vulkan1.1' if env == 'vulkan1.1' else 'vulkan1.3', str(spv_path)])
    if r.returncode:
        spv_path.unlink()
        return 'spirv-val: ' + r.stderr.strip()[:200]
    return None


census = {}   # module -> (kind, the modes applied, the stage memory's bytes, the shared memory's bytes after)


def one(key):
    names = sorted(p.name for p in kernels.glob(f'{key}-*.spv'))   # every variant of the kernel (oot_mesh.py texture_variants)
    if not names:
        return key, 'no kernel module'
    for k, name in enumerate(names):
        e = one_variant(key, name, vertex=k == 0)
        if e:
            return key, e
    return key, None


def one_variant(key, name, vertex):
    i = info[key]
    stem = name[:-4]
    spv = kernels / name
    ver = struct.unpack_from('<I', spv.read_bytes(), 4)[0]
    env = 'vulkan1.1' if ver <= 0x10300 else 'vulkan1.2'
    r = run([spirv_cross, '--vulkan-semantics', str(spv)])
    if r.returncode:
        return 'spirv-cross: ' + r.stderr.strip()[:160]
    try:
        glsl = gmem_pass(r.stdout)
        plan = gmemfast.prefetch_last() if GMEM in ('fastI', 'fastN', 'all') else None
        text, binds, applied = wrap_modes(glsl, i)
    except ValueError as e:
        return 'wrap: ' + str(e)
    if plan:
        PREFETCHED[stem] = [len(plan['keys']), len(plan['take']), plan['covered'], plan['sites']]
    g = out / f'{stem}.comp'; g.write_text(text)
    e = compile_module(g, out / name, 'comp', env)
    g.unlink()   # the GLSL is 0.5 MB a kernel; --keep-glsl in oot_mesh.py is not there: rerun spirv-cross to look at one
    if e:
        return e
    # what a workgroup's shared memory is now (bytes; an array of one word that nothing uses is gone after spirv-opt);
    # only of modules that compiled
    census[stem] = (i['kind'], ','.join(sorted(applied)) or 'none', i['shared'],
                    4 * sum(int(n) for n in re.findall(r'(?m)^shared uint \w+\[(\d+)\];', text) if int(n) > 1))
    vk = (kernels / f'{stem}.vk').read_text().rstrip('\n').split('\n')
    vk += ['b %d 1 %d %d -1 -1 0 %d 1 0 0' % (b, dtype, kind, rw) for b, dtype, kind, rw in binds]
    vk.append(mesh_line(i))
    (out / f'{stem}.vk').write_text('\n'.join(vk) + '\n')
    vv = kernels / f'{key}.vv'
    if vv.is_file():
        (out / f'{key}.vv').write_text(vv.read_text())
    if i['kind'] != 'ms' or not vertex:
        return None
    # the vertex shader of its draws
    v = out / f'{key}-meshvs.vert'; v.write_text(vertex_shader(i))
    e = compile_module(v, out / f'{key}-meshvs.spv', 'vert', 'vulkan1.1')
    v.unlink()
    if e:
        return e + ' (vertex shader)'
    (out / f'{key}-meshvs.vk').write_text('vk 1\nstage 0\nlocal 1 1 1\nc1 \nvin \nfout \ndepth 0\n'
                                         'b 0 1 %d %d -1 -1 0 0 1 0 0\n' % (STORAGE_BUFFER, RB_MESH_VTX))
    return None


def main():
    global kernels, out, info, spirv_cross, MODES
    kernels, out, info_path, spirv_cross = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    jobs = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    names = [x.strip() for x in (sys.argv[6] if len(sys.argv) > 6 else DEFAULT_MODES).split(',') if x.strip()]
    if [x for x in names if x not in KNOWN_MODES]:
        sys.exit('unknown mode(s) %s; known: %s' % (', '.join(x for x in names if x not in KNOWN_MODES), ', '.join(sorted(KNOWN_MODES))))
    if 'whole' in names and 'direct' not in names:
        sys.exit("the mode 'whole' is a part of 'direct': name both")
    MODES = frozenset(x for x in names if x != 'none')
    info = json.loads(info_path.read_text())
    census.clear()
    PREFETCHED.clear()
    out.mkdir(parents=True, exist_ok=True)
    keys = sorted(info)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(one, keys))
    failed = [(k, e) for k, e in results if e]
    report = {'done': [k for k, e in results if not e], 'failed': failed, 'census': census}
    pmode = gmemfast.prefetch_setting()
    if pmode != '0':
        report['prefetch'] = {s: PREFETCHED[s] for s in sorted(PREFETCHED) if s in census}
    (out / 'wrapped.json').write_text(json.dumps(report, indent=1))
    kinds = {k: sum(1 for key, e in results if not e and info[key]['kind'] == k) for k in ('ts', 'ms')}
    print(f'wrapped {kinds["ts"]} task and {kinds["ms"]} mesh kernels, {len(failed)} failed', flush=True)
    if pmode != '0':
        p = report['prefetch']
        print(f'  prefetch ({pmode}): {len(p)} modules, {sum(v[0] for v in p.values())} pointers, {sum(v[1] for v in p.values())} run starts '
              f'take an entry, {sum(v[2] for v in p.values())} of {sum(v[3] for v in p.values())} sites read through them', flush=True)
    errs = {}
    for k, e in failed:
        errs.setdefault(e[:120], []).append(k)
    for e, ks in sorted(errs.items(), key=lambda kv: -len(kv[1]))[:12]:
        print(f'  {len(ks):4d}  {e}   e.g. {ks[0][:16]}', flush=True)
    # the modes taken and the shared memory of a workgroup, before and after (a workgroup's time on Maleoon follows it)
    tally = {}
    for stem, (kind, modes, before, after) in census.items():
        t = tally.setdefault((kind, modes), [0, 0, 0, 0, None])
        t[0] += 1; t[1] += before; t[2] += after
        if after >= t[3]:
            t[3] = after; t[4] = stem[:16]
    for (kind, modes), (n, before, after, top, who) in sorted(tally.items()):
        print(f'  {kind} {modes:20s} {n:4d} modules: shared memory {before // n:6d} -> {after // n:6d} bytes on average, the most {top} ({who})', flush=True)


if __name__ == '__main__':
    main()
