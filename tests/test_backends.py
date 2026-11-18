"""backends/: the game backends' sources as this repository carries them (packaging/backend_import.py), what
backends/SOURCES.tsv says about where they come from, and the prebuilt libraries."""
from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
for p in (REPO, REPO / "packaging"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import backend_import as bi  # noqa: E402
import full_bundle as fb  # noqa: E402
from nx2h.backends import MARKERS  # noqa: E402
from nx2h.backends.common import BaseBackend  # noqa: E402

B = REPO / "backends"
SHA = re.compile(r"[0-9a-f]{40}")
# a Windows path into a user's or a porter's folders, in any spelling (a build path inside a library)
DRIVE_PATH = re.compile(rb"(?i)(?<![a-z])[a-z]:[\\/]+(?:users|github|harmonyos|nx2h[\w-]*|src)[\\/]")


def tracked(prefix: str) -> list:
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--",
                          prefix], capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def rows() -> dict:
    return {r[0]: r for r in bi.read_sources()}


class Folders(unittest.TestCase):
    def test_every_backend_is_here(self):
        for name, (marker, folder) in MARKERS.items():
            with self.subTest(name):
                root = B / folder
                self.assertTrue((root / marker).is_file(), marker)
                self.assertTrue((root / "build-profile.template.json5").is_file())
                module = root / "entry" / "build-profile.json5"
                self.assertTrue(module.is_file(), "hvigor needs the module build file")
                self.assertNotRegex(module.read_text(encoding="utf-8"), r'"(storeFile|storePassword|certpath)"')
                if shutil.which("git") and (REPO / ".git").exists():   # a build writes one for its duration (ignored)
                    self.assertEqual(tracked(f"backends/{folder}/build-profile.json5"), [], "a signing configuration")

    def test_only_what_the_package_may_carry(self):
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        for folder in (f for _, f in MARKERS.values()):
            for rel in tracked(f"backends/{folder}/"):
                inner = rel[len(f"backends/{folder}/"):]
                with self.subTest(rel):
                    self.assertTrue(fb._allowed(Path(inner)), "full_bundle._allowed")
                    self.assertFalse(any(inner == s or inner.startswith(s) for s in bi.SKIP), "the standalone "
                                     "project's own AI-install instructions and hooks stay out")

    def test_nothing_here_is_hidden_by_an_ignore_rule(self):
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--ignored", "--exclude-standard",
                              "--", "backends/"], capture_output=True, check=True).stdout
        self.assertEqual([p for p in out.decode("utf-8").split("\0") if p], [])

    def test_every_picture_is_listed(self):
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        ok = fb._pictures_ok()
        for rel in tracked("backends/"):   # not the ignored clones and build outputs beside them
            if bi._picture(rel) and (REPO / rel).is_file():
                with self.subTest(rel):
                    self.assertIn(hashlib.sha256((REPO / rel).read_bytes()).hexdigest(), ok)


class Sources(unittest.TestCase):
    def test_a_row_for_every_folder(self):
        r = rows()
        for _, folder in MARKERS.values():
            with self.subTest(folder):
                self.assertIn(folder, r)
                self.assertTrue(SHA.fullmatch(r[folder][3]) or r[folder][3].startswith("snapshot-"), r[folder][3])
                patch = r[folder][5]   # kept on the maintainer's PC only (backends/README.md)
                self.assertIn(patch, ("-", f"patches/{folder}.patch"))
        for p in (B / "patches").glob("*.patch") if (B / "patches").is_dir() else []:
            self.assertIn(p.stem, r, f"{p.name} without a row")

    def test_the_patches_stay_out_of_git(self):
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        self.assertEqual(tracked("backends/patches/"), [], "they quote the projects' original lines")

    def test_upstreams_are_pinned(self):
        r = rows()
        for name in ("nxnative", "totk-android"):
            with self.subTest(name):
                self.assertIn(name, r)
                self.assertRegex(r[name][2], r"^https://github\.com/")
                self.assertTrue(SHA.fullmatch(r[name][3]), r[name][3])
        for _, folder in MARKERS.values():
            pin = (B / folder / "native-harmonyos" / "UPSTREAM_PIN").read_text(encoding="utf-8").split()[0].lower()
            self.assertEqual(pin, r["totk-android"][3], folder)

    def test_no_local_path_in_sources(self):
        text = (B / "SOURCES.tsv").read_bytes()
        self.assertIsNone(DRIVE_PATH.search(text))
        self.assertNotRegex(text.decode("utf-8"), r"(?i)\b[a-z]:\\")


class Prebuilt(unittest.TestCase):
    def test_the_libraries_and_their_licences(self):
        root = B / "prebuilt-ohos"
        for lib in BaseBackend.PREBUILT.values():
            self.assertTrue((root / lib).is_file(), lib)
        for rel in ("README.txt", "native-opus/source/opus-1.6.1/COPYING", "native-opus/source/opus-1.6.1/include/opus.h",
                    "native-vpx/src/libvpx/LICENSE", "native-vpx/src/libvpx/PATENTS", "native-vpx/ohos/include/vpx/vpx_decoder.h"):
            self.assertTrue((root / rel).is_file(), rel)

    def test_no_build_path_inside(self):
        for p in sorted((B / "prebuilt-ohos").rglob("*")):
            if p.is_file():
                with self.subTest(p.name):
                    self.assertIsNone(DRIVE_PATH.search(p.read_bytes()))


if __name__ == "__main__":
    unittest.main()
