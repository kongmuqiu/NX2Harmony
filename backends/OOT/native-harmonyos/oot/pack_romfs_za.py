# Pack Pokémon Legends: Z-A's RomFS folder (2.0.2, as tools/nup_extract.py unpacks it) into the single image the
# runtime's RomFS backend reads (runtime/romfs.c through game-data/romfs-config.txt and runtime-index.bin):
#   romfs.bin    every file's bytes, 16-byte aligned, in path order
#   index.json   {"status":"pass","romfs_size","romfs_sha256","files":[{"path","offset","size"}...]} as game_data_ohos.py reads it
# The NUP already checked every file's SHA-256 (the part's .digests record); this checks each file against that record
# while it copies. Game material: stays under prepared/, never committed or published.
# usage: python pack_romfs_za.py <part folder with romfs/ and .digests> <out folder>
import hashlib, json, os, sys
from pathlib import Path

ALIGN = 16
CHUNK = 8 << 20
part, out = Path(sys.argv[1]), Path(sys.argv[2])
src = part / 'romfs'
digests = json.loads((part / '.digests').read_text())
out.mkdir(parents=True, exist_ok=True)
# by the path's bytes, the order runtime/romfs.c checks (strcmp): Windows paths sort without case, which put
# Ocarina of Time's mixed-case names out of order ("RomFS sorted paths")
files = sorted((p for p in src.rglob('*') if p.is_file()), key=lambda p: p.relative_to(src).as_posix().encode())
image = out / 'romfs.bin'
tmp = image.with_suffix('.partial')
whole = hashlib.sha256()
rows, offset = [], 0
with open(tmp, 'wb') as w:
    for p in files:
        rel = p.relative_to(src).as_posix()
        pad = -offset % ALIGN
        if pad:
            w.write(bytes(pad)); whole.update(bytes(pad)); offset += pad
        h = hashlib.sha256(); size = 0
        with open(p, 'rb') as r:
            for b in iter(lambda: r.read(CHUNK), b''):
                w.write(b); whole.update(b); h.update(b); size += len(b)
        assert h.hexdigest() == digests['romfs/' + rel], f'{rel}: differs from the package record'
        rows.append(dict(path=rel, offset=offset, size=size))
        offset += size
        print(f'{rel} {size:,d}', flush=True)
os.replace(tmp, image)
for stale in ('runtime-index.bin',):   # game_data_ohos.py writes it again from this index
    if (out / stale).exists(): (out / stale).unlink()
manifest = dict(status='pass', romfs_size=offset, romfs_sha256=whole.hexdigest(), files=rows,
                integrity='files from the NUP package, each SHA-256 checked against its record while packed')
(out / 'index.json').write_text(json.dumps(manifest, indent=1) + '\n')
print(json.dumps(dict(files=len(rows), romfs_size=offset, romfs_sha256=manifest['romfs_sha256'])))
