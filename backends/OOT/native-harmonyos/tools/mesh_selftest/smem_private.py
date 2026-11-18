"""PROTOTYPE (oot-a106, not wired into the seed build): a game compute shader's shared memory that is really each
thread's own memory, made a private array. docs/DESIGN-OOT.md "不适配清单" G3.

Why: on Maleoon a compute workgroup's time follows the declared size of its shared (Workgroup) memory (DESIGN-OOT.md
section 9). The translator puts a console compute shader's local memory into ONE shared array, a slot of S bytes a
thread: every access is word (T * S + K) >> 2 with T the thread's number in its workgroup and 0 <= K < S. Nothing is
shared: no thread reads another thread's slot. Ocarina of Time's 58f47abfa0a84773 (96 threads x 92 bytes = 8.8 KB),
9ed48d1011f1b48b (8 x 8 x 8 threads x 36 bytes = 18.4 KB) and 54 more modules of the seed with 1 KB or more are of this
form (--scan); f8965bff (a 35 x 28 texel tile the threads fill, barrier, then read each other's texels) is NOT and is
refused, as are the modules with two shared arrays or atomics on the array (118 of the seed).

The rewrite, on the SPIR-V module itself (no GLSL round trip):
  the Workgroup variable 'uint[N]' becomes a Private variable 'uint[S / 4]' (zero-initialised: a word read before it is
  stored is 0 instead of undefined), and every access chain into it gets the constant index K / 4.
It is done only when the module shows all of this (analyse(); anything else is refused with the reason):
  - one entry point, GLCompute, its workgroup size a constant: its LocalSize, or a BuiltIn WorkgroupSize made of plain
    constants (that overrides LocalSize); not LocalSizeId or specialisation constants (a107: these were taken as LocalSize);
  - one Workgroup variable, an array of 32-bit words of a constant length; its only uses are access chains with one index;
  - every such pointer is only loaded from and stored through (no atomics, no call arguments, no copies);
  - every index is ShiftRightLogical(base + K, 2) through bitcasts, with ONE base for all of them, K a multiple of 4;
  - base = T * S (or T << n), S a multiple of 4, every K < S;
  - T is a linear form of gl_LocalInvocationID's components that gives every thread of the workgroup (LocalSize) a
    different number, and (T + 1) * S stays inside the array: slots do not overlap, so no thread sees another's words.
Under these conditions the two modules compute the same thing (a private word and the thread's own shared word hold the
same values in the same order), except that the unwritten words are defined now.

usage: smem_private.py <in.spv> <out.spv>          rewrite one module (exit 2 with the reason when refused)
       smem_private.py --scan <folder> [--min N]   which compute modules of a folder would be rewritten (shared >= N bytes)"""
import struct
import sys
from itertools import product
from pathlib import Path

OP = dict(Name=5, EntryPoint=15, ExecutionMode=16, TypeInt=21, TypeArray=28, TypePointer=32, Constant=43, ConstantNull=46, Function=54,
          Variable=59, Load=61, Store=62, AccessChain=65, InBoundsAccessChain=66, Decorate=71, CompositeExtract=81, CopyObject=83,
          Bitcast=124, IAdd=128, IMul=132, ShiftRightLogical=194, ShiftLeftLogical=196)
WORKGROUP, PRIVATE = 4, 6
ANNOTATION = (5, 6, 71, 72, 15)   # OpName, OpMemberName, OpDecorate, OpMemberDecorate, OpEntryPoint


class Refused(Exception):
    pass


def parse(b):
    n = len(b) // 4
    w = list(struct.unpack(f'<{n}I', b[:n * 4]))
    if n < 5 or w[0] != 0x07230203:
        raise Refused('not a SPIR-V module')
    ins, i = [], 5
    while i < n:
        ln = w[i] >> 16
        if not ln or i + ln > n:
            raise Refused('a broken instruction')
        ins.append(w[i:i + ln]); i += ln
    return w[:5], ins


