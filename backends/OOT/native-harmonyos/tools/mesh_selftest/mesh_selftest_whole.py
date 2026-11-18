"""Self-test of the mesh wrapper's mode 'whole' (no clear of the vertex slot) and of the area classes, on lavapipe.

mesh_selftest_modes.py only has kernels that store every component of every vertex, so nothing in it reads a word the
clear would have written. Here: kernels that do NOT store a vertex whole (the mode must be refused, and the triangles
must equal the wrapper without modes), kernels that store a vertex through two registers of one value (the mode must be
taken), and addresses the wrapper must class by their whole sum. From the review of oot-a102 (workflow wf_561cdcac-bdc).

  docker run --rm -v <port>:/repo mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/mesh_selftest_whole.py [case ...]

Each case is run with no modes (the reference), 'all three' (bytes, inputs, direct: the slot is cleared) and the default
modes (with whole). The vertex buffer is filled with 0xcdcdcdcd before the run: a word the clear should have written
and no thread stored shows as a difference from the reference."""
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import mesh_selftest_modes as st   # noqa: E402
import mesh_wrap_in_container as mw   # noqa: E402

PAYLOAD = 128   # as the corpus's mesh kernels without a task: an input area nothing fills

# case: (the mode 'whole' with the default modes: True taken / False refused / None either, the triangles must equal the
# reference, what the case is)
CASES = {
    'plain':    (True,  True, 'every vertex stored whole through one register'),
    'twonames': (True,  True, 'a vertex stored through two registers of one value (the 53 large kernels of the corpus)'),
    'partial':  (False, True, 'the last component only of even vertices'),
    'ret':      (False, True, 'a return between two components'),
    'ifelse':   (False, True, 'a component stored in both arms of an if'),
    'twovals':  (False, True, 'two registers of different values, half of the components each'),
    'reassign': (False, True, 'one variable assigned twice between two registers written the same way'),
    'const':    (False, True, 'a store at a constant address besides the vertex stores'),
    'outvert':  (False, True, 'the vertex number is a local set through an out parameter, twice'),
    'outreg':   (False, True, 'the register is changed through an out parameter between two components'),
    'label':    (False, True, 'a case label between the stores of a vertex (not a form SPIRV-Cross writes)'),
    'ifline':   (False, True, 'an if without braces around one store (not a form SPIRV-Cross writes)'),
    'count255': (None,  True, 'a primitive count over the capacity'),
    'loadattr': (None,  True, 'an attribute word read back'),
    'viainput': (None,  True, 'an attribute word read back through a register that carries the area offset'),
    'stale':    (None,  None, 'a triangle names a vertex no thread stored (outside the premise of whole: shown, not judged)'),
}


EXTRA_FN = """void dmsh_other(int _9001, out int _9003)
{
    _9003 = _9001;
}

"""


