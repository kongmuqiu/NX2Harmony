"""Fold the development environment files on the phone into one (oot-a106).
devenv.ps1 writes a new files/android/env-dev-<time>.txt for every change; the app applies them all in name order, a later
line winning and 'KEY=' clearing. This reads them in that order, computes what is in force, shows it, writes it as
env-dev-<now>.txt and removes the older files (only env-dev-<digits>.txt; env-dev.txt is left alone).
    python -I native-harmonyos/oot/devenv_compact.py [--dry-run]"""
import datetime, os, subprocess, sys, tempfile
HDC = os.environ.get('TOTK_HDC', 'hdc'); SRV = os.environ.get('TOTK_HDC_SERVER', ''); TGT = os.environ.get('TOTK_HDC_TARGET', '')   # '': hdc's own server, the only connected device
BUNDLE = os.environ.get('TOTK_OHOS_BUNDLE') or 'nx2h.u000000.oot'; REMOTE = f'/data/app/el2/100/base/{BUNDLE}/haps/entry/files/android'
def hdc(*a):
    r = subprocess.run([HDC, *(['-s', SRV] if SRV else []), *(['-t', TGT] if TGT else []), *a], capture_output=True, text=True, errors='replace', env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
    return r.stdout
names = sorted(n.strip() for n in hdc('shell', f'ls {REMOTE}').split() if n.startswith('env-dev-') and n.endswith('.txt') and n[8:-4].isdigit())
state = {}
for n in names:
    for line in hdc('shell', f'cat {REMOTE}/{n}').splitlines():
        line = line.strip()
        if '=' not in line or line.startswith('#'): continue
        k, v = line.split('=', 1)
        if v: state[k] = v
        else: state.pop(k, None)
print(f'{len(names)} files; in force:'); [print(f'  {k}={v}') for k, v in sorted(state.items())]
if '--dry-run' in sys.argv or not names: sys.exit(0)
name = 'env-dev-' + datetime.datetime.now().strftime('%Y%m%d%H%M%S') + '.txt'
tmp = os.path.join(tempfile.gettempdir(), name)
with open(tmp, 'w', newline='\n') as f: f.write(''.join(f'{k}={v}\n' for k, v in sorted(state.items())))
print(hdc('file', 'send', '-b', BUNDLE, tmp, f'/data/storage/el2/base/haps/entry/files/android/{name}').strip().splitlines()[-1:])
os.remove(tmp)
for n in names:
    if n != name: hdc('shell', f'rm -f {REMOTE}/{n}')
left = [n for n in hdc('shell', f'ls {REMOTE}').split() if n.startswith('env-dev-')]
print('left on the phone:', left)
