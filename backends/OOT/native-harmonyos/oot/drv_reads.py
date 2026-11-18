"""Which words of the NVN driver constant segments (c[0], c[2]) the game's shaders read: an offline audit of a seed folder.

The host fills those segments from the current bindings (overlay android/gfx/vk_draw.c driver_bank_fill, after nxnative's
nvn_mtl.m and dm/gfx/nvn/dmsh_abi.h). A word a shader reads that the host leaves 0 is silent: Ocarina of Time's SH producer
21d9f83e reads the dispatch's workgroup counts at c0 + 4 / + 8 (NVN2's place; Switch 1 has them at c0 + 0x200), took its
sample count N = 0, and wrote +inf / NaN / -inf / NaN into the lighting buffer every material reads (the black 3D world,
oot-a47 .. a61). nxnative's build records each shader's reads (drv_reads) and fails at load on an unknown one; this does the
same for a translated seed: every constant-index read of <stage>_c0 / <stage>_c2 in every module, by stage and byte offset,
against the ranges the host fills.

usage: python native-harmonyos/oot/drv_reads.py [seed shaders/<version> dir] [--jobs 14] [--list OFFSET]
Reads SPIR-V only (no game data is written anywhere but the report under prepared/)."""
import argparse, json, re, struct, sys
from collections import Counter, defaultdict
from multiprocessing import Pool
from pathlib import Path

PORT = Path(__file__).resolve().parents[2]
NAME = re.compile(r'^([a-z]+)_c(0|2)$')

# What driver_bank_fill fills (keep in step with vk_draw.c): (bank, stage prefix or None for any) -> list of [lo, hi)
FILLED = {
    (0, 'vp'): [(0x110, 0x210), (0x610, 0x614)],
    (0, 'fp'): [(0x510, 0x610), (0x610, 0x614)],
    (0, 'cp'): [(0x004, 0x010), (0x200, 0x20c), (0x210, 0x310), (0x310, 0x410)],
    (2, None): [(0x020, 0x120), (0x168, 0x568), (0x568, 0x968)],
}


def reads(path):
    """{(prefix, bank, byte offset or 'dyn')} of one module."""
    d = Path(path).read_bytes()
    w = struct.unpack_from(f'<{len(d) // 4}I', d)
    names, consts, ptr_to, var_bank, chain, loaded, out = {}, {}, {}, {}, {}, {}, set()
    i = 5
    insts = []
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if n == 0:
            break
        insts.append((op, i, n))
        i += n
    for op, i, n in insts:
        if op == 5:      # OpName
            s = b''.join(struct.pack('<I', x) for x in w[i + 2:i + n]).split(b'\0')[0].decode('utf-8', 'replace')
            names[w[i + 1]] = s
        elif op == 43 and n == 4:   # OpConstant, one word
            consts[w[i + 2]] = w[i + 3]
        elif op == 46:   # OpConstantNull
            consts[w[i + 2]] = 0
        elif op == 32:   # OpTypePointer
            ptr_to[w[i + 1]] = w[i + 3]
    for op, i, n in insts:
        if op == 59:     # OpVariable: named itself, or through its struct type
            m = NAME.match(names.get(w[i + 2], '')) or NAME.match(names.get(ptr_to.get(w[i + 1], 0), ''))
            if m:
                var_bank[w[i + 2]] = (m.group(1), int(m.group(2)))
    if not var_bank:
        return out
    for op, i, n in insts:
        if op in (65, 66) and w[i + 3] in var_bank:   # OpAccessChain / OpInBoundsAccessChain: var, 0, element[, component]
            pre, bank = var_bank[w[i + 3]]
            idx = [consts.get(x) for x in w[i + 4:i + n]]
            if len(idx) < 2 or idx[1] is None:
                out.add((pre, bank, 'dyn'))
            elif len(idx) >= 3 and idx[2] is not None:
                out.add((pre, bank, idx[1] * 16 + idx[2] * 4))
            elif len(idx) >= 3:
                out.add((pre, bank, 'dyn'))
            else:
                chain[w[i + 2]] = (pre, bank, idx[1] * 16)
        elif op == 61 and w[i + 3] in chain:          # OpLoad of a whole vec4 element
            loaded[w[i + 2]] = chain[w[i + 3]]
        elif op == 81 and w[i + 3] in loaded:         # OpCompositeExtract: its component
            pre, bank, base = loaded[w[i + 3]]
            out.add((pre, bank, base + w[i + 4] * 4))
        elif op == 79 and (w[i + 3] in loaded or w[i + 4] in loaded):   # OpVectorShuffle: components picked from it
            for k, c in enumerate(w[i + 5:i + n]):
                src = w[i + 3] if c < 4 else w[i + 4]
                if src in loaded and c != 0xffffffff:
                    pre, bank, base = loaded[src]
                    out.add((pre, bank, base + (c & 3) * 4))
    return out


