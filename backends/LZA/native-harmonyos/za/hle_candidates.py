# Z-A main's SDK imports that no takeover of this runtime covers, matched against nxnative's HLE_FORCE takeovers
# (dm/host/*.c of the nxnative checkout, seedds/nxnative: NXNATIVE, else ../nxnative beside this backend): name, file,
# return type, arguments, body. A worklist for game_patches_za.c.
# usage: python hle_candidates.py > out.txt
import glob, json, os, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NXNATIVE = Path(os.environ.get('NXNATIVE') or ROOT.parent / 'nxnative')   # else backends/nxnative beside this backend
src = glob.glob(str(ROOT / 'artifacts/harmonyos-native/src/games/totk/runtime/*.c')) + \
      glob.glob(str(ROOT / 'native-harmonyos/overlay/runtime/*.c')) + \
      glob.glob(str(ROOT / 'artifacts/harmonyos-native/src/games/totk/third_party/d2r_gfx/gfx/nvn/*.c'))
taken = set()
for f in src:   # names outside #if GAME_BOTW blocks (TOTK's runtime keeps Breath of the Wild-only takeovers there)
    botw = []
    for line in Path(f).read_text(encoding='utf-8', errors='replace').splitlines():
        s = line.strip()
        if s.startswith('#if'): botw.append('GAME_BOTW' in s and '!' not in s)
        elif s.startswith('#else') and botw: botw[-1] = False
        elif s.startswith('#endif') and botw: botw.pop()
        if not any(botw): taken |= set(re.findall(r'"(_ZN2nn[0-9A-Za-z_]+|nn[a-z][A-Za-z_]+)"', line))
mods = json.loads((ROOT / 'prepared/za/game-data/runtime-layout.json').read_text())
main = next(m for m in mods if m['name'] == 'main')
und = {s['name'] for s in main['symbols'] if not s['defined'] and s['name'].startswith('_ZN2nn')} - taken
hle = {}
for f in glob.glob(str(NXNATIVE / 'dm/host/*.c')):
    t = Path(f).read_text(encoding='utf-8', errors='replace')
    for m in re.finditer(r'HLE_FORCE\("([^"]+)",\s*"((?:[^"\\]|\\.)*)",\s*([^,]+),\s*([A-Za-z0-9_]+)\s*,?([^)]*)\)\s*\{', t):
        name = m.group(1)
        if name not in und:
            continue
        i, depth = m.end(), 1
        while depth and i < len(t):
            depth += {'{': 1, '}': -1}.get(t[i], 0)
            i += 1
        hle[name] = (Path(f).name, m.group(2), m.group(3).strip(), m.group(5).strip(), ' '.join(t[m.end():i - 1].split()))
for n in sorted(hle):
    f, desc, ret, args, body = hle[n]
    print(f'{n}\t{f}\t{ret}\t({args})\t{body[:260]}\t# {desc[:140]}')
print('---- not taken over by nxnative either (pure SDK code or not reached):')
for n in sorted(und - set(hle)):
    print('  ', n)
