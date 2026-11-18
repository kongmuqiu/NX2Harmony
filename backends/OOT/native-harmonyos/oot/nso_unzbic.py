"""A Switch 2 module (system 22.0.0+ NSO, flag bit 7: segments in ZBIC, Nintendo's zstd variant) as a plain NSO with its
segments stored uncompressed, for the rewriter (d2r/patch_nso.py and nsoimg.py read LZ4 or uncompressed segments only).

Only the storage changes: every segment is decompressed with the zbic package (the decoder seedds/nxnative's dm/tools/nso.py
uses, MIT; pip install zbic) and checked against the size in the header and, when its hash flag is set, its SHA-256; the
segments are then laid out one after another from 0x100, their file offsets and stored sizes set to match, and the
compression flags (bits 0-2 and 7) cleared. Memory offsets, sizes, the build ID, the hash flags and hashes (over the
uncompressed bytes) and every other header field stay, so the module is the same image.

    python nso_unzbic.py <in NSO> <out NSO>      (prints what it did; an NSO without ZBIC segments is copied as it is)
"""
import hashlib, struct, sys

NAMES = ('text', 'rodata', 'data')
ZBIC = 1 << 7


def unzbic(d):
    if d[:4] != b'NSO0' or len(d) < 0x100:
        raise SystemExit('not an NSO')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + 0x10 * i) for i in range(3)]   # file offset, memory offset, size
    stored = struct.unpack_from('<III', d, 0x60)
    if not flags & 7:
        return bytes(d), 'not compressed'
    out = bytearray(d[:0x100])
    at = 0x100
    for i, ((fo, mo, sz), c) in enumerate(zip(segs, stored)):
        raw = d[fo:fo + (c if flags >> i & 1 else sz)]
        if flags >> i & 1:
            if flags & ZBIC:
                import zbic
                raw = zbic.decompress(bytes(raw))
            else:
                import lz4.block
                raw = lz4.block.decompress(raw, uncompressed_size=sz)
        if len(raw) != sz:
            raise SystemExit(f'{NAMES[i]}: {len(raw):#x} bytes after decompression, the header says {sz:#x}')
        if flags >> (3 + i) & 1 and hashlib.sha256(raw).digest() != d[0xa0 + 0x20 * i:0xc0 + 0x20 * i]:
            raise SystemExit(f'{NAMES[i]}: SHA-256 does not match the header')
        struct.pack_into('<III', out, 0x10 + 0x10 * i, at, mo, sz)
        struct.pack_into('<I', out, 0x60 + 4 * i, sz)
        out += raw
        at += sz
    struct.pack_into('<I', out, 0xc, flags & ~(7 | ZBIC))
    return bytes(out), f'{"ZBIC" if flags & ZBIC else "LZ4"} segments stored uncompressed ({len(d):,} -> {len(out):,} bytes)'


if __name__ == '__main__':
    src, dst = sys.argv[1:3]
    data, what = unzbic(open(src, 'rb').read())
    open(dst, 'wb').write(data)
    print(f'{src}: {what}')
