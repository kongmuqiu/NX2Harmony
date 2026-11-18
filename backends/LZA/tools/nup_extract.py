#!/usr/bin/env python3
"""Unpack a .nup game package (the player's own Pokémon Legends: Z-A files) into plain ExeFS / RomFS folders.

A .nup holds already decrypted, extracted content: one part per title (base program, update, add-on content), each a
folder tree whose files are stored raw or zstd-compressed, with the SHA-256 of every unpacked file. Layout (all
little-endian), as read from the file itself:

  0x00  "NUP\\x01", u32 flags, u32 TOC offset (0x80), u32 0
  0x10  u64 data start of the table area, 32-byte digest, u64 first data offset
  0x40  u64 total file size, u32 part count, ...
  0x80  "TOC\\x01", u32 parts, u32 0x18, u32 0; u64 file count, u64 string table offset (from 0x80), u64 string bytes
  0xb0  parts, 0x40 each: u64 title, i32 root dir, i32 ExeFS dir (-1 none), i32 RomFS dir, u32 version,
        u32 type (0 program, 1 update, 2 add-on), u32, u64 metadata offset, u64 metadata size (an NPDM copy and names)
  then  dirs, 0x18 each: u32 name offset, u32 name length, u32 first child, u32 child count, u64 0
        files, 0x50 each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size, 32-byte SHA-256,
        u32 1 = zstd frame / 0 = raw, 12 bytes 0
        children: u32 node indexes (directories first, then files), then the string table

    python tools/nup_extract.py <file.nup> --list
    python tools/nup_extract.py <file.nup> --out <folder> [--parts program,update,addon]

Writes <folder>/<title>-v<version>-<type>/{exefs,romfs}/..., checks every SHA-256 while writing (a mismatch stops
with the file name), and skips files already unpacked with the right size and a matching digest record
(<folder>/<part>/.digests). Nothing here is game data; what it writes is, and stays out of any repository."""
import argparse, hashlib, json, os, struct, sys, time
from pathlib import Path

try:
    import zstandard
except ImportError:  # pragma: no cover
    sys.exit("needs the zstandard module (pip install zstandard)")

TYPES = {0: 'program', 1: 'update', 2: 'addon'}
CHUNK = 8 << 20


