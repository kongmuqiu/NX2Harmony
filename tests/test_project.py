"""Checks that need no game files, keys, phone or network:  python -I -m unittest discover -s tests"""
from __future__ import annotations

import ast
import contextlib
import hashlib
import io
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import warnings
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "packaging"))

from nx2h import __version__, bundle, config  # noqa: E402
from nx2h.backends import BACKENDS, MARKERS  # noqa: E402

PY_FILES = [REPO / "nx2h.py"] + sorted((REPO / "nx2h").rglob("*.py")) + sorted((REPO / "packaging").glob("*.py")) \
    + sorted((REPO / "tests").glob("*.py")) + [REPO / ".githooks" / "pre-commit"]
DOCS = ["README.md", "AGENTS.md", "CONTRIBUTING.md", "CHANGELOG.md", "docs/install.md", "docs/install.zh-CN.md",
        "docs/how-it-works.md", "docs/how-it-works.zh-CN.md", ".claude/skills/install-nx2harmony/SKILL.md"]
# the pages that tell a player which nx2h.py options to use
OPTION_DOCS = ["README.md", "docs/install.md", "docs/install.zh-CN.md", ".claude/skills/install-nx2harmony/SKILL.md"]


def lan(port: str = "5555") -> str:
    """The documented example address with a port, put together here so that this file names none."""
    return ".".join(("10", "1", "2", "3")) + ":" + port


def cloud_drive_folder() -> str:
    """A download folder of a cloud-drive client, the kind full_bundle.py refuses; put together at run time."""
    return "E:/" + "Baidu" + "Net" + "disk" + "Download/x"


class Source(unittest.TestCase):
    def test_python_39_syntax(self):
        for p in PY_FILES:
            with self.subTest(p.relative_to(REPO).as_posix()):
                ast.parse(p.read_text(encoding="utf-8"), str(p), feature_version=(3, 9))

    def test_no_invalid_escape_sequences(self):
        """A backslash path in a docstring that is not raw (bin\\java.exe) warns now and is meant to become an error."""
        for p in PY_FILES:
            with self.subTest(p.relative_to(REPO).as_posix()):
                with warnings.catch_warnings():
                    warnings.simplefilter("error")
                    compile(p.read_text(encoding="utf-8"), str(p), "exec")

    def test_version_is_the_changelog_top(self):
        top = re.search(r"^## (\d+\.\d+\.\d+)", (REPO / "CHANGELOG.md").read_text(encoding="utf-8"), re.M)
        self.assertIsNotNone(top)
        self.assertEqual(top.group(1), __version__)


class GamesTable(unittest.TestCase):
    def test_rows(self):
        lines = (REPO / "games.tsv").read_text(encoding="utf-8").splitlines()
        header = next(l[2:].split("\t") for l in lines if l.startswith("# title_id"))
        rows = [l.split("\t") for l in lines if l.strip() and not l.startswith("#")]
        self.assertTrue(rows)
        slugs = set()
        for r in rows:
            with self.subTest(r[0]):
                self.assertEqual(len(r), len(header), "a row config.games() would drop silently")
                d = dict(zip(header, r))
                self.assertRegex(d["title_id"], r"^[0-9A-F]{16}$")
                self.assertIn(d["status"], ("已验证", "优化中", "适配中"))
                self.assertTrue(d["backend"] == "-" or d["backend"] in BACKENDS, d["backend"])
                self.assertNotIn(d["slug"], slugs)
                slugs.add(d["slug"])
                self.assertTrue(bundle.valid(f"nx2h.uk3f9a2.{bundle.slug_of(d['slug'], d['title_id'])}"))

    def test_every_backend_has_a_marker_and_a_default_folder(self):
        self.assertEqual(set(MARKERS), set(BACKENDS))
        self.assertEqual(set(config.DEFAULT_BACKENDS), set(BACKENDS))


class Bundles(unittest.TestCase):
    def test_this_pcs_part_has_the_documented_shape(self):
        if not bundle._machine_guid():
            self.skipTest("no Windows MachineGuid here (the random fallback would write nx2h.local.json)")
        self.assertRegex(bundle.pc_part(), r"^u[0-9a-z]{6}$")

    def test_the_release_placeholder_is_a_valid_name(self):
        import full_bundle as fb
        self.assertTrue(bundle.valid(fb.NEUTRAL_BUNDLE.decode() + "oot"))


PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 32   # bytes are enough: the gate looks at names and hashes, not pixels


