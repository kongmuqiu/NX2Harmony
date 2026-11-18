# Symbolize module offsets (main+0x... / sdk+0x...) with the defined symbols in prepared/oot/game-data/runtime-layout.json,
# and name the PLT target of a call just before a return address in main (its GOT relocation's symbol).
# usage: python symbolize.py sdk+0xdc83c main+0x1b2e858 ...
import bisect, json, struct, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
mods = {m['name']: m for m in json.loads((ROOT / 'prepared/oot/game-data/runtime-layout.json').read_text())}
tables = {}
for n, m in mods.items():
    defs = sorted((s['value'], s['size'], s['name']) for s in m['symbols'] if s['defined'] and s['value'])
    tables[n] = defs
def name_at(mod, off):
    d = tables[mod]; i = bisect.bisect_right(d, (off, 1 << 62, '')) - 1
    if i >= 0:
        v, size, nm = d[i]
        if off < v + max(size, 1) or size == 0: return f'{nm}+{off - v:#x}'
    return '?'
def plt_target(mod, ret):
    img = (ROOT / f'prepared/oot/rewrite/rewritten/{mod}.patched.bin').read_bytes()
    w = struct.unpack_from('<I', img, ret - 4)[0]
    if w & 0xfc000000 != 0x94000000: return None
    tgt = ret - 4 + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
    a, b = struct.unpack_from('<II', img, tgt)
    if a & 0x9f00001f != 0x90000010: return f'bl {tgt:#x} ({name_at(mod, tgt)})'
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2); imm = (imm ^ (1 << 20)) - (1 << 20)
    slot = (tgt & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8
    m = mods[mod]
    for r in m['relocs']:
        if r['where'] == slot: return f'PLT {tgt:#x} -> {m["symbols"][r["symbol"]]["name"]}'
    return f'PLT {tgt:#x} slot {slot:#x}'
for arg in sys.argv[1:]:
    mod, off = arg.split('+'); off = int(off, 16)
    t = plt_target(mod, off)
    print(f'{arg}: in {name_at(mod, off)}' + (f'; call before it: {t}' if t else ''))
