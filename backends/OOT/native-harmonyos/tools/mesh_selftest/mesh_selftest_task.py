"""Self-test of the mesh wrapper's task mode 'payload' (a task kernel without its stage memory: mesh_wrap_in_container.py,
task_payload()), on lavapipe. oot-a106.

Every case is a task kernel in the translator's idiom (mesh_selftest_modes.py's Gen), wrapped with no modes (the
reference), with 'payload' alone and with the default modes, and dispatched twice: with room for every launch, and
with a payload buffer and a table that hold fewer tasks and launches than the dispatch has. The task step's output
must be the reference's: the payloads word for word (also the words no thread stored: zero), the control block word
for word, the table of the mesh groups as a set of (payload place, launch number) - its order is the order of the
atomics - and nothing written past the payloads or past the table's entries. A case the mode must REFUSE keeps the
stage memory (checked: the mode is not in the modes applied) and is run all the same.

  taken    plain     thread 1 stores the launch count and three words at constant payload words, every thread its words
                     through 'register + K'; one group in seven returns at once in every thread (count 0, all zero)
           countdyn  the launch count itself is stored through 'register + K' (the store function's shared word)
           sparse    three threads store, the rest of the payload stays as cleared
           chain     the register carries a part of the area's offset (classed by the whole sum)
           twice     a thread stores its word twice (the second value stays)
           big       3200 payload bytes with three areas of computed words, as the corpus's largest task kernels
           one       ONE thread: it reads payload words back, at a constant word, through 'register + K', and the count
  refused  readback  32 threads and a load of a payload word
           input     a load of a word of the input area
           lowstore  a store below the payload
           past      a store at a constant word past the payload
           atomic    an atomic on a payload word
           helper    a byte stored through the translator's byte store helper
           opaque    an index that is not the translator's form
           plainglsl plain GLSL with a loop (mesh_selftest_modes.py's old task kernel)

  docker run --rm -v <port>:/repo mng-vk-host:lavapipe python3 /repo/native-harmonyos/tools/mesh_selftest/mesh_selftest_task.py [case ...]

The corpus (the game's own task kernels; game-derived, so the folder is an argument):
  ... mesh_selftest_task.py --corpus <kernels dir> <mesh-info.json> <spirv-cross> [key prefix ...]
wraps every task kernel with no modes and with 'payload' and runs both on the same synthetic buffers (uniform words and
storage words of one value a scenario, as exp-gmem-verify's v_kernel_eq.py; lavapipe's subgroups are 8 wide, so the
kernels do not compute what the console computes, but the two modules are one program apart from the mode). Every
buffer must be equal word for word, the table as a set."""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import mesh_selftest_modes as st   # noqa: E402
import mesh_wrap_in_container as mw   # noqa: E402

FILL = st.FILL
SLACK = 64   # words after the payloads that nothing may write
# case: (the mode must be taken, threads, payload bytes)
CASES = {
    'plain': (True, 32, 256), 'countdyn': (True, 32, 256), 'sparse': (True, 32, 256), 'chain': (True, 32, 256),
    'twice': (True, 32, 256), 'big': (True, 32, 3200), 'one': (True, 1, 128),
    'readback': (False, 32, 256), 'input': (False, 32, 256), 'lowstore': (False, 32, 256), 'past': (False, 32, 256),
    'atomic': (False, 32, 256), 'helper': (False, 32, 256), 'opaque': (False, 32, 256), 'plainglsl': (False, 32, 256),
}
MODESETS = (('none', set()), ('payload', {'payload'}), ('default', set(mw.DEFAULT_MODES.split(','))))


