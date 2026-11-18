# The Android runtime's game data folder (runtime/game_data.h) for the HarmonyOS port, from what is already prepared:
#   runtime-tables.bin  scripts/prepare_runtime.py on the HarmonyOS rewrite (rewrite_ohos.py output)
#   romfs-config.txt    as scripts/prepare_romfs_backend.py: image size and SHA-256 from the RomFS folder's index.json, and the
#                       SHA-256 of runtime-index.bin rebuilt with the same algorithm, which must equal the index already
#                       prepared (and pushed to the device next to romfs.bin), byte for byte
#   shared-fonts.txt    as scripts/prepare_shared_fonts.py, from the shared-fonts folder's manifest.json
# usage: python game_data_ohos.py <RomFS folder> <shared-fonts folder> <game-data folder>
#   the RomFS folder holds romfs.bin, index.json and runtime-index.bin (written here when missing): the 1.0.0 extraction's romfs/, or the merged
#   update's (scripts/extract_update_romfs.py); the shared fonts come from the firmware and are the same for every version
import hashlib, json, struct, sys
from pathlib import Path

romfs, fonts, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
assert (out / 'runtime-tables.bin').is_file(), 'run scripts/prepare_runtime.py first'

manifest = json.loads((romfs / 'index.json').read_text())
assert manifest['status'] == 'pass' and (romfs / 'romfs.bin').stat().st_size == manifest['romfs_size']
strings, rows = bytearray(), bytearray()
for item in manifest['files']:
    name = item['path'].encode() + b'\0'
    rows.extend(struct.pack('<QQII', item['offset'], item['size'], len(strings), len(name)))
    strings.extend(name)
index = struct.pack('<8sQQ', b'TOTKRFS1', len(manifest['files']), 24 + len(rows)) + rows + strings
if not (romfs / 'runtime-index.bin').exists():   # the merged update's folder has none yet (extract_update_romfs.py)
    (romfs / 'runtime-index.bin').write_bytes(index)
assert (romfs / 'runtime-index.bin').read_bytes() == index, 'prepared runtime-index.bin differs from the reference algorithm'
(out / 'romfs-config.txt').write_bytes(f'ROMFS_IMAGE_SIZE {manifest["romfs_size"]}\nROMFS_IMAGE_SHA256 {manifest["romfs_sha256"]}\n'
                                      f'ROMFS_INDEX_SHA256 {hashlib.sha256(index).hexdigest()}\n'.encode())

fm = json.loads((fonts / 'manifest.json').read_text())
assert fm['status'] == 'pass' and [f['type'] for f in fm['fonts']] == list(range(6))
lines = []
for f in fm['fonts']:
    path = fonts / f"{f['type']}-{f['name']}.bfttf"
    assert path.stat().st_size == f['bfttf_bytes'] == f['font_bytes'] + 8 and ' ' not in path.name
    i = f['type']
    lines.append(f"FONT{i}_FILE {path.name}\nFONT{i}_FILE_BYTES {f['bfttf_bytes']}\nFONT{i}_FONT_BYTES {f['font_bytes']}\nFONT{i}_SHA256 {f['sha256']}\n")
(out / 'shared-fonts.txt').write_bytes(''.join(lines).encode())
print(json.dumps({'romfs_files': len(manifest['files']), 'index_sha256': hashlib.sha256(index).hexdigest(), 'fonts': len(lines)}))
