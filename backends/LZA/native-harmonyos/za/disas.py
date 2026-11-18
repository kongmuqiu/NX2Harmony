"""Disassemble Z-A's original modules around an offset (the NSO from the unpacked NUP, text/ro/data segments laid out
at their memory offsets; LZ4 segments are decompressed here). For reading crash sites: main+0x... as the runtime's
crash report prints them.
usage: python -I native-harmonyos/za/disas.py main+0x261c910 [--before 24] [--after 24] [--func]
  --func  walk back to the function start (the previous ret / unconditional branch boundary, heuristic) and print it
"""
import argparse, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXEFS = ROOT / 'prepared/nup/0100F43008C44000-v393216-update/exefs'


def lz4_block(src, size):
    out = bytearray(); i = 0
    while i < len(src):
        tok = src[i]; i += 1
        n = tok >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255: break
        out += src[i:i + n]; i += n
        if i >= len(src): break
        off = src[i] | src[i + 1] << 8; i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        m += 4
        for _ in range(m): out.append(out[-off])
    assert len(out) == size, (len(out), size)
    return bytes(out)


def load(name):
    d = (EXEFS / name).read_bytes()
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 0xc)[0]
    img = bytearray()
    for k, (hoff, csz_off) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, moff, msz = struct.unpack_from('<III', d, hoff)
        csz = struct.unpack_from('<I', d, csz_off)[0]
        seg = d[foff:foff + csz]
        if flags >> k & 1: seg = lz4_block(seg, msz)
        else: seg = seg[:msz]
        if len(img) < moff: img += bytes(moff - len(img))
        img[moff:moff + msz] = seg
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('where'); ap.add_argument('--before', type=int, default=24); ap.add_argument('--after', type=int, default=24)
    ap.add_argument('--func', action='store_true')
    a = ap.parse_args()
    mod, off = a.where.split('+'); off = int(off, 16)
    img = load(mod)
    import capstone   # only here: load() (za_seed.py, find_field.py) needs no disassembler
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    start = off - 4 * a.before
    if a.func:
        p = off - 4
        while p > 0:
            w = struct.unpack_from('<I', img, p)[0]
            if w == 0xd65f03c0 or (w & 0xfc000000) == 0x14000000: break   # ret / b
            p -= 4
        start = p + 4
    end = off + 4 * a.after
    for ins in md.disasm(img[start:end], start):
        mark = '=>' if ins.address == off else '  '
        print(f'{mark} {mod}+{ins.address:#x}: {ins.mnemonic:8} {ins.op_str}')


if __name__ == '__main__':
    sys.exit(main())
