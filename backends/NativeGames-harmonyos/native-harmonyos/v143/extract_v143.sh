#!/bin/sh
# 1.4.3 for the HarmonyOS port, step 1 (code), as the reference's build_app.py does it for --update:
#   decompress the update NSZ with NSZ's hash-verified NCA conversion, extract the update's ExeFS (scripts/extract_exefs.py
#   --update) and check every module against games/totk/assets/expected-v1.4.3.json (the version is chosen by those
#   hashes, never by the file name), then the module inventory (scripts/prepare_modules.py).
# Container: python:3.14 with /android (reference repo, read-only), /keys (the player's key folder, read-only),
# /update (the update .nsz, read-only), /work (output: extracted/containers, extracted/v143/{exefs,elf}),
# /venv (cached virtualenv volume).
#   sh extract_v143.sh
set -eu
if [ ! -x /venv/bin/nsz ]; then
  python3 -m venv /venv
  /venv/bin/pip install -q cryptography==50.0.2 lz4==4.4.5 capstone==5.0.9 nsz==5.0.0
fi
T=/android/games/totk
W=/work
mkdir -p "$W/extracted/containers" "$W/extracted/v143" "$W/logs"
UPD=$(ls /update/*.nsz | head -1)
NSP="$W/extracted/containers/$(basename "${UPD%.nsz}").nsp"
if [ ! -s "$NSP" ]; then
  echo "decompressing $(basename "$UPD") (hash-verified NCA conversion)"
  /venv/bin/nsz -D --keys /keys/prod.keys -o "$W/extracted/containers" "$UPD" > "$W/logs/decompress-update.log" 2>&1
fi
ls -la "$NSP"
rm -rf "$W/extracted/v143/exefs"
cd "$T"
/venv/bin/python scripts/extract_exefs.py "$NSP" --keys /keys/prod.keys --update --output "$W/extracted/v143/exefs" > "$W/logs/extract_exefs-v143.log" 2>&1
/venv/bin/python - "$W/extracted/v143/exefs" <<'EOF'
import hashlib, json, sys
from pathlib import Path
exefs = Path(sys.argv[1])
want = json.loads(Path('/android/games/totk/assets/expected-v1.4.3.json').read_text())['exefs_sha256']
bad = 0
for name, h in want.items():
    p = exefs / name
    got = hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'missing'
    ok = got == h
    bad += not ok and name != 'main.npdm'
    print(f'{name}: {"match" if ok else "DIFFERS " + got[:16]}')
sys.exit(1 if bad else 0)
EOF
echo "ExeFS is 1.4.3"
/venv/bin/python scripts/prepare_modules.py "$W/extracted/v143/exefs" --output "$W/extracted/v143/elf" > "$W/logs/prepare_modules-v143.log" 2>&1 && echo "module inventory done"
