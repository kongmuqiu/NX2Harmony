"""games.tsv (nx2h/config.py games()) and each game's HarmonyOS bundle name (nx2h/bundle.py). Settings go to a
nx2h.local.json in a temporary folder, never to the checkout's own."""
from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nx2h import bundle, config  # noqa: E402
from nx2h.backends import BACKENDS, MARKERS  # noqa: E402

PC_PART = re.compile(r"^u[0-9a-z]{6}$")


class TempSettings(unittest.TestCase):
    """config.LOCAL points at an empty temporary nx2h.local.json for the test."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.local = Path(tmp.name) / "nx2h.local.json"
        p = mock.patch.object(config, "LOCAL", self.local)
        p.start()
        self.addCleanup(p.stop)

    def saved(self) -> dict:
        return json.loads(self.local.read_text(encoding="utf-8"))


class GamesTable(TempSettings):
    def test_three_games_with_their_slugs_backends_and_statuses(self):
        rows = {r["slug"]: r for r in config.games()}
        self.assertEqual(set(rows), {"totk", "za", "oot"})
        expect = {"totk": ("0100F2C0115B6000", "totk_v143", "1.4.3", "已验证"),
                  "za": ("0100F43008C44000", "za_202", "2.0.2", "优化中"),
                  "oot": ("0400486024DEC000", "oot_v131072", "v131072", "优化中")}
        for slug, (title, backend, version, status) in expect.items():
            with self.subTest(slug=slug):
                r = rows[slug]
                self.assertEqual((r["title_id"], r["backend"], r["version"], r["status"]),
                                 (title, backend, version, status))
                self.assertIn(r["status"], config.BUILDABLE)
                self.assertTrue(r["name"] and r["needs"])

    def test_every_row_has_every_column(self):
        lines = (REPO / "games.tsv").read_text(encoding="utf-8").splitlines()
        data = [l for l in lines if l.strip() and not l.startswith("#")]
        self.assertEqual(len(data), len(config.games()), "a row with a missing or extra tab is dropped silently")
        header = next(l[2:].split("\t") for l in lines if l.startswith("# title_id"))
        self.assertEqual(header, ["title_id", "name", "slug", "version", "update", "backend", "status", "needs"])

    def test_title_ids_are_base_games_and_unique(self):
        titles = [r["title_id"] for r in config.games()]
        self.assertEqual(len(titles), len(set(titles)))
        for t in titles:
            with self.subTest(t):
                self.assertRegex(t, r"^[0-9A-F]{16}$")
                self.assertTrue(t.endswith("000"), "a base game's title id ends in 000")

    def test_every_backend_is_known_everywhere(self):
        for r in config.games():
            with self.subTest(r["backend"]):
                self.assertIn(r["backend"], BACKENDS)
                self.assertIn(r["backend"], MARKERS)
                self.assertIn(r["backend"], config.DEFAULT_BACKENDS)
        self.assertEqual(set(BACKENDS), set(MARKERS))

    def test_backend_classes_load_without_a_phone(self):
        from nx2h.backends import load
        for name in BACKENDS:
            with self.subTest(name):
                self.assertEqual(load(name).__name__, "Backend")
        with self.assertRaises(KeyError):
            load("no_such_backend")


class BundleNames(TempSettings):
    def guid(self, value: str):
        p = mock.patch.object(bundle, "_machine_guid", return_value=value)
        p.start()
        self.addCleanup(p.stop)

    def test_valid_follows_the_harmonyos_rule(self):
        good = ["nx2h.uk3f9a2.totk", "nx2h.u000000.oot", "com.a.b", "a.b_c.d9", "x" * 61 + "." + "y" * 65]
        bad = ["a.b", "nx2h", "nx2h.1abc.totk", "nx2h._u.totk", "nx2h.u-1.totk", "nx2h..totk", ".nx2h.totk",
               "nx2h.totk.", "nx2h.uk3f9a2.tö", "x" * 62 + "." + "y" * 65, "nx2h.uk3f9a2.totk "]
        for name in good:
            with self.subTest(good=name):
                self.assertTrue(bundle.valid(name))
        for name in bad:
            with self.subTest(bad=name):
                self.assertFalse(bundle.valid(name))

    def test_pc_part_from_the_machine_guid(self):
        self.guid("01234567-89ab-cdef-0123-456789abcdef")
        a = bundle.pc_part()
        self.assertRegex(a, PC_PART)
        self.assertEqual(a, bundle.pc_part(), "the same PC gives the same part")
        self.assertFalse(self.local.exists(), "a PC part made from the MachineGuid is not stored")
        self.guid("fedcba98-7654-3210-fedc-ba9876543210")
        self.assertRegex(bundle.pc_part(), PC_PART)
        self.assertNotEqual(a, bundle.pc_part(), "another PC gives another part")

    def test_pc_part_without_a_machine_guid_is_drawn_once_and_kept(self):
        self.guid("")
        a = bundle.pc_part()
        self.assertRegex(a, PC_PART)
        self.assertEqual(self.saved()["bundle_uid"], a)
        self.assertEqual(bundle.pc_part(), a)

    def test_slug_of(self):
        self.assertEqual(bundle.slug_of("totk", "0100F2C0115B6000"), "totk")
        self.assertEqual(bundle.slug_of("Z-A 2", "0100F43008C44000"), "za2")
        self.assertEqual(bundle.slug_of("", "0100F2C0115B6000"), "g0100f2c0115b6000")
        self.assertEqual(bundle.slug_of("1up", "0100000000000000"), "g1up")
        self.assertEqual(bundle.slug_of("塞尔达", "0400486024DEC000"), "g0400486024dec000")

    def test_every_game_gets_its_own_valid_name_and_keeps_it(self):
        self.guid("01234567-89ab-cdef-0123-456789abcdef")
        part = bundle.pc_part()
        names = {}
        for r in config.games():
            with self.subTest(r["slug"]):
                name = bundle.for_game(r["title_id"].lower(), r["slug"])
                self.assertEqual(name, f"nx2h.{part}.{r['slug']}")
                self.assertTrue(bundle.valid(name))
                self.assertTrue(7 <= len(name.encode("utf-8")) <= 127)
                for seg in name.split("."):
                    self.assertRegex(seg, r"^[A-Za-z][0-9A-Za-z_]*$")
                names[r["title_id"]] = name
        self.assertEqual(len(set(names.values())), len(names))
        self.assertEqual(self.saved()["bundles"], names, "kept in nx2h.local.json under the upper-case title id")
        self.guid("fedcba98-7654-3210-fedc-ba9876543210")   # the app copied to another PC keeps the names it made
        for title, name in names.items():
            self.assertEqual(bundle.for_game(title, "whatever"), name)

    def test_a_second_game_with_the_same_slug_gets_a_number(self):
        self.guid("01234567-89ab-cdef-0123-456789abcdef")
        part = bundle.pc_part()
        first = bundle.for_game("0100AAAA00000000", "game")
        second = bundle.for_game("0100BBBB00000000", "game")
        third = bundle.for_game("0100CCCC00000000", "game")
        self.assertEqual(first, f"nx2h.{part}.game")
        self.assertEqual(second, f"nx2h.{part}.game2")
        self.assertEqual(third, f"nx2h.{part}.game3")

    def test_a_remembered_name_is_used_as_it_is(self):
        self.local.write_text(json.dumps({"bundles": {"0100F2C0115B6000": "nx2h.uk3f9a2.totk"}}), encoding="utf-8")
        self.guid("01234567-89ab-cdef-0123-456789abcdef")
        self.assertEqual(bundle.for_game("0100f2c0115b6000", "totk"), "nx2h.uk3f9a2.totk")


if __name__ == "__main__":
    unittest.main()
