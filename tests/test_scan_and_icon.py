"""nx2h/scan.py and nx2h/icon.py on small made-up files: what can be told without keys (plain headers, the file name's
[title id][vN] tags, a NUP's table of contents and meta block). Nothing here is a real game file."""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nx2h import config, icon, scan  # noqa: E402

TOTK, TOTK_UPD = "0100F2C0115B6000", "0100F2C0115B6800"
OOT, OOT_UPD = "0400486024DEC000", "0400486024DEC800"
JPEG = b"\xff\xd8\xff\xe0" + b"made-up jpeg"
PNG = b"\x89PNG\r\n\x1a\n" + b"made-up png"


def pfs0(names) -> bytes:
    """A PFS0 header with these file names and no file data (all that scan.pfs0_names reads)."""
    strings = b"".join(n.encode("utf-8") + b"\0" for n in names)
    entries, off = b"", 0
    for n in names:
        entries += struct.pack("<QQII", 0, 0, off, 0)
        off += len(n.encode("utf-8")) + 1
    return b"PFS0" + struct.pack("<III", len(names), len(strings), 0) + entries + strings


def card() -> bytes:
    return b"\0" * 0x100 + b"HEAD" + b"\0" * 0x100


def tlv(records: dict) -> bytes:
    return b"".join(struct.pack("<HHI", t, 0, len(v)) + v for t, v in records.items())


def localized(pairs) -> bytes:
    """pack_nup.py's localized record: [tag length u8][tag][data length u32][data] ..."""
    return b"".join(bytes([len(t)]) + t.encode() + struct.pack("<I", len(d)) + d for t, d in pairs)


def nup(records) -> bytes:
    """A NUP with a table of contents: records are (title, version, kind, meta block bytes)."""
    head = bytearray(0xB0)
    head[0:4] = b"NUP\x01"
    head[0x80:0x84] = b"TOC\x01"
    struct.pack_into("<I", head, 0x84, len(records))
    off = 0xB0 + icon.PKG.size * len(records)
    table, metas = b"", b""
    for title, version, kind, meta in records:
        title = int(title, 16) if isinstance(title, str) else title
        table += icon.PKG.pack(title, 0, 0, 0, version, kind, 0, 0, 0, off if meta else 0, len(meta))
        metas += meta
        off += len(meta)
    return bytes(head) + table + metas