def kernel_t(variant, threads, payload):
    lay, size = st.task_layout(payload)
    wg, inp, out, _ = lay
    pw = payload // 4
    if variant == 'plainglsl':
        return st.task_kernel_plain(lay, (size + 3) // 4, payload), lay, size
    g = st.Gen()
    I = '        '
    t = g.t('int', 'int(gl_LocalInvocationID.x)')
    n = g.t('int', f'int(_33[{wg // 4}])')
    g.raw(f'    if (({n} % 7) == 3)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    count = g.t('int', f'{n} % 5')
    g.raw(f'    if ({t} == {1 if threads > 1 else 0})'); g.raw('    {')
    if variant == 'countdyn':
        r = g.t('int', f'({t} | 1) << 2', I)                 # 4: the payload's word 1, through a register
        g.store(r, out, f'uint({count})', I)
    else:
        g.raw(f'        _33[{out // 4 + 1}] = uint({count});')
    for w, e in ((2, f'40 + {n}'), (4, f'{n} * 3 + 1'), (5, f'{n} ^ 21845')):
        v = g.t('int', e, I)
        g.raw(f'        _33[{out // 4 + w}] = uint({v});')
    if variant == 'readback':
        back = g.t('int', f'int(_33[{out // 4 + 2}])', I)   # what this thread stored itself: still a load of the payload
        v = g.t('int', f'{back} + 7', I)
        g.raw(f'        _33[{out // 4 + 3}] = uint({v});')
    if variant == 'input':
        z = g.t('int', f'int(_33[{inp // 4 + 1}])', I)      # the input area: cleared, 0
        v = g.t('int', f'{z} + 77', I)
        g.raw(f'        _33[{out // 4 + 3}] = uint({v});')
    if variant == 'lowstore':
        g.raw(f'        _33[{inp // 4 + 2}] = 99u;')          # a workgroup variable below the payload: not copied out
    if variant == 'past':
        g.raw(f'        _33[{out // 4 + pw}] = 99u;')         # the word after the payload
    if variant == 'atomic':
        g.raw(f'        atomicAdd(_33[{out // 4 + 3}], 5u);')
    if variant == 'helper':
        g.store8(g.t('int', f'{t} << 2', I), out + 12, f'{n} + 3', I)   # a byte of payload word 3 + t
    if variant == 'opaque':
        a = g.t('uint', f'uint({t} * 2 + {out // 4 + 1})', I)   # the payload's word 3, which no other store writes
        g.raw(f'        _33[int({a})] = 99u;')
    g.raw('    }')
    areas = ((6, pw),) if variant != 'big' else ((6, 262), (262, 518), (518, pw))   # K - out = 24, 1048, 2072
    for lo, hi in areas:
        for j in range((min(hi - lo, 96) + threads - 1) // threads if threads > 1 else min(hi - lo, 12)):
            w = g.t('int', f'{t} + {threads * j}')            # the word within the area
            cond = f'{w} < {hi - lo}' + (f' && {t} < 3' if variant == 'sparse' else '')
            g.raw(f'    if ({cond})'); g.raw('    {')
            b = g.t('int', f'{w} << 2', I)
            v = g.t('int', f'{n} * 1000 + {w} + {lo}', I)
            if variant == 'chain':
                b = g.t('int', f'{b} + 8', I)                 # the register carries 8 of the area's offset
                g.store(b, out + lo * 4 - 8, f'uint({v})', I)
            else:
                g.store(b, out + lo * 4, f'uint({v})', I)
            if variant == 'twice':
                v2 = g.t('int', f'{v} ^ 255', I)
                g.store(b, out + lo * 4, f'uint({v2})', I)
            if variant == 'one':
                back = g.load(b, out + lo * 4, I)             # the word just stored, through 'register + K'
                v3 = g.t('int', f'{back} + int(_33[{out // 4 + 4}]) + int(_33[{out // 4 + 1}])', I)   # + a constant word + the count
                g.store(b, out + lo * 4 + 48, f'uint({v3})', I)
            g.raw('    }')
    if variant == 'one':
        r1 = g.t('int', f'({n} | 1) & 4 | 4')                 # 4: the count, read back through a register
        c2 = g.load(r1, out)
        g.raw(f'    _33[{out // 4 + 3}] = uint({c2} + 1000);')
    g.raw(f'    if ({t} > 9)'); g.raw('    {'); g.raw('        return;'); g.raw('    }')
    g.t('int', f'{t} + 1')
    return g.text((size + 3) // 4, helper=variant == 'helper', threads=threads), lay, size


def task_step(vkrun, d, spv, by, first, tasks, tcap, cap, pw, extra=()):
    """one dispatch of a wrapped task kernel: (the table's words, the payload words and the slack after them, the control block)"""
    st.ctl_file(d / 'ctl.bin', first, cap, tcap, tasks, 0)
    in_words = 2 * cap + tcap * pw + SLACK
    st.run([str(vkrun), str(spv), str(tasks), f'{by[mw.RB_MESH_IN]}:s:{in_words * 4}', f'{by[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{d / "ctl.bin"}',
            *extra], cwd=d)
    w, ctl = st.words_of(d / f'out_{by[mw.RB_MESH_IN]}.bin'), st.words_of(d / f'out_{by[mw.RB_MESH_PAR]}.bin')
    return w[:2 * cap], w[2 * cap:], ctl


def judge(table, pay, ctl, tcap, cap, pw):
    """(what must equal the reference, the faults of the output by itself)"""
    faults = []
    total = ctl[mw.C_DISP]
    counts = [min(pay[g * pw + 1], cap) for g in range(tcap)]
    if total != sum(counts):
        faults.append(f'{total} launches counted, the payloads say {sum(counts)}')
    fit = min(total, cap)
    entries = [(table[2 * gi], table[2 * gi + 1]) for gi in range(fit)]
    for at, j in entries:
        g = (at - 2 * cap) // pw
        if (at - 2 * cap) % pw or not 0 <= g < tcap or j >= counts[g]:
            faults.append(f'table entry ({at}, {j})')
    if len(set(entries)) != fit:
        faults.append('a table entry twice')
    if any(w != FILL for w in table[2 * fit:]):
        faults.append('words written past the table\'s entries')
    if any(w != FILL for w in pay[tcap * pw:]):
        faults.append('words written past the payloads')
    return (pay, ctl, sorted(entries) if cap >= total else fit), faults


def synthetic(tmp, vkrun, which):
    bad = 0
    for variant, (taken, threads, payload) in CASES.items():
        if which and variant not in which:
            continue
        text, lay, size = kernel_t(variant, threads, payload)
        pw = (payload + 3) // 4
        info = dict(kind='ts', threads=threads, payload=payload, stage_memory=lay, shared=size)
        ref = {}
        for tag, modes in MODESETS:
            d = tmp / f'{variant}-{tag}'; d.mkdir()
            by, applied = st.build(mw.wrap_modes, d, 'task', text, info, set(modes))
            verdict = []
            if ('payload' in applied) != (taken and 'payload' in modes):
                verdict.append("FAILED: 'payload' " + ('was taken' if 'payload' in applied else 'was not taken'))
            shared = 4 * sum(int(x) for x in re.findall(r'(?m)^shared uint \w+\[(\d+)\];', (d / 'task.comp').read_text()))
            # room for every launch; then a payload buffer of 9 tasks under a dispatch of 13 and a table of 10 mesh groups
            for run, (tasks, tcap, cap) in (('room', (13, 13, 64)), ('tight', (13, 9, 10))):
                table, pay, ctl = task_step(vkrun, d, d / 'task.spv', by, 2, tasks, tcap, cap, pw)
                got, faults = judge(table, pay, ctl, tcap, cap, pw)
                verdict += [f'FAILED ({run}): {f}' for f in faults]
                if tag == 'none':
                    ref[run] = got
                    if run == 'room' and (not ctl[mw.C_DISP] or not any(w not in (0, FILL) for w in pay)):
                        verdict.append('FAILED: the reference launches nothing')
                elif got != ref[run]:
                    what = [n for n, a, b in zip(('the payloads', 'the control block', 'the table'), got, ref[run]) if a != b]
                    verdict.append(f'FAILED ({run}): {", ".join(what)} differ from the wrapper with no modes')
            bad += len(verdict)
            same = '' if tag == 'none' else 'as the wrapper with no modes wrote'
            print(f'{variant:9s} {tag:8s} applied {",".join(sorted(applied)) or "none":8s} shared memory {shared:5d} bytes, '
                  f'{ref["room"][1][mw.C_DISP]} launches: {"; ".join(verdict) or same or "the reference"}', flush=True)
    return bad


# ---- the corpus ----
SCEN = [('u=0 s=0x40', 0, 0x40, 0), ('u=3 s=0x40', 3, 0x40, 5), ('u=0x101 s=0x44', 0x101, 0x44, 1), ('u=7 s=0x4c', 7, 0x4c, 100),
        ('u=3 s=0x80', 3, 0x80, 2)]
MiB = 1 << 20


def corpus_one(args):
    tmp, vkrun, kernels, info, cross, key = args
    i = info[key]
    pw = (i['payload'] + 3) // 4
    name = sorted(p.name for p in kernels.glob(f'{key}-*.spv'))[0]
    d = tmp / key[:16]; d.mkdir()
    r = subprocess.run([cross, '--vulkan-semantics', str(kernels / name)], capture_output=True, text=True, errors='replace')
    if r.returncode:
        return key, 1, f'spirv-cross: {r.stderr.strip()[:120]}'
    glsl = mw.gmem_pass(r.stdout)
    ver = struct.unpack_from('<I', (kernels / name).read_bytes(), 4)[0]
    mods = {}
    for tag, modes in (('none', set()), ('payload', {'payload'})):
        text, binds, applied = mw.wrap_modes(glsl, i, modes)
        if ('payload' in applied) != bool(modes):
            return key, 1, f"'payload' not taken ({tag})"
        (d / f'{tag}.comp').write_text(text)
        e = mw.compile_module(d / f'{tag}.comp', d / f'{tag}.spv', 'comp', 'vulkan1.1' if ver <= 0x10300 else 'vulkan1.2')
        if e:
            return key, 1, f'{tag}: {e}'
        by = {kind: b for b, _, kind, _ in binds}
        own = [(int(b), k) for b, k in re.findall(r'(?m)^layout\(set = 0, binding = (\d+)[^)]*\) (?:readonly |writeonly )?(uniform|buffer) (?!dmsh_)\w+$', text)]
        if len(own) + 2 != len(re.findall(r'(?m)^layout\(set = 0, binding = \d+', text)):
            return key, 0, 'a binding that is no buffer: not run'
        shared = 4 * sum(int(x) for x in re.findall(r'(?m)^shared uint \w+\[(\d+)\];', text))
        mods[tag] = (by, own, shared)
    tasks, cap = 3, 4096
    bad = died = 0
    notes = []
    for sc, uword, sword, first in SCEN:
        (d / 'ubo.bin').write_bytes(struct.pack('<I', uword) * 16384)
        (d / 'ssbo.bin').write_bytes(struct.pack('<I', sword) * (MiB // 4))
        res = {}
        for tag, (by, own, _) in mods.items():
            rd = d / f'{tag}-run'; rd.mkdir(exist_ok=True)
            for f in rd.glob('out_*.bin'):
                f.unlink()
            st.ctl_file(rd / 'ctl.bin', first, cap, tasks, tasks, 0)
            cmd = [str(vkrun), str(d / f'{tag}.spv'), str(tasks), f'{by[mw.RB_MESH_IN]}:s:{(2 * cap + tasks * pw + SLACK) * 4}',
                   f'{by[mw.RB_MESH_PAR]}:s:{mw.C_WORDS * 4}:{rd / "ctl.bin"}']
            cmd += [f'{b}:u:65536:{d / "ubo.bin"}' if k == 'uniform' else f'{b}:s:{MiB}:{d / "ssbo.bin"}' for b, k in own]
            try:
                r = subprocess.run(cmd, cwd=rd, capture_output=True, text=True, timeout=150)
                ok = r.returncode == 0
            except subprocess.TimeoutExpired:
                ok = False
            if not ok:
                res = None; break
            w, ctl = st.words_of(rd / f'out_{by[mw.RB_MESH_IN]}.bin'), st.words_of(rd / f'out_{by[mw.RB_MESH_PAR]}.bin')
            got, faults = judge(w[:2 * cap], w[2 * cap:], ctl, tasks, cap, pw)
            others = {b: (rd / f'out_{b}.bin').read_bytes() for b, _ in own}
            res[tag] = (got, faults, others)
        if res is None:
            died += 1; notes.append(f'{sc}: a module died or ran over the time on lavapipe, not compared')
            continue
        (g0, f0, o0), (g1, f1, o1) = res['none'], res['payload']
        same = g0 == g1 and o0 == o1 and not f1
        bad += not same
        pay = g0[0][:tasks * pw]
        notes.append(f'{sc}: {g0[1][mw.C_DISP]} launches, {sum(1 for w in pay if w)} payload words not zero: '
                     + ('equal' if same else 'DIFFERENT ' + '; '.join(f1)))
    return key, bad, f'shared memory {mods["none"][2]} -> {mods["payload"][2]} bytes; ' + ' | '.join(notes) + (f' ({died} not compared)' if died else '')


def corpus(tmp, vkrun, argv):
    kernels, info, cross, which = Path(argv[0]), json.loads(Path(argv[1]).read_text()), argv[2], argv[3:]
    keys = sorted(k for k, v in info.items() if v['kind'] == 'ts' and (not which or any(k.startswith(w) for w in which)))
    bad = 0
    with ThreadPoolExecutor(max_workers=int(os.environ.get('JOBS', '8'))) as pool:
        for key, b, what in pool.map(corpus_one, [(tmp, vkrun, kernels, info, cross, k) for k in keys]):
            bad += b
            print(f'{key[:16]} {"ts"} {info[key]["threads"]:2d} threads: {what}', flush=True)
    print(f'{len(keys)} task kernels of the corpus, {bad} with a difference or not built')
    return bad


def main():
    if not os.path.exists(os.environ.get('VK_ICD_FILENAMES', '')):   # the image names a file its Mesa does not install
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); vkrun = tmp / 'vkrun'
        st.run(['gcc', '-O1', '-o', str(vkrun), str(HERE / 'vkrun.c'), '-lvulkan'])
        if sys.argv[1:2] == ['--corpus']:
            total = corpus(tmp, vkrun, sys.argv[2:])
        else:
            total = synthetic(tmp, vkrun, set(sys.argv[1:]))
    print("mesh wrapper self-test (the task mode 'payload'):", 'ok' if not total else f'FAILED ({total})')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
