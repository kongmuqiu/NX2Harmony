"""packaging/backend_import.py: a backend project's files coming into backends/<folder>, NX2's own changes kept as a
patch, and what is refused on the way. The project and this repository are made-up git repositories in a temporary
folder; the packing user's home folder and git e-mail and this PC's folders are faked, so nothing depends on this PC."""
from __future__ import annotations

import contextlib
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
for p in (REPO, REPO / "packaging"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import backend_import as bi  # noqa: E402
import full_bundle as fb  # noqa: E402

ICON = b"\x89PNG\r\n\x1a\n" + b"\0" * 24 + b"neutral icon"
ART = b"\x89PNG\r\n\x1a\n" + b"\0" * 24 + b"a game's official art"
MARKER = "native-harmonyos/za/rewrite_za.py"   # what makes a folder the za_202 backend


@unittest.skipUnless(shutil.which("git"), "git is not installed")
class Import(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        empty = self.tmp / "gitconfig"
        empty.write_text("", encoding="utf-8")
        env = {"GIT_CONFIG_GLOBAL": str(empty), "GIT_CONFIG_NOSYSTEM": "1"}
        p = mock.patch.dict(os.environ, env)
        p.start()
        self.addCleanup(p.stop)
        self.repo = self.tmp / "nx2"           # this repository, faked
        self.project = self.tmp / "LZA-project"
        for r in (self.repo, self.project):
            r.mkdir()
            self.git(r, "init", "-q")
        listed = self.tmp / "pictures.tsv"
        listed.write_text(hashlib.sha256(ICON).hexdigest() + "\ttest\tneutral\n", encoding="utf-8")
        for target, name, value in (
                (bi, "REPO", self.repo), (bi, "BACKENDS", self.repo / "backends"),
                (bi, "PATCHES", self.repo / "backends" / "patches"),
                (bi, "SOURCES", self.repo / "backends" / "SOURCES.tsv"), (bi, "pc_folders", lambda: []),
                (fb, "PICTURES_OK", listed),
                (fb, "_private", lambda: {"the packing user's folder": re.compile(rb"(?i)users[\\/]+packer\b")})):
            p = mock.patch.object(target, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.put(self.project, MARKER, "print('rewrite')\r\n")
        self.put(self.project, "native-harmonyos/ohos_env.py", 'BUNDLE = "nx2h.uk3f9a2.za"\n')
        self.put(self.project, "entry/src/main/resources/base/media/icon.png", ICON)
        self.put(self.project, ".claude/skills/x/SKILL.md", "the standalone project's install skill\n")
        self.put(self.project, "AGENTS.md", "the standalone project's agent notes\n")
        self.put(self.project, "native-harmonyos/local.ohos.env", "TARGET=x\n")
        self.rev = self.commit(self.project)

    def git(self, root: Path, *args) -> str:
        return subprocess.run(["git", "-C", str(root), *args], capture_output=True, check=True, text=True).stdout

    def commit(self, root: Path) -> str:
        self.git(root, "add", "-A", "-f")
        self.git(root, "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-qm", "x", "--no-verify")
        return self.git(root, "rev-parse", "HEAD").strip()

    def put(self, root: Path, rel: str, data) -> None:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data.encode("utf-8") if isinstance(data, str) else data)

    def run_tool(self, *extra) -> int:
        with contextlib.redirect_stdout(io.StringIO()):
            return bi.main(["za_202", str(self.project), *extra])

    def here(self, rel: str) -> Path:
        return self.repo / "backends" / "LZA" / rel

    def test_a_git_project_comes_in_at_its_commit(self):
        self.put(self.project, "native-harmonyos/later.py", "not committed\n")
        self.assertEqual(self.run_tool("--rev", self.rev), 0)
        self.assertEqual(self.here(MARKER).read_bytes(), b"print('rewrite')\n", "LF line ends")
        self.assertEqual(self.here("native-harmonyos/ohos_env.py").read_bytes(), b'BUNDLE = "nx2h.u000000.za"\n')
        for rel in ("native-harmonyos/later.py", "AGENTS.md", ".claude/skills/x/SKILL.md",
                    "native-harmonyos/local.ohos.env"):
            self.assertFalse(self.here(rel).exists(), rel)
        sources = (self.repo / "backends" / "SOURCES.tsv").read_text(encoding="utf-8")
        self.assertIn(f"LZA\tza_202\t", sources)
        self.assertIn(f"\t{self.rev}\t", sources)
        self.assertEqual(self.run_tool("--rev", self.rev, "--check"), 0)

    def test_nx2s_changes_survive_a_new_import(self):
        self.run_tool()
        self.put(self.repo, "backends/LZA/native-harmonyos/ohos_env.py", 'BUNDLE = "nx2h.u000000.za"\nNX2 = 1\n')
        self.put(self.repo, "backends/LZA/native-harmonyos/nx2_only.py", "added by NX2\n")
        self.assertEqual(self.run_tool("--save-patch"), 0)
        self.commit(self.repo)
        patch = (self.repo / "backends" / "patches" / "LZA.patch").read_text(encoding="utf-8")
        self.assertIn("+NX2 = 1", patch)
        self.assertIn("nx2_only.py", patch)
        self.put(self.project, MARKER, "print('rewrite v2')\n")   # the project moves on
        self.commit(self.project)
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.here(MARKER).read_text(encoding="utf-8"), "print('rewrite v2')\n")
        self.assertIn("NX2 = 1", self.here("native-harmonyos/ohos_env.py").read_text(encoding="utf-8"))
        self.assertTrue(self.here("native-harmonyos/nx2_only.py").is_file())

    def save_nx2_change(self):
        """Import, change one backend file the way NX2 would, commit it here and record it as the patch."""
        self.run_tool()
        self.put(self.repo, "backends/LZA/native-harmonyos/ohos_env.py", 'BUNDLE = "nx2h.u000000.za"\nNX2 = 1\n')
        self.commit(self.repo)
        self.run_tool("--save-patch")

    def test_a_patch_that_no_longer_applies_is_refused_then_merged(self):
        self.save_nx2_change()
        self.commit(self.repo)
        self.put(self.project, "native-harmonyos/ohos_env.py", 'BUNDLE = "other"\n')
        self.put(self.project, MARKER, "print('rewrite v3')\n")
        self.commit(self.project)
        with self.assertRaises(SystemExit) as e:
            self.run_tool()
        self.assertIn("--merge", str(e.exception.code))
        self.assertEqual(self.run_tool("--merge"), 0)
        self.assertEqual(self.here(MARKER).read_text(encoding="utf-8"), "print('rewrite v3')\n", "the parts that apply")
        self.assertTrue(self.here("native-harmonyos/ohos_env.py.rej").is_file(), "the rest beside its file")
        with self.assertRaises(SystemExit):   # not before the .rej files are resolved
            self.run_tool("--save-patch")

    def test_a_patch_the_project_took_over_is_dropped(self):
        self.save_nx2_change()
        self.commit(self.repo)
        self.put(self.project, "native-harmonyos/ohos_env.py", 'BUNDLE = "nx2h.uk3f9a2.za"\nNX2 = 1\n')
        self.commit(self.project)
        self.assertEqual(self.run_tool(), 0)
        self.assertFalse((self.repo / "backends" / "patches" / "LZA.patch").exists())
        self.assertEqual(next(r for r in bi.read_sources() if r[0] == "LZA")[5], "-")

    def test_changes_that_are_not_committed_are_not_written_over(self):
        self.run_tool()
        self.commit(self.repo)
        self.put(self.repo, "backends/LZA/native-harmonyos/ohos_env.py", "work in progress\n")
        with self.assertRaises(SystemExit) as e:
            self.run_tool()
        self.assertIn("not committed", str(e.exception.code))
        self.assertEqual(self.here("native-harmonyos/ohos_env.py").read_text(encoding="utf-8"), "work in progress\n")
        self.assertEqual(self.run_tool("--force"), 0)

    def test_nx2s_own_pictures_stay(self):
        self.run_tool()
        other = ICON + b" (NX2's)"
        listed = (hashlib.sha256(ICON).hexdigest() + "\ttest\tneutral\n" + hashlib.sha256(other).hexdigest()
                  + "\ttest\tneutral too\n")
        fb.PICTURES_OK.write_text(listed, encoding="utf-8")
        self.put(self.repo, "backends/LZA/entry/src/main/resources/base/media/icon.png", other)
        self.put(self.repo, "backends/LZA/entry/src/main/resources/base/media/nx2.png", other)
        self.commit(self.repo)
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.here("entry/src/main/resources/base/media/icon.png").read_bytes(), other)
        self.assertTrue(self.here("entry/src/main/resources/base/media/nx2.png").is_file())

    def test_official_art_never_comes_in(self):
        self.run_tool()
        self.commit(self.repo)
        self.put(self.project, "entry/src/main/resources/base/media/icon.png", ART)
        self.commit(self.project)
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.here("entry/src/main/resources/base/media/icon.png").read_bytes(), ICON,
                         "NX2's listed picture stays")
        self.put(self.project, "entry/src/main/resources/base/media/new.png", ART)
        self.commit(self.project)
        with self.assertRaises(SystemExit) as e:
            self.run_tool()
        self.assertIn("new.png", str(e.exception.code))

    def test_private_content_is_refused(self):
        self.put(self.project, "native-harmonyos/paths.py", "LOG = r'C:\\Users\\packer\\x.log'\n")
        self.commit(self.project)
        with self.assertRaises(SystemExit) as e:
            self.run_tool()
        self.assertIn("native-harmonyos/paths.py:1", str(e.exception.code))
        self.assertFalse(self.here(MARKER).exists(), "nothing written")

    def test_a_working_folder_is_a_snapshot(self):
        folder = self.tmp / "LZA-folder"   # the same files, no git
        shutil.copytree(self.project, folder, ignore=shutil.ignore_patterns(".git"))
        self.project = folder
        self.put(self.project, "docs/notes.md", "the porter's notes\n")
        self.assertEqual(self.run_tool(), 0)
        self.assertFalse(self.here("docs/notes.md").exists(), "a working folder's docs/ stays behind")
        row = next(r for r in bi.read_sources() if r[0] == "LZA")
        self.assertTrue(row[3].startswith("snapshot-"), row)
        self.assertIn("working folder", row[2])
        self.assertNotIn(str(self.tmp), "\t".join(row), "no local path in SOURCES.tsv")


if __name__ == "__main__":
    unittest.main()