def kernel_v(max_v, max_p, ncomp, variant):
    lay, size = st.layout(max_v, max_p, ncomp, PAYLOAD); inp, out, prim = lay[1], lay[2], lay[3]
    half = max(ncomp // 2, 1)
    g = st.Gen()
    t = g.t('int', 'int(gl_LocalInvocationID.x)')
    n = g.t('int', 'int(_33[0])')
    g.raw(f'    if (({n} % 5) == 0)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    np_ = g.t('int', f'5 + ({n} * 7) % {max_p - 4}')
    nv = g.t('int', f'3 + ({n} * 5) % {max_v - 2}')

    def chain(v):
        hi = g.t('int', f'int(uint({v}) >> uint(5)) * {ncomp * 128}')
        lo = g.t('int', f'({v} & 31) << 2')
        return g.t('int', f'{hi} + {lo}')

    for j in range((max_v + 31) // 32):
        vexpr = f'min({nv} - 1, {t} + {32 * j})'
        v = g.t('int', vexpr)
        if variant == 'reassign':
            m = g.t('int', '0'); g.raw(f'    {m} = {v};')
            base = chain(m)
        elif variant == 'outvert':
            g.n += 2; ov = f'_{g.n}'; g.raw(f'    int {ov} = 0;'); g.raw(f'    dmsh_other({v}, {ov});')
            base = chain(ov)
        else:
            base = chain(v)
        base2 = base
        if variant in ('label', 'ifline'):
            words = []   # the index words and the values first: declarations do not go between case labels
            for c in range(ncomp):
                a = g.t('int', f'{base} + {out + c * 128}')
                words.append((g.t('uint', f'uint(int(uint({a}) >> uint(2)))'), g.t('int', f'{n} * 10000 + {v} * 16 + {c}')))
            if variant == 'label':   # odd vertices enter at the second label: only their second half is stored
                sel = g.t('int', f'{v} & 1')
                g.raw(f'    switch ({sel})'); g.raw('    {'); g.raw('        case 0:')
                for c in range(half):
                    g.raw(f'            _33[int({words[c][0]})] = uint({words[c][1]});')
                g.raw('        default:')
                for c in range(half, ncomp):
                    g.raw(f'            _33[int({words[c][0]})] = uint({words[c][1]});')
                g.raw('    }')
            else:
                for c in range(ncomp):
                    if c == ncomp - 1:
                        cond = g.t('bool', f'({v} & 1) == 0')
                        g.raw(f'    if ({cond})')
                        g.raw(f'        _33[int({words[c][0]})] = uint({words[c][1]});')
                    else:
                        g.raw(f'    _33[int({words[c][0]})] = uint({words[c][1]});')
            continue
        for c in range(ncomp):
            if c == half:   # the second register of the two-register cases
                if variant == 'twonames':
                    base2 = chain(g.t('int', vexpr))
                elif variant == 'twovals':
                    base2 = chain(g.t('int', f'{vexpr} ^ 1'))
                elif variant == 'reassign':
                    g.raw(f'    {m} = {v} ^ 1;')
                    base2 = chain(m)
                elif variant == 'outvert':
                    o2 = g.t('int', f'{vexpr} ^ 1'); g.raw(f'    dmsh_other({o2}, {ov});')
                    base2 = chain(ov)
                elif variant == 'outreg':   # one name, another value: base2 stays base
                    b2 = chain(g.t('int', f'{vexpr} ^ 1')); g.raw(f'    dmsh_other({b2}, {base});')
            b = base2 if c >= half else base
            if variant == 'partial' and c == ncomp - 1:
                g.raw(f'    if (({v} & 1) == 0)'); g.raw('    {')
                val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}', '        ')
                g.store(b, out + c * 128, f'uint({val})', '        ')
                g.raw('    }')
                continue
            if variant == 'ret' and c == 2 and j == 0:
                g.raw(f'    if ({t} >= 16)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
            if variant == 'ifelse' and c == 1:
                g.raw(f'    if (({t} & 1) == 0)'); g.raw('    {')
                val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}', '        ')
                g.store(b, out + c * 128, f'uint({val})', '        ')
                g.raw('    }'); g.raw('    else'); g.raw('    {')
                val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}', '        ')
                g.store(b, out + c * 128, f'uint({val})', '        ')
                g.raw('    }')
                continue
            val = g.t('int', f'{n} * 10000 + {v} * 16 + {c}')
            if variant == 'loadattr' and c == 1:
                back = g.load(b, out)                    # component 0 of this vertex, read back
                val = g.t('int', f'{back} + 1')
            if variant == 'viainput' and c == 1:
                r2 = g.t('int', f'{b} + {out - inp}')    # a register that carries the attribute area's offset itself
                back = g.load(r2, inp)                   # ... + K with K the INPUT area's: the same word as above
                val = g.t('int', f'{back} + 1')
            g.store(b, out + c * 128, f'uint({val})')
        if variant == 'const' and j == 0:
            g.raw(f'    if ({t} == 31)'); g.raw('    {')
            g.raw(f'        _33[{out // 4 + (max_v - 1) % 32 + ((max_v - 1) // 32 * ncomp + 1) * 32}] = 4242u;')   # the last vertex's component 1
            g.raw('    }')
    g.raw(f'    if ({t} == 3)'); g.raw('    {')
    a = g.t('int', str(prim + 3), '        '); c = g.t('int', '255' if variant == 'count255' else np_, '        ')
    g.raw(f'        dmsh_shared_store8_0({a}, {c});'); g.raw('    }')
    for j in range((max_p + 31) // 32):
        p = g.t('int', f'min({np_} - 1, {t} + {32 * j})')
        base = g.t('int', f'({p} << 1) + {p}')
        for k in range(3):
            if variant == 'stale' and k == 0:
                g.store8(base, prim + 4 + k, f'min({nv}, {max_v - 1})')   # the vertex after the last one stored
                continue
            g.store8(base, prim + 4 + k, f'(({p} * {(5, 11, 3)[k]} + {k}) % {nv}){("", " - 256", f" + (({p} & 3) << 8)")[k]}')
    g.raw(f'    if ({t} > 20)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    g.t('int', f'{t} + 1')
    text = g.text((size + 3) // 4)
    return (text.replace('\nvoid main()', '\n' + EXTRA_FN + 'void main()', 1) if 'dmsh_other' in text else text), lay, size


def run_one(tmp, vkrun, name, text, info, modes, dims, first, groups, cap):
    max_v, max_p, ncomp = dims
    d = tmp / f'{name}-{"-".join(sorted(modes)) or "none"}'; d.mkdir()
    by, applied = st.build(mw.wrap_modes, d, 'mesh', text, info, modes)
    st.DIRECT = 'direct' in applied
    st.ctl_file(d / 'ctl.bin', first, cap, 0, 0, groups)
    st.run([str(vkrun), str(d / 'mesh.spv'), str(groups), f'{by[mw.RB_MESH_VTX]}:s:{cap * max_v * ncomp * 4}',
            f'{by[mw.RB_MESH_IDX]}:s:{cap * max_p * 3 * 4}', f'{by[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}'], cwd=d)
    vtx, idx, ctl = (st.words_of(d / f'out_{by[k]}.bin') for k in (mw.RB_MESH_VTX, mw.RB_MESH_IDX, mw.RB_MESH_PAR))
    got = st.canon(vtx, idx, ctl, ncomp)
    ni = min(ctl[mw.C_DRAW], len(idx))
    named = set(idx[:ni])
    stale = sum(1 for x in named if x * ncomp + ncomp <= len(vtx) and any(w == st.FILL for w in vtx[x * ncomp:(x + 1) * ncomp]))
    return applied, got, stale, ctl[mw.C_DRAW]


def differential(tmp, vkrun, variant, dims, first=3, groups=24, cap=24):
    max_v, max_p, ncomp = dims
    whole_want, same_want, what = CASES[variant]
    text, lay, size = kernel_v(max_v, max_p, ncomp, variant)
    info = dict(kind='ms', threads=st.THREADS, payload=PAYLOAD, task=False, verts=3, max_v=max_v, max_p=max_p,
                vertex=[['pos', c] for c in range(min(ncomp, 4))] + [[0, c] for c in range(max(ncomp - 4, 0))], primitive=[], stage_memory=lay, shared=size)
    ref, bad = None, 0
    for tag, modes in (('none', set()), ('all three', {'bytes', 'inputs', 'direct'}), ('default', {'bytes', 'inputs', 'direct', 'whole'})):
        applied, got, stale, ni = run_one(tmp, vkrun, variant, text, info, modes, dims, first, groups, cap)
        verdict = ''
        if ref is None:
            ref = got
            if not ni:
                verdict = 'FAILED: the reference draws nothing'; bad += 1
        else:
            same = got[0] == ref[0] and got[3] == ref[3] and not got[4]
            diff = sum(((got[3] - ref[3]) + (ref[3] - got[3])).values())
            verdict = 'same triangles as no modes' if same else f'DIFFERS from no modes (draw {got[0]} / {ref[0]}, {diff} triangles differ, past {got[4]})'
            if same_want and not same:
                verdict = 'FAILED: ' + verdict; bad += 1
            if tag == 'default' and whole_want is not None and ('whole' in applied) != whole_want:
                verdict += "; FAILED: 'whole' " + ('was not taken' if whole_want else 'was taken'); bad += 1
        print(f'{variant:9s} {tag:10s} applied {",".join(sorted(applied)) or "none":28s} {ni:5d} indices, {stale} named vertices hold an unwritten word: {verdict}', flush=True)
    return bad


def main():
    if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):   # the image names a file its Mesa does not install
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    which = set(sys.argv[1:])
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); vkrun = tmp / 'vkrun'
        st.run(['gcc', '-O1', '-o', str(vkrun), str(HERE / 'vkrun.c'), '-lvulkan'])
        for variant, (_, _, what) in CASES.items():
            if which and variant not in which:
                continue
            print(f'-- {variant}: {what}', flush=True)
            total += differential(tmp, vkrun, variant, (64, 40, 5))
    print("mesh wrapper self-test (the mode 'whole' and the area classes):", 'ok' if not total else f'FAILED ({total})')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