class Nup:
    def __init__(self, path):
        self.path = Path(path)
        self.f = open(self.path, 'rb')
        h = self.f.read(0x80)
        if h[:4] != b'NUP\x01':
            raise SystemExit(f'{path}: not a NUP package (magic {h[:4]!r})')
        toc = struct.unpack_from('<I', h, 8)[0]
        self.total = struct.unpack_from('<Q', h, 0x40)[0]
        if self.total != self.path.stat().st_size:
            raise SystemExit(f'{path}: header says {self.total} bytes, the file has {self.path.stat().st_size} (incomplete download?)')
        self.f.seek(toc)
        t = self.f.read(0x30)
        if t[:4] != b'TOC\x01':
            raise SystemExit(f'{path}: no TOC at {toc:#x}')
        nparts = struct.unpack_from('<I', t, 4)[0]
        nfiles, str_off, str_len = struct.unpack_from('<QQQ', t, 0x10)
        self.f.seek(toc + 0x30)
        pe = self.f.read(0x40 * nparts)
        self.parts = []
        for i in range(nparts):
            title, root, exefs, romfs, version, ptype = struct.unpack_from('<QiiiII', pe, 0x40 * i)
            moff, msize = struct.unpack_from('<QQ', pe, 0x40 * i + 0x20)
            self.parts.append(dict(title=f'{title:016X}', root=root, exefs=exefs, romfs=romfs, version=version,
                                   type=TYPES.get(ptype, str(ptype)), meta_offset=moff, meta_size=msize))
        nodes_at = toc + 0x30 + 0x40 * nparts
        strings_at = toc + str_off
        self.f.seek(nodes_at)
        area = self.f.read(strings_at - nodes_at + str_len)
        self.strings = area[strings_at - nodes_at:]
        # directories: 0x18 each up to the file table; files: 0x50 each; then the children (one per non-root node)
        # ndirs is fixed by the sizes: 0x18*d + 0x50*f + 4*(d+f-parts) == strings_at - nodes_at
        span = strings_at - nodes_at
        ndirs = None
        for d in range(1, 4096):
            if 0x18 * d + 0x50 * nfiles + 4 * (d + nfiles - nparts) == span:
                ndirs = d
                break
        if ndirs is None:
            raise SystemExit(f'{path}: table sizes do not add up (files {nfiles}, span {span:#x})')
        self.dirs = [struct.unpack_from('<IIII', area, 0x18 * i) for i in range(ndirs)]
        fa = 0x18 * ndirs
        self.files = []
        for i in range(nfiles):
            o = fa + 0x50 * i
            no, nl, size, off, stored = struct.unpack_from('<IIQQQ', area, o)
            sha = area[o + 0x20:o + 0x40].hex()
            flag = struct.unpack_from('<I', area, o + 0x40)[0]
            self.files.append(dict(name=self.name(no, nl), size=size, offset=off, stored=stored, sha256=sha, zstd=flag == 1))
        ka = fa + 0x50 * nfiles
        self.kids = struct.unpack_from(f'<{ndirs + nfiles - nparts}I', area, ka)

    def name(self, off, n):
        return self.strings[off:off + n].decode('utf-8')

    def walk(self, node, prefix=()):
        """(relative path, file record) for every file under a directory node"""
        no, nl, first, count = self.dirs[node]
        for k in self.kids[first:first + count]:
            if k < len(self.dirs):
                d = self.dirs[k]
                yield from self.walk(k, prefix + (self.name(d[0], d[1]),))
            else:
                rec = self.files[k - len(self.dirs)]
                yield '/'.join(prefix + (rec['name'],)), rec

    def part_files(self, part):
        return list(self.walk(part['root']))

    def part_dir(self, part):
        return f"{part['title']}-v{part['version']}-{part['type']}"

    def extract_file(self, rec, dst):
        tmp = dst.with_name(dst.name + '.part')
        h = hashlib.sha256()
        self.f.seek(rec['offset'])
        left = rec['stored']
        with open(tmp, 'wb') as out:
            if rec['zstd']:
                class Slice:
                    def __init__(s, f, n): s.f, s.n = f, n
                    def read(s, k=-1):
                        k = s.n if k < 0 else min(k, s.n)
                        b = s.f.read(k); s.n -= len(b); return b
                reader = zstandard.ZstdDecompressor().stream_reader(Slice(self.f, left), read_size=CHUNK)
                while True:
                    b = reader.read(CHUNK)
                    if not b:
                        break
                    h.update(b); out.write(b)
            else:
                while left:
                    b = self.f.read(min(CHUNK, left))
                    if not b:
                        raise SystemExit(f'{dst}: package ends early')
                    left -= len(b); h.update(b); out.write(b)
        if tmp.stat().st_size != rec['size'] or h.hexdigest() != rec['sha256']:
            tmp.unlink()
            raise SystemExit(f'{dst}: size or SHA-256 does not match the package record')
        os.replace(tmp, dst)
        return h.hexdigest()


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nup', type=Path)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--parts', default='program,update,addon')
    a = ap.parse_args()
    n = Nup(a.nup)
    if a.list or not a.out:
        for p in n.parts:
            files = n.part_files(p)
            total = sum(r['size'] for _, r in files)
            print(f"{n.part_dir(p)}: {len(files)} files, {total / 2**30:.2f} GiB")
            for rel, r in files:
                print(f"  {r['size']:>14,d}  {'zstd' if r['zstd'] else 'raw '}  {rel}")
        return
    want = set(a.parts.split(','))
    for p in n.parts:
        if p['type'] not in want:
            continue
        base = a.out / n.part_dir(p)
        base.mkdir(parents=True, exist_ok=True)
        rec_path = base / '.digests'
        try:
            digests = json.loads(rec_path.read_text()) if rec_path.is_file() else {}
        except ValueError:   # a record cut off by an earlier run: the files themselves are checked below
            digests = {}
        files = n.part_files(p)

        def save():   # whole, through a temporary file: a stopped run never leaves an empty or cut record
            tmp = rec_path.with_name('.digests.part')
            tmp.write_text(json.dumps(digests, indent=1, sort_keys=True))
            os.replace(tmp, rec_path)
        # The record is written every few seconds, not after every file: rewritten per file it grew with the part
        # (Ocarina of Time's 186,588 files: 19 files a second at 70,000, hours to go).
        saved = time.monotonic()
        for i, (rel, r) in enumerate(files, 1):
            dst = base / rel
            if dst.is_file() and dst.stat().st_size == r['size']:
                if digests.get(rel) == r['sha256']:
                    continue
                if rel not in digests and file_sha256(dst) == r['sha256']:   # written before the record was saved
                    digests[rel] = r['sha256']
                    continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            print(f"[{n.part_dir(p)} {i}/{len(files)}] {rel} ({r['size'] / 2**20:.1f} MiB)", flush=True)
            digests[rel] = n.extract_file(r, dst)
            if time.monotonic() - saved > 5:
                save(); saved = time.monotonic()
        save()
        n.f.seek(p['meta_offset'])
        (base / '.meta.bin').write_bytes(n.f.read(p['meta_size']))
        (base / '.part.json').write_text(json.dumps(p, indent=1))
        print(f"{n.part_dir(p)}: {len(files)} files verified")


if __name__ == '__main__':
    main()
