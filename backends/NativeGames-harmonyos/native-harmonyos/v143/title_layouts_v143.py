#!/usr/bin/env python3
"""totk-ohos: the title screen's layouts in a per-screen overlay romfs-patches-<W>x<H> (v143/romfs_patches_v143.sh, after
the reference's make_romfs_patches.py has written the overlay's Common archive).

1.4.3 ships its UI layouts in two archives the game loads by UI/LayoutArchiveCategory.Product.140.byml: Common.Product.140
(334 layouts) and Title.Product.140 (45: Title_00, TitleMenuBG_00, LoadingFade_00, SystemOption_00, SystemLoadList_00,
SystemSaveLoad_00, SystemWindow_00, ShortCut_00 and their parts), whose members are byte-for-byte copies of Common's.
scripts/ultrawide_layout.py edits Common's only, and with those layouts the app turns its UI squeeze off
(TOTK_UI_SQUEEZE=0), so what the game draws from the Title archive would be stretched across the screen. This gives
the Title archive the same edits: each of its members that the overlay's Common archive changed (the layout edits; the
base overlay's quick-menu capture keys are not in it) is replaced by Common's edited copy, only where the Title member
is the very bytes of Common's original. The archive is compressed with its own dictionary and level 19 as
make_romfs_patches.py does, and its line is added to the overlay's manifest.tsv (written to a new file, then renamed).

    python title_layouts_v143.py <merged romfs folder: index.json, romfs.bin> <overlay folder> <W>x<H>
Container: python:3.14 (compression.zstd)."""
import hashlib, json, os, struct, sys
from pathlib import Path
import compression.zstd as zstd

romfs, out, size = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
COMMON = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'
TITLE = 'UI/LayoutArchive/Title.Product.140.Nin_NX_NVN.blarc.zs'
BASE = out.parent / 'romfs-patches'   # the base overlay: its Common archive holds the edits that are not layout edits

def sarc_members(data):
    """name -> (absolute start, end) of every SARC member (as make_romfs_patches.py reads them)."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, total, start = struct.unpack_from('<HxxII', data, 4)
    assert total == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'; names += 8
    members = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        members[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return members

index = json.loads((romfs / 'index.json').read_text()); assert index['status'] == 'pass'
files = {f['path']: f for f in index['files']}
with (romfs / 'romfs.bin').open('rb') as image:
    def read(path): f = files[path]; image.seek(f['offset']); b = image.read(f['size']); assert len(b) == f['size']; return b
    dictionaries = {}
    for name, (lo, hi) in sarc_members(pack := zstd.decompress(read('Pack/ZsDic.pack.zs'))).items():
        d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
    raw, common_raw = read(TITLE), read(COMMON)
def unpack(data):
    info = zstd.get_frame_info(data); b = zstd.decompress(data, zstd_dict=dictionaries[info.dictionary_id])
    assert len(b) == info.decompressed_size; return b, info.dictionary_id
blarc, dict_id = unpack(raw); blarc = bytearray(blarc)
common, _ = unpack(common_raw); base, _ = unpack((BASE / COMMON).read_bytes()); wide, _ = unpack((out / COMMON).read_bytes())
members, cm = sarc_members(blarc), sarc_members(common)
assert sarc_members(base) == cm == sarc_members(wide), 'the overlays\' Common archives have another member table'
manifest = (out / 'manifest.tsv').read_text().splitlines()
assert any(l.split('\t')[0] == COMMON for l in manifest) and not any(l.split('\t')[0] == TITLE for l in manifest), 'manifest'

# the members the per-screen Common archive changed beyond the base overlay's: the layout edits
edited = sorted(m for m, (lo, hi) in cm.items() if base[lo:hi] != wide[lo:hi])
copied, kept = [], []
for m, (lo, hi) in sorted(members.items()):
    if m not in edited: continue
    clo, chi = cm[m]
    if bytes(blarc[lo:hi]) != common[clo:chi]: raise SystemExit(f'{TITLE}: {m} is not Common\'s original, not copied')
    assert base[clo:chi] == common[clo:chi], f'{m}: the base overlay changed it too'
    blarc[lo:hi] = wide[clo:chi]; copied.append(m)
layouts = [m for m in members if m.startswith('blyt/')]
if not copied or not all(m in copied or m[5:-6] in ('TitleMenuBG_00',) for m in layouts):   # TitleMenuBG_00: KEEP_ROOT, no edit
    raise SystemExit(f'{TITLE}: expected every layout but TitleMenuBG_00 edited, got {len(copied)} members: {copied}')
dictionary = dictionaries[dict_id]
data = zstd.compress(bytes(blarc), level=19, zstd_dict=dictionary)
assert zstd.decompress(data, zstd_dict=dictionary) == bytes(blarc) and zstd.get_frame_info(data).dictionary_id == dict_id
target = out / TITLE; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
line = '\t'.join([TITLE, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()])
tmp = out / 'manifest.tsv.tmp'; tmp.write_text('\n'.join(manifest + [line]) + '\n'); os.replace(tmp, out / 'manifest.tsv')
print(json.dumps(dict(status='pass', path=TITLE, aspect=size, members=len(copied), layouts=sum(m.startswith('blyt/') for m in copied), size=len(data))))
