"""Find the instructions of a game module that load or store a given structure offset (for example the frame
controller's field at +0x2c8), with the function context printed around each hit.

usage: python native-harmonyos/oot/find_field.py <offset hex> [--module main] [--stores] [--context 6]
The text segment is scanned with capstone (AArch64); only immediate-offset loads / stores are found."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disas   # noqa: E402  (load(): the module image as laid out in memory)
import capstone   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('offset'); ap.add_argument('--module', default='main'); ap.add_argument('--stores', action='store_true')
    ap.add_argument('--context', type=int, default=6)
    a = ap.parse_args()
    want = int(a.offset, 16)
    raw = (disas.EXEFS / a.module).read_bytes()
    foff, moff, msz = struct.unpack_from('<III', raw, 0x10)   # text segment
    img = disas.load(a.module)
    text = img[moff:moff + msz]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    tag = f'#0x{want:x}]'
    hits = []
    for i in range(0, len(text) - 3, 4):
        w = text[i:i + 4]
        for ins in md.disasm(w, moff + i):
            if tag not in ins.op_str:
                continue
            if a.stores and not ins.mnemonic.startswith(('str', 'stur', 'stp')):
                continue
            if not a.stores and not ins.mnemonic.startswith(('ldr', 'ldur', 'ldp', 'str', 'stur', 'stp')):
                continue
            hits.append(moff + i)
    print(f'{len(hits)} instructions with [..., #0x{want:x}] in {a.module}')
    for h in (hits if a.context == 0 else hits[:60]):
        lo = max(moff, h - 4 * a.context)
        print('----')
        for ins in md.disasm(img[lo:h + 4 * (a.context + 1)], lo):
            print(('=> ' if ins.address == h else '   ') + f'{a.module}+0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}')


if __name__ == '__main__':
    main()