class ReleaseGate(unittest.TestCase):
    """packaging/full_bundle.py: what a release would carry from a backend that is a working folder (not git)."""

    def setUp(self):
        import full_bundle as fb
        self.fb = fb
        self.tmp = Path(tempfile.mkdtemp())
        self.root = self.tmp / "BK"
        (self.root / "native-harmonyos").mkdir(parents=True)
        (self.root / "docs").mkdir()
        (self.root / "docs" / "notes.md").write_text("phone at " + lan(), encoding="utf-8")
        self.code = self.root / "native-harmonyos" / "env.py"
        self.code.write_text('BUNDLE = "nx2h.uk3f9a2.oot"\nSERVER = "127.0.0.1:48710"\nVER = "10.0.0.0"\n',
                             encoding="utf-8")
        media = self.root / "entry" / "src" / "main" / "resources" / "base" / "media"
        media.mkdir(parents=True)
        (media / "icon.png").write_bytes(PNG)
        listed = self.tmp / "pictures.tsv"
        listed.write_text("# sha256\twhere\twhat\n" + hashlib.sha256(PNG).hexdigest() + "\ttest\tblank\n",
                          encoding="utf-8")
        self.saved = fb.PICTURES_OK
        fb.PICTURES_OK = listed

    def tearDown(self):
        self.fb.PICTURES_OK = self.saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def vet(self):
        with contextlib.redirect_stdout(io.StringIO()):   # the list of this PC's own folders it may print
            return self.fb._vet([("bk", "BK", self.root)])

    def refused(self, text: str) -> str:
        bad = self.root / "native-harmonyos" / "leak.py"
        bad.write_text(text, encoding="utf-8")
        try:
            with self.assertRaises(SystemExit) as e:
                self.vet()
            return str(e.exception.code)
        finally:
            bad.unlink()

    def test_docs_of_a_working_folder_stay_behind(self):
        files = {r.as_posix() for r in self.fb._files(self.root)}
        self.assertIn("native-harmonyos/env.py", files)
        self.assertNotIn("docs/notes.md", files)

    def test_a_clean_backend_passes_and_its_bundle_name_is_made_neutral(self):
        self.assertEqual(len(self.vet()), 1)
        out = self.tmp / "out" / "env.py"
        self.assertTrue(self.fb._copy(self.code, out))
        text = out.read_text(encoding="utf-8")
        self.assertIn("nx2h.u000000.oot", text)
        self.assertNotIn("uk3f9a2", text)
        self.assertIn("uk3f9a2", self.code.read_text(encoding="utf-8"), "the source must stay as it is")

    def test_the_packing_users_folder_is_refused(self):
        self.assertIn("leak.py:1", self.refused(f"log = r'C:\\Users\\{Path.home().name}\\x.log'\n"))

    def test_a_lan_address_with_a_port_is_refused(self):
        self.assertIn("leak.py:1", self.refused('TARGET = "' + lan() + '"\n'))

    def test_a_cloud_drive_download_folder_is_refused(self):
        self.assertIn("leak.py:1", self.refused("# cache in " + cloud_drive_folder() + "\n"))

    def test_a_picture_nobody_has_looked_at_is_refused(self):
        art = self.root / "entry" / "src" / "main" / "resources" / "base" / "media" / "art.png"
        art.write_bytes(PNG + b"art")
        with self.assertRaises(SystemExit) as e:
            self.vet()
        self.assertIn("art.png", str(e.exception.code))

    def test_the_listed_pictures_are_well_formed(self):
        for line in self.saved.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                self.assertRegex(line.split("\t")[0], r"^[0-9a-f]{64}$")