def analyse(head, ins):
    """(index of the variable's instruction, {index of an access chain: K}, S, the id of 'uint', the array's words)"""
    op = lambda x: x[0] & 0xffff
    defs = {}
    for x in ins:
        o = op(x)
        if o in (OP['TypeInt'], OP['TypeArray'], OP['TypePointer']):
            defs[x[1]] = x
        elif o in (OP['Constant'], OP['Variable'], OP['Load'], OP['AccessChain'], OP['InBoundsAccessChain'], OP['CompositeExtract'], OP['CopyObject'],
                   OP['Bitcast'], OP['IAdd'], OP['IMul'], OP['ShiftRightLogical'], OP['ShiftLeftLogical']):
            defs[x[2]] = x
    entries = [x for x in ins if op(x) == OP['EntryPoint']]
    if not any(x[1] == 5 for x in entries):
        raise Refused('not a compute module')
    if len(entries) != 1:   # (a107) another entry point's LocalSize would be the one checked
        raise Refused(f'{len(entries)} entry points')
    local = next((x[3:6] for x in ins if op(x) == OP['ExecutionMode'] and x[1] == entries[0][2] and x[2] == 17), None)
    if not local or any(op(x) == 331 for x in ins):   # OpExecutionModeId (LocalSizeId)
        raise Refused('no LocalSize')
    # (a107) a BuiltIn WorkgroupSize object overrides LocalSize, and a specialisation constant in it is set at pipeline creation
    wgs = [x[1] for x in ins if op(x) == OP['Decorate'] and x[2] == 11 and x[3] == 25]
    if wgs:
        comp = [x for x in ins if op(x) == 44 and x[2] in wgs]   # OpConstantComposite (not OpSpecConstantComposite)
        vals = [defs.get(c) for c in comp[0][3:6]] if len(wgs) == 1 and len(comp) == 1 and len(comp[0]) == 6 else None
        if not vals or any(v is None or op(v) != OP['Constant'] for v in vals):
            raise Refused('the workgroup size is not a constant (BuiltIn WorkgroupSize of specialisation constants)')
        local = [v[3] for v in vals]
    shared = [k for k, x in enumerate(ins) if op(x) == OP['Variable'] and x[3] == WORKGROUP]
    if len(shared) != 1:
        raise Refused(f'{len(shared)} Workgroup variables')
    vi = shared[0]; var = ins[vi][2]
    ptr = defs.get(ins[vi][1]); arr = defs.get(ptr[3]) if ptr else None
    if not arr or op(arr) != OP['TypeArray']:
        raise Refused('the Workgroup variable is no array')
    uint = arr[2]
    if op(defs.get(uint, [0])) != OP['TypeInt'] or defs[uint][2] != 32:
        raise Refused('not an array of 32-bit words')
    if op(defs.get(arr[3], [0])) != OP['Constant']:   # (a107) a specialisation constant was a KeyError
        raise Refused('the array length is not a constant')
    words = defs[arr[3]][3]

    def const(i):
        x = defs.get(i)
        if x is None or op(x) != OP['Constant'] or len(x) != 4:
            return None
        t = defs.get(x[1])
        return x[3] if t is not None and op(t) == OP['TypeInt'] and t[2] == 32 and x[3] < 1 << 31 else None

    def strip(i):
        while i in defs and op(defs[i]) in (OP['Bitcast'], OP['CopyObject']):
            i = defs[i][3]
        return i

    def operands(x):
        """the words of an instruction that can be ids: all of them, less the literals of the instructions the corpus has
        with small literals (an instruction number, a member, a case value). A literal of any other instruction that
        happens to be the number of an id counts as a use of it, which only makes the module refused."""
        o = op(x)
        if o == 12:                 # OpExtInst: type, result, set, the instruction's number, operands
            return x[1:4] + x[5:]
        if o in (81, 61):           # OpCompositeExtract, OpLoad: type, result, composite / pointer, literals
            return x[1:4]
        if o in (82, 79):           # OpCompositeInsert, OpVectorShuffle: type, result, two ids, literals
            return x[1:5]
        if o == 62:                 # OpStore: pointer, object, memory access
            return x[1:3]
        if o == 251:                # OpSwitch: selector, default, (literal, label) ...
            return x[1:3] + x[4::2]
        return x[1:]

    # every use of the variable, and of the pointers into it. Before the first function only a constant expression could
    # name a variable (OpSpecConstantOp); the debug names, decorations and the entry point's list are no uses.
    chains = {}   # index of the access chain -> its result id
    body = next((k for k, x in enumerate(ins) if op(x) == OP['Function']), len(ins))
    if any(op(x) == 52 and var in x[1:] for x in ins[:body]):
        raise Refused('the shared array in a constant expression')
    for k, x in enumerate(ins):
        if k < body or var not in operands(x):
            continue
        if op(x) not in (OP['AccessChain'], OP['InBoundsAccessChain']) or x[3] != var or len(x) != 5 or var in (x[1], x[2], x[4]):
            raise Refused('a use of the shared array that is no access chain with one index')
        chains[k] = x[2]
    if not chains:
        raise Refused('the shared array is not used')
    ids = set(chains.values())
    for k, x in enumerate(ins):
        if k < body or k in chains or not ids.intersection(operands(x)):
            continue
        o = op(x)
        if not ((o == OP['Load'] and x[3] in ids and not ids.intersection(x[1:3])) or (o == OP['Store'] and x[1] in ids and x[2] not in ids)):
            raise Refused('a pointer into the shared array that is not just loaded or stored (an atomic, a call, a copy)')

    # the index of every access chain: ShiftRightLogical(base + K, 2)
    bases, ks = set(), {}
    for k in chains:
        d = defs.get(strip(ins[k][4]))
        if d is None or op(d) != OP['ShiftRightLogical'] or const(d[4]) != 2:
            raise Refused('an index that is not a byte address >> 2')
        x, kk = strip(d[3]), 0
        while x in defs and op(defs[x]) == OP['IAdd']:
            a, b2 = defs[x][3], defs[x][4]
            if const(b2) is not None:
                kk += const(b2); x = strip(a)
            elif const(a) is not None:
                kk += const(a); x = strip(b2)
            else:
                break
        bases.add(x); ks[k] = kk
    if len(bases) != 1:
        raise Refused(f'{len(bases)} different bases of the byte addresses (not one slot a thread)')
    base = defs.get(bases.pop())
    if base is None:
        raise Refused('the base is not understood')
    if op(base) == OP['IMul'] and (const(base[4]) is not None or const(base[3]) is not None):
        size, t = (const(base[4]), base[3]) if const(base[4]) is not None else (const(base[3]), base[4])
    elif op(base) == OP['ShiftLeftLogical'] and const(base[4]) is not None and const(base[4]) < 16:
        size, t = 1 << const(base[4]), base[3]
    else:
        raise Refused('the base is not a thread number times a slot size')
    if size % 4 or not size or any(kk % 4 or not 0 <= kk < size for kk in ks.values()):
        raise Refused('an offset outside the slot, or a slot or offset that is not whole words')

    # the thread number: a linear form of gl_LocalInvocationID's components
    builtin = {x[1] for x in ins if op(x) == OP['Decorate'] and x[2] == 11 and x[3] == 27}

    def lin(i, depth=0):
        i = strip(i)
        c = const(i)
        if c is not None:
            return (0, 0, 0, c)
        x = defs.get(i)
        if x is None or depth > 24:
            return None
        o = op(x)
        if o == OP['IAdd']:
            a, b2 = lin(x[3], depth + 1), lin(x[4], depth + 1)
            return tuple(p + q for p, q in zip(a, b2)) if a and b2 else None
        if o == OP['IMul']:
            for a, b2 in ((x[3], x[4]), (x[4], x[3])):
                if const(strip(b2)) is not None:
                    v = lin(a, depth + 1)
                    return tuple(p * const(strip(b2)) for p in v) if v else None
            return None
        if o == OP['ShiftLeftLogical'] and const(strip(x[4])) is not None and const(strip(x[4])) < 31:
            v = lin(x[3], depth + 1)
            return tuple(p << const(strip(x[4])) for p in v) if v else None
        if o == OP['Load']:
            p = defs.get(x[3])
            if p is not None and op(p) in (OP['AccessChain'], OP['InBoundsAccessChain']) and p[3] in builtin and len(p) == 5 and const(p[4]) in (0, 1, 2):
                return tuple(1 if a == const(p[4]) else 0 for a in range(3)) + (0,)
            return None
        if o == OP['CompositeExtract'] and len(x) == 5 and x[4] in (0, 1, 2):
            p = defs.get(strip(x[3]))
            if p is not None and op(p) == OP['Load'] and p[3] in builtin:
                return tuple(1 if a == x[4] else 0 for a in range(3)) + (0,)
        return None

    f = lin(t)
    if f is None:
        raise Refused('the thread number is no linear form of gl_LocalInvocationID')
    slots = [f[0] * x + f[1] * y + f[2] * z + f[3] for x, y, z in product(range(local[0]), range(local[1]), range(local[2]))]
    if len(set(slots)) != len(slots) or min(slots) < 0 or (max(slots) + 1) * size > words * 4:
        raise Refused('two threads share a slot, or a slot lies outside the array')
    return vi, {k: kk // 4 for k, kk in ks.items()}, size, uint, words


def rewrite(b):
    """(the rewritten module's bytes, a line of what was done); Refused with the reason"""
    head, ins = parse(b)
    vi, ks, size, uint, words = analyse(head, ins)
    op = lambda x: x[0] & 0xffff
    bound = head[3]
    alloc = []

    def make(opcode, *operands):
        return [(len(operands) + 1) << 16 | opcode, *operands]

    def fresh():
        alloc.append(None)
        return bound + len(alloc) - 1

    var = ins[vi][2]
    extra = []
    c_len = fresh(); extra.append(make(OP['Constant'], uint, c_len, size // 4))
    t_arr = fresh(); extra.append(make(OP['TypeArray'], t_arr, uint, c_len))
    t_parr = fresh(); extra.append(make(OP['TypePointer'], t_parr, PRIVATE, t_arr))
    # a pointer type must not be declared twice: the module's own 'pointer to a private uint', if it has one before the variable
    t_puint = next((x[1] for x in ins[:vi] if op(x) == OP['TypePointer'] and x[2] == PRIVATE and x[3] == uint), None)
    later = next((k for k, x in enumerate(ins) if k > vi and op(x) == OP['TypePointer'] and x[2] == PRIVATE and x[3] == uint), None)
    if t_puint is None and later is not None:   # declared after the variable: move it up
        t_puint = ins[later][1]; extra.append(ins[later])
    elif t_puint is None:
        t_puint = fresh(); extra.append(make(OP['TypePointer'], t_puint, PRIVATE, uint))
    null = fresh(); extra.append(make(OP['ConstantNull'], t_arr, null))
    consts = {}
    for k in sorted(set(ks.values())):
        consts[k] = fresh(); extra.append(make(OP['Constant'], uint, consts[k], k))
    out = []
    for k, x in enumerate(ins):
        if k == vi:
            out += extra
            out.append(make(OP['Variable'], t_parr, var, PRIVATE, null))
        elif k in ks:
            out.append(make(x[0] & 0xffff, t_puint, x[2], var, consts[ks[k]]))
        elif k == later and any(e is x for e in extra):
            continue
        else:
            out.append(x)
    head = head[:3] + [bound + len(alloc)] + head[4:]
    flat = head + [w for x in out for w in x]
    return struct.pack(f'<{len(flat)}I', *flat), f'{words * 4} bytes of shared memory -> a private array of {size // 4} words a thread ({len(ks)} accesses)'


def main():
    if sys.argv[1] == '--scan':
        folder = Path(sys.argv[2])
        least = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 1024
        done, why = [], {}
        for p in sorted(folder.glob('*.spv')):
            b = p.read_bytes()
            try:
                head, ins = parse(b)
                sh = [x for x in ins if x[0] & 0xffff == OP['Variable'] and x[3] == WORKGROUP]
                if not sh or not any(x[0] & 0xffff == OP['EntryPoint'] and x[1] == 5 for x in ins):
                    continue
                vk = p.with_suffix('.vk')
                if vk.is_file() and any(l.startswith('mesh ') for l in vk.read_text().splitlines()):
                    continue   # a task / mesh kernel: the wrapper's
                vi, ks, size, uint, words = analyse(head, ins)
                if words * 4 >= least:
                    done.append((words * 4, size, p.name[:16]))
            except Refused as e:
                why.setdefault(str(e), []).append(p.name[:16])
        print(f'{len(done)} compute modules with {least} bytes or more of shared memory that is a slot a thread (private after the rewrite):')
        for total, size, name in sorted(done, reverse=True):
            print(f'  {name}  {total:6d} bytes = {total // size} threads x {size} bytes')
        print('refused:')
        for e, names in sorted(why.items(), key=lambda kv: -len(kv[1])):
            print(f'  {len(names):4d}  {e}   e.g. {" ".join(names[:3])}')
        return 0
    try:
        out, what = rewrite(Path(sys.argv[1]).read_bytes())
    except Refused as e:
        print(f'refused: {e}')
        return 2
    Path(sys.argv[2]).write_bytes(out)
    print(what)
    return 0


if __name__ == '__main__':
    sys.exit(main())