class Folder(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        for target, value in ((config, "LOCAL"), (icon, "ICONS")):   # nothing of this PC's settings or icon cache
            p = mock.patch.object(target, value, self.dir / "_app" / value.lower())
            p.start()
            self.addCleanup(p.stop)

    def file(self, rel: str, data: bytes, size: int = 0) -> Path:
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
            if size > len(data):
                f.truncate(size)
        return p


class TitleIds(unittest.TestCase):
    def test_base_of_and_role(self):
        self.assertEqual(scan.base_of(TOTK), TOTK)
        self.assertEqual(scan.base_of(TOTK_UPD), TOTK)
        self.assertEqual(scan.base_of("0100F2C0115B7001"), TOTK)   # add-on content: base + 0x1000 + n
        self.assertEqual(scan.base_of(OOT_UPD.lower()), OOT)
        self.assertEqual([scan.role(t) for t in (TOTK, TOTK_UPD, "0100F2C0115B7001")], ["base", "update", "dlc"])


class Identify(Folder):
    def test_nsp_titles_come_from_its_tickets(self):
        p = self.file(f"Zelda [{TOTK}][v0].nsp", pfs0([
            "0123456789abcdef0123456789abcdef.nca", TOTK + "0000000000000010.tik", TOTK_UPD.lower() + "0000000000000010.tik",
            TOTK + "0000000000000010.cert"]))
        item = scan.identify(p)
        self.assertEqual(item.kind, "nsp")
        self.assertEqual(item.titles, [(TOTK, "v0", "base"), (TOTK_UPD, "v0", "update")])
        self.assertEqual(item.base_title, TOTK)
        self.assertEqual(item.note, "")

    def test_nsz_without_tickets_takes_the_name_tags(self):
        p = self.file(f"update [{TOTK_UPD}][v720896].nsz", pfs0(["0123456789abcdef0123456789abcdef.ncz"]))
        item = scan.identify(p)
        self.assertEqual(item.kind, "nsz")
        self.assertEqual(item.titles, [(TOTK_UPD, "v720896", "update")])
        self.assertEqual(item.base_title, TOTK)

    def test_not_a_pfs0(self):
        item = scan.identify(self.file(f"x [{TOTK}].nsp", b"NOPE" + b"\0" * 64))
        self.assertEqual(item.titles, [])
        self.assertTrue(item.note)
        self.assertIsNone(item.base_title)

    def test_card_image_from_the_name_tags(self):
        item = scan.identify(self.file(f"Zelda [{TOTK}][v0].xci", card()))
        self.assertEqual(item.titles, [(TOTK, "v0", "base")])
        item = scan.identify(self.file(f"Zelda [{TOTK}][v196608].xci", card()))   # a card that carries 1.2.1
        self.assertEqual(item.titles, [(TOTK, "v0", "base"), (TOTK_UPD, "v196608", "update")])
        item = scan.identify(self.file(f"zelda [{TOTK.lower()}].xcz", card()))
        self.assertEqual((item.kind, item.titles), ("xcz", [(TOTK, "v0", "base")]))

    def test_card_image_without_tags_or_header(self):
        item = scan.identify(self.file("Zelda.xci", card()))
        self.assertEqual(item.titles, [])
        self.assertTrue(item.note, "without a [title id] tag a card needs keys")
        item = scan.identify(self.file(f"broken [{TOTK}].xci", b"\0" * 0x200))
        self.assertEqual(item.titles, [])
        self.assertTrue(item.note)

    def test_nup_parts_from_its_table_of_contents(self):
        data = nup([(0, 0, 0, b""),                        # the empty base record of an update-only container
                    (OOT, 0, 0, b""), (OOT_UPD, 131072, 1, b""),
                    (0x0400486024DED001, 0, 2, b""),          # add-on content
                    (0x0100000000000819, 5, 4, b"")])         # a system package
        item = scan.identify(self.file("oot.nup", data))
        self.assertEqual(item.titles, [(OOT, "v0", "base"), (OOT_UPD, "v131072", "update"),
                                       ("0400486024DED001", "v0", "dlc")])
        self.assertEqual(item.base_title, OOT)

    def test_not_a_nup(self):
        item = scan.identify(self.file("x.nup", b"NUP\x01" + b"\0" * 0x200))
        self.assertEqual(item.titles, [])
        self.assertTrue(item.note)


class Find(Folder):
    MB = 1 << 20

    def test_walks_subfolders_and_skips_what_is_not_a_game(self):
        games = self.dir / "games"
        self.file(f"games/Zelda [{TOTK}][v0].xci", card(), self.MB)
        self.file(f"games/updates/Zelda [{TOTK_UPD}][v720896].nsz", pfs0(["a.ncz"]), self.MB)
        self.file("games/switch2/oot.NUP", nup([(OOT, 0, 0, b""), (OOT_UPD, 131072, 1, b"")]), self.MB)
        self.file(f"games/small [{TOTK}].nsp", pfs0([]), 1000)              # under 1 MiB: not a game
        self.file("games/notes.txt", b"x", self.MB)
        self.file(f"games/.hidden/x [{TOTK}].xci", card(), self.MB)
        self.file(f"games/$RECYCLE.BIN/x [{TOTK}].xci", card(), self.MB)
        self.file(f"games/prepared/nx2h-stage/x [{TOTK}].xci", card(), self.MB)
        items = scan.find([games])
        rel = [it.path.relative_to(games).as_posix() for it in items]
        self.assertEqual(rel, ["switch2/oot.NUP", "updates/Zelda [0100F2C0115B6800][v720896].nsz",
                               "Zelda [0100F2C0115B6000][v0].xci"])
        by = scan.games_found(items)
        self.assertEqual(sorted(by), [TOTK, OOT])
        self.assertEqual(len(by[TOTK]), 2)

    def test_a_chosen_file_is_identified_too(self):
        p = self.file(f"one/Zelda [{TOTK}][v0].xci", card(), self.MB)
        self.assertEqual([it.path for it in scan.find([p])], [p])
        self.assertEqual(scan.find([self.dir / "missing"]), [])


class Icons(Folder):
    def test_localized_record(self):
        payload = localized([("ZH-Hans", JPEG), ("en-US", PNG), ("zh-hans", b"second, ignored")])
        self.assertEqual(icon._localized(payload), {"zh-hans": JPEG, "en-us": PNG})
        self.assertEqual(icon._localized(b""), {})
        cut = localized([("en-US", PNG)]) + bytes([5]) + b"ja-JP"   # a tag without its length: stops there
        self.assertEqual(icon._localized(cut), {"en-us": PNG})

    def test_nup_icon_and_name_prefer_chinese(self):
        names = localized([("en-US", "The Legend of Zelda: Ocarina of Time".encode()),
                           ("zh-Hans", "塞尔达传说 时之笛".encode())])
        meta = tlv({icon.T_LNAMES: names, icon.T_LICONS: localized([("en-US", PNG), ("zh-Hans", JPEG)])})
        p = self.file("oot.nup", nup([(OOT, 0, 0, meta), (OOT_UPD, 131072, 1, b"")]))
        self.assertEqual(icon.nup_icon(p, OOT), JPEG)
        self.assertEqual(icon.nup_name(p, OOT), "塞尔达传说 时之笛")
        self.assertIsNone(icon.nup_icon(p, TOTK), "another game's NUP gives nothing")

    def test_nup_meta_numbered_from_zero_and_the_update_wins(self):
        base = tlv({0: b"META", icon.T_ICON - 1: PNG, icon.T_NAME - 1: b"Old name"})   # an older packer: from 0
        update = tlv({icon.T_NAME: b"New name"})
        p = self.file("oot.nup", nup([(OOT, 0, 0, base), (OOT_UPD, 131072, 1, update)]))
        self.assertEqual(icon.nup_icon(p, OOT), PNG)
        self.assertEqual(icon.nup_name(p, OOT), "New name")

    def test_a_nup_without_meta_has_no_icon(self):
        p = self.file("za.nup", nup([(0x0100F43008C44000, 0, 0, b"")]))
        self.assertIsNone(icon.nup_icon(p, "0100F43008C44000"))
        self.assertEqual(icon.nup_name(p, "0100F43008C44000"), "")

    def test_from_files_takes_a_nup_icon_without_keys(self):
        meta = tlv({icon.T_ICON: JPEG})
        p = self.file("oot.nup", nup([(OOT, 0, 0, meta)]))
        item = scan.identify(p)
        self.assertIsNone(icon.cached(OOT))
        self.assertTrue(icon.from_files(OOT.lower(), [item], None))
        self.assertEqual(icon.cached(OOT), icon.ICONS / f"{OOT}.jpg")
        self.assertEqual(icon.cached(OOT).read_bytes(), JPEG)

    def test_without_keys_a_card_gives_no_icon(self):
        item = scan.identify(self.file(f"Zelda [{TOTK}][v0].xci", card()))
        self.assertFalse(icon.from_files(TOTK, [item], None))
        self.assertFalse(icon.from_files(TOTK, [item], self.dir / "no-keys-here"))

    def test_sources_order(self):
        a = scan.Item(Path("u.nsz"), "nsz", [(TOTK_UPD, "v1", "update")])
        b = scan.Item(Path("b.nsp"), "nsp", [(TOTK, "v0", "base")])
        c = scan.Item(Path("c.xci"), "xci", [(TOTK, "v0", "base")])
        d = scan.Item(Path("u.nsp"), "nsp", [(TOTK_UPD, "v1", "update")])
        e = scan.Item(Path("o.nup"), "nup", [(OOT, "v0", "base")])
        self.assertEqual([it.path.name for it in icon.sources([a, b, c, d, e])], ["c.xci", "b.nsp", "u.nsp", "u.nsz"])

    def test_is_image(self):
        self.assertTrue(icon._is_image(JPEG))
        self.assertTrue(icon._is_image(PNG))
        self.assertFalse(icon._is_image(b"<html>"))
        self.assertFalse(icon._is_image(b""))

    def test_settings_stay_in_the_temporary_folder(self):
        self.assertEqual(config.local(), {})
        self.assertFalse((self.dir / "_app" / "local").exists())


if __name__ == "__main__":
    unittest.main()