class Repository(unittest.TestCase):
    def test_paths_named_in_the_docs_exist(self):
        link = re.compile(r"\]\(([^)#:]+)(?:#[^)]*)?\)")
        code = re.compile(r"`((?:nx2h|packaging|docs|tests|\.githooks|\.claude)/[^`\s*<>]+|[\w.-]+\.(?:md|tsv|txt|cmd|py))`")
        # files of other projects the docs name (nxnative's, a backend's, the full package's marker in totk-android)
        elsewhere = {"build_ipa_from_nsp.py", "scripts/pack_nup.py", "upstream.py", "docs/frontends.md", "build_ohos.py",
                     "manifest.tsv", "NX2HARMONY-UPSTREAM.txt"}
        own = {p.name for d in ("nx2h", "packaging", "docs", "tests") for p in (REPO / d).rglob("*") if p.is_file()}
        for doc in DOCS:
            text = (REPO / doc).read_text(encoding="utf-8")
            for m in list(link.finditer(text)) + list(code.finditer(text)):
                ref = m.group(1).rstrip("/.,")
                if ref.startswith("http") or ref in elsewhere:
                    continue
                base = (REPO / doc).parent if m.re is link else REPO
                with self.subTest(doc=doc, ref=ref):
                    found = (base / ref).exists() or (REPO / ref).exists() or ("/" not in ref and ref in own)
                    self.assertTrue(found, ref)

    def test_the_guides_use_options_the_command_line_has(self):
        """Every --option the guides give nx2h.py is one cli.py defines. Taken as nx2h.py's: the options in a code span
        or a code line that runs nx2h.py, in a code span that is only options (`--game <title id>`), and in the comment
        lines of a code block that runs nx2h.py. Another program's options (winget --id, git --short) are left out."""
        fence = re.compile(r"^ {0,3}(```|~~~)[^\n]*\n(.*?)^ {0,3}\1[^\n]*$", re.M | re.S)
        span = re.compile(r"(`+)((?:(?!\1).)+?)\1", re.S)
        option = re.compile(r"(?<![\w-])(--[a-z][a-z0-9-]*)")
        cli = (REPO / "nx2h" / "cli.py").read_text(encoding="utf-8")
        for doc in OPTION_DOCS:
            text = (REPO / doc).read_text(encoding="utf-8")
            used = set()
            for m in fence.finditer(text):
                lines = m.group(2).splitlines()
                if any("nx2h.py" in ln for ln in lines):
                    for ln in lines:
                        if "nx2h.py" in ln or ln.lstrip().startswith("#"):
                            used.update(option.findall(ln))
            for m in span.finditer(fence.sub("", text)):
                code = m.group(2).strip()
                if "nx2h.py" in code or code.startswith("--"):
                    used.update(option.findall(code))
            for opt in sorted(used):
                with self.subTest(doc=doc, option=opt):
                    self.assertIn(f'"{opt}"', cli)

    def test_notices_and_package_carry_the_vendored_licences(self):
        notices = (REPO / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        spec = (REPO / "packaging" / "NX2Harmony.spec").read_text(encoding="utf-8")
        for lic in ("nx2h/vendor/switchfs/LICENSE", "nx2h/vendor/nxnative-LICENSE"):
            with self.subTest(lic):
                self.assertTrue((REPO / lic).is_file())
                self.assertIn(lic, notices)
                self.assertIn(lic.rsplit("/", 1)[-1], spec)

    def test_private_files_are_ignored(self):
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        for p in ("prod.keys", "title.keys", "game.nsp", "game.nsz", "game.xci", "game.nup", "x.hap", "nx2h.local.json",
                  "nx2h.local.json.123.tmp", "build-profile.json5", "nx2h.p12", "debug.p7b", "debug.cer", "work/icons/a.jpg",
                  "logs/build.log", "dist/NX2Harmony/NX2Harmony.exe", "prepared/x", "artifacts/x", "NX2Harmony.zip"):
            with self.subTest(p):
                r = subprocess.run(["git", "-C", str(REPO), "check-ignore", "-q", "--no-index", p])
                self.assertEqual(r.returncode, 0, f"{p} is not ignored")


class Licences(unittest.TestCase):
    """What packaging/build.py writes into dist/NX2Harmony/licenses: its committed texts are there, the notices name
    them, and the spec packs NX2Harmony's own LICENSE."""
    # gnu.org's lgpl-3.0.txt and gpl-3.0.txt (with LF line ends)
    GNU = {"LGPL-3.0.txt": "e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118",
           "GPL-3.0.txt": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}

    @staticmethod
    def build_constant(name: str):
        tree = ast.parse((REPO / "packaging" / "build.py").read_text(encoding="utf-8"))
        for node in tree.body:
            if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == name for t in node.targets):
                return ast.literal_eval(node.value)
        raise AssertionError(f"packaging/build.py has no {name}")

    def test_the_committed_texts_build_py_promises_are_there(self):
        committed = self.build_constant("COMMITTED")
        self.assertTrue(set(self.GNU) <= set(committed))
        self.assertTrue({"QT-THIRD-PARTY.txt", "QT-SOURCES.txt"} <= set(committed))
        notices = (REPO / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for name in committed:
            p = REPO / "packaging" / "licenses" / name
            with self.subTest(name):
                self.assertTrue(p.is_file(), "missing")
                self.assertGreater(p.stat().st_size, 0, "empty")
                self.assertIn(f"licenses/{name}", notices, "THIRD_PARTY_NOTICES.md does not name it")

    def test_the_gnu_texts_are_the_canonical_ones(self):
        for name, sha in self.GNU.items():
            with self.subTest(name):
                data = (REPO / "packaging" / "licenses" / name).read_bytes().replace(b"\r\n", b"\n")
                self.assertEqual(hashlib.sha256(data).hexdigest(), sha)

    def test_the_texts_name_the_qt_and_cryptography_versions_build_py_checks(self):
        lic = REPO / "packaging" / "licenses"
        qt, crypto = self.build_constant("QT_VERSION"), self.build_constant("CRYPTOGRAPHY_VERSION")
        for name, version in (("QT-THIRD-PARTY.txt", qt), ("QT-SOURCES.txt", qt), ("CRYPTOGRAPHY-RUST.txt", crypto)):
            with self.subTest(name):
                self.assertIn(version, (lic / name).read_text(encoding="utf-8"))

    def test_the_spec_packs_licence_and_leaves_out_software_opengl(self):
        spec = (REPO / "packaging" / "NX2Harmony.spec").read_text(encoding="utf-8")
        self.assertRegex(spec, r'\(str\(root / "LICENSE"\), "\."\)')
        self.assertIn('!= "opengl32sw.dll"', spec)
        self.assertRegex(spec, r"COLLECT\([^)]*upx=False")


if __name__ == "__main__":
    unittest.main()