def one(path):
    try:
        return path, sorted(reads(path), key=str)
    except Exception as e:   # noqa: BLE001
        return path, [('error', -1, str(e)[:60])]


def filled(pre, bank, off):
    for lo, hi in FILLED.get((bank, pre), []) + FILLED.get((bank, None), []):
        if lo <= off < hi:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('seed', nargs='?', default=str(PORT / 'prepared' / 'oot' / 'seed-build' / 'shaders' / 'oot131072'))
    ap.add_argument('--jobs', type=int, default=14)
    ap.add_argument('--list', default='', help='print the shaders that read this c0 offset, e.g. cp:0:0x4')
    a = ap.parse_args()
    files = sorted(str(p) for p in Path(a.seed).glob('*.spv'))
    by = defaultdict(set)
    with Pool(a.jobs) as pool:
        for path, rs in pool.imap_unordered(one, files, chunksize=64):
            key = Path(path).name[:64]
            for pre, bank, off in rs:
                by[(pre, bank, off)].add(key)
    rep = {f'{pre}:{bank}:{off if isinstance(off, str) else hex(off)}': sorted(v) for (pre, bank, off), v in by.items()}
    out = Path(a.seed).resolve().parents[1] / 'drv-reads.json'   # beside the seed folder's shaders/
    out.write_text(json.dumps(rep, indent=0))
    print(f'{len(files)} modules; report {out}')
    rows = sorted(by.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2] if isinstance(kv[0][2], int) else -1))
    gaps = [(k, v) for k, v in rows if isinstance(k[2], int) and not filled(*k)]
    print('reads the host does NOT fill (stage:bank:offset  shaders):')
    for (pre, bank, off), v in gaps:
        print(f'  {pre}:c{bank}+{off:#06x}  {len(v):6d}   e.g. {sorted(v)[0][:16]}')
    print('reads by table (filled):')
    tab = Counter()
    for (pre, bank, off), v in rows:
        if isinstance(off, int) and filled(pre, bank, off):
            lo = next(lo for lo, hi in FILLED.get((bank, pre), []) + FILLED.get((bank, None), []) if lo <= off < hi)
            tab[(pre, bank, lo)] += len(v)
        elif not isinstance(off, int):
            tab[(pre, bank, off)] += len(v)
    for (pre, bank, lo), n in sorted(tab.items(), key=str):
        print(f'  {pre}:c{bank} {lo if isinstance(lo, str) else hex(lo)}: {n} shader-reads')
    print('every constant offset read (stage:bank+offset shaders):')
    line = []
    for (pre, bank, off), v in rows:
        if isinstance(off, int):
            line.append(f'{pre}:c{bank}+{off:#x} {len(v)}')
    print('  ' + ', '.join(line))
    if a.list:
        for k in rep.get(a.list, []):
            print(k)


if __name__ == '__main__':
    sys.exit(main())
