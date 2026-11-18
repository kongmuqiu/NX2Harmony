""".githooks/pre-commit: a commit that stages keys, game files, signing material, packages, this PC's settings, a file
over 20 MB or a text file with key lines in it stops. Each test stages files in a new git repository in a temporary
folder and runs the hook there: with this Python, through sh as git starts it, or through git itself. Key lines are put
together at run time, so this file holds none."""
from __future__ import annotations

import hashlib
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HOOK = REPO / ".githooks" / "pre-commit"
HEX = "0123456789abcdef" * 4


def key_line(name: str, digits: int = 32) -> str:
    return name + " = " + HEX[:digits]


def find_sh():
    """The sh git starts hooks with: on PATH, else the one beside Git for Windows' git.exe; None without one."""
    sh = shutil.which("sh")
    if sh:
        return sh
    git = shutil.which("git")
    if not git:
        return None
    here = Path(git).resolve().parent
    for root in (here.parent, here.parent.parent):   # <Git>\cmd\git.exe, <Git>\mingw64\bin\git.exe
        for c in (root / "bin" / "sh.exe", root / "usr" / "bin" / "sh.exe"):
            if c.is_file():
                return str(c)
    return None


def load_hook() -> dict:
    """The hook's functions and tables, read without leaving a __pycache__ in .githooks."""
    return runpy.run_path(str(HOOK), run_name="pre_commit_hook")


@unittest.skipUnless(shutil.which("git"), "git is not installed")
class TempRepo(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        empty = self.tmp / "gitconfig"   # no global or system settings of this PC
        empty.write_text("", encoding="utf-8")
        path = os.path.dirname(sys.executable) + os.pathsep + os.environ.get("PATH", "")
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=str(empty), GIT_CONFIG_NOSYSTEM="1", PATH=path)
        self.git("init", "-q")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=str(self.repo), env=self.env, capture_output=True, check=True)

    def stage(self, *paths: str, data: bytes = b"x"):
        for path in paths:
            f = self.repo / path
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(data)
        self.git("add", "-f", "--", *paths)

    def hook(self):
        return subprocess.run([sys.executable, str(HOOK)], cwd=str(self.repo), env=self.env, capture_output=True)

    def assert_stopped(self, r, path: str, why: str = ""):
        err = r.stderr.decode("utf-8", "replace")
        self.assertEqual(r.returncode, 1, err)
        self.assertIn(path, err)
        self.assertIn(why, err)


class Names(TempRepo):
    def test_nothing_staged_passes(self):
        self.assertEqual(self.hook().returncode, 0)

    def test_source_files_pass(self):
        self.stage("nx2h/scan.py", "nx2h/backends/totk_v143.py", "nx2h/signing.py", "nx2h/fonts.py",
                   "nx2h/vendor/switchfs/extract_romfs.py", "nx2h/vendor/switchfs/extract_shared_fonts.py",
                   "packaging/build.py", "packaging/NX2Harmony.ico", "games.tsv", "README.md", "docs/install.zh-CN.md",
                   "docs/how-it-works.zh-CN.md", "docs/game-notes.md", ".claude/skills/install-nx2harmony/SKILL.md",
                   ".github/workflows/tests.yml", ".github/ISSUE_TEMPLATE/build-problem.yml", "tests/test_scan.py",
                   "一键构建.cmd", "packaging/bundle_pictures.tsv", "docs/romfs-notes.md",
                   "build-profile.template.json5", "local.ohos.example.ps1",
                   "backends/LZA/entry/build-profile.json5", "backends/OOT/native-harmonyos/local.ohos.example.ps1",
                   "backends/prebuilt-ohos/native-opus/build-ohos/opus/libopus.a",
                   "backends/prebuilt-ohos/native-vpx/ohos/lib/libvpx.a")
        r = self.hook()
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))

    def test_a_bundle_name_made_on_a_pc_is_stopped_in_backends(self):
        self.stage("backends/LZA/AppScope/app.json5", data=b'{"app": {"bundleName": "nx2h.uab12cd.za"}}\n')
        self.assert_stopped(self.hook(), "backends/LZA/AppScope/app.json5", "bundle name")
        self.git("reset", "-q")
        self.stage("backends/LZA/AppScope/app.json5", data=b'{"app": {"bundleName": "nx2h.u000000.za"}}\n')
        r = self.hook()
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))

    def test_an_unlisted_picture_in_backends_is_stopped(self):
        picture = b"\x89PNG\r\n\x1a\n" + b"\0" * 24 + b"the game's icon"
        self.stage("backends/OOT/entry/src/main/resources/base/media/game_icon.png", data=picture)
        self.assert_stopped(self.hook(), "game_icon.png", "bundle_pictures.tsv")
        listed = hashlib.sha256(picture).hexdigest() + "\ttest\tlooked at\n"
        self.stage("packaging/bundle_pictures.tsv", data=listed.encode("utf-8"))
        r = self.hook()
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))

    def test_a_backends_module_file_with_signing_inside_is_stopped(self):
        self.stage("backends/LZA/entry/build-profile.json5",
                   data=b'{"app": {"signingConfigs": [{"material": {"storePassword": "x"}}]}}\n')
        self.assert_stopped(self.hook(), "backends/LZA/entry/build-profile.json5", "signing")

    def test_private_and_game_files_are_stopped(self):
        paths = ("prod.keys", "keys/title.keys", "prod.keys.bak", ".switch/x.txt", "Games/Game.NSP", "update.nsz",
                 "base.xci", "base.xcz", "game.nup", "x/00.nca", "x/00.ncz", "x/0100.tik", "main.nso", "x.nro",
                 "extracted/romfs/Pack/a.bin", "dump/ExeFS/main", "romfs.bin", "out/romfs.img",
                 "game-v143/romfs-config.txt", "game-za/fonts/a.txt", "x/game-oot/runtime-index.bin",
                 "runtime-tables.bin", "shared-fonts/a.txt", "FontStandard.bfttf", "x/FontExt.BFOTF",
                 "seed.vkpack", "save/progress.sav", "out/entry-default-signed.hap", "out/x.hsp", "out/release.app",
                 "NX2Harmony.exe", "x/a.dll", "entry/libs/arm64-v8a/libgame.so", "libopus.a", "NX2Harmony.zip",
                 "x.7z", "nx2h.p12", "x.pfx", "debug.p7b", "debug.cer", "debug.csr", "release.jks", "x.pem",
                 "release.keystore", "build-profile.json5", "signing/material/fd/0/x", "material/x",
                 "account.dat", "keys/key.dat", ".nx2h/login.log", ".ohos/config/default.cer.txt",
                 "nx2h.local.json", "nx2h.local.json.1234.tmp", "native-harmonyos/local.ohos.env",
                 "local.ohos.ps1.wifi", "local.properties", "prepared/totk-v143/x.txt", "artifacts/native-opus/x.txt",
                 "work/icons/A.jpg", "logs/a.log", "dist/NX2Harmony/x.txt", "build/x.txt", "游戏/prod.keys",
                 "main.npdm", "x/title.key", ".env", "x/.env.local", "build.log", "backends/LZA/build-profile.json5",
                 "backends/OOT/native-harmonyos/local.ohos.ps1", "backends/LZA/prepared/za/x.txt",
                 "backends/OOT/artifacts/native-opus/libopus.a", "backends/OOT/entry/libs/arm64-v8a/libtotk_ohos.so",
                 "x/entry/build-profile.json5", "backends/prebuilt-ohos/libgame.so", "backends/patches/LZA.patch",
                 "backends/OOT/native-harmonyos/x.c.rej", "backends/OOT/native-harmonyos/x.c.orig")
        self.stage(*paths)
        r = self.hook()   # one run lists every path it stops
        self.assertEqual(r.returncode, 1)
        listed = {ln.strip().rsplit("    (", 1)[0] for ln in r.stderr.decode("utf-8", "replace").splitlines()}
        for path in paths:
            with self.subTest(path=path):
                self.assertIn(path, listed)

    def test_one_bad_file_among_good_ones_stops_the_commit(self):
        self.stage("nx2h/cli.py")
        self.stage("README.md")
        self.stage("keys/prod.keys")
        r = self.hook()
        self.assert_stopped(r, "keys/prod.keys")
        self.assertNotIn("nx2h/cli.py", r.stderr.decode("utf-8", "replace"))

    def test_removing_a_committed_game_file_is_allowed(self):
        self.stage("old.nsp")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-qm", "x", "--no-verify")
        self.git("rm", "-q", "old.nsp")
        self.assertEqual(self.hook().returncode, 0)


class Contents(TempRepo):
    def test_key_lines_under_any_name_are_stopped(self):
        cases = {
            "keys.txt": key_line("header_key", 64),
            "notes/prod_keys.txt": "# dumped\r\n" + key_line("key_area_key_application_00") + "\r\n",
            "docs/howto.md": "text\n\n    " + key_line("titlekek_0a").upper() + "\n",
            "x.ini": key_line("master_key_10") + "\n" + key_line("package2_key_10"),
            "x.cfg": key_line("eticket_rsa_kek"),
            "x.yml": "aes_kek_generation_source: " + HEX[:32],
            "rights.txt": HEX[:32].upper() + " = " + HEX[32:],
        }
        for path, text in cases.items():
            self.stage(path, data=text.encode("utf-8"))
        r = self.hook()
        self.assertEqual(r.returncode, 1)
        err = r.stderr.decode("utf-8", "replace")
        for path in cases:
            with self.subTest(path=path):
                self.assertIn(f"  {path}    (keys inside the file", err)

    def test_hex_that_is_no_key_passes(self):
        self.stage("a.py", data=('PIN = "' + HEX[:40] + '"\nsha256 = ' + HEX + "\nheader_key = bytes.fromhex('"
                                 + HEX + "')\n").encode("utf-8"))
        self.stage("b.md", data=("commit " + HEX[:40] + "\n" + key_line("header_key", 31) + "\n").encode("utf-8"))
        r = self.hook()
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))

    def test_a_file_over_20_mb_is_stopped(self):
        limit = load_hook()["LIMIT"]
        self.assertEqual(limit, 20 * 1024 * 1024)
        self.stage("data/at-the-limit.bin", data=bytes(limit))
        r = self.hook()
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        self.stage("data/over.bin", data=bytes(limit + 1))
        r = self.hook()
        self.assert_stopped(r, "data/over.bin", "over 20 MB")
        self.assertNotIn("at-the-limit", r.stderr.decode("utf-8", "replace"))


class HowGitStartsIt(TempRepo):
    def test_sh_header_and_lf(self):
        data = HOOK.read_bytes()
        self.assertEqual(data.split(b"\n", 1)[0], b"#!/bin/sh")
        self.assertNotIn(b"\r", data, ".gitattributes keeps .githooks/* LF: sh reads a CR as part of a word")
        self.assertIn(b'for p in python python3; do', data)

    def test_git_runs_it_on_commit(self):
        if os.name != "nt":
            self.assertTrue(os.access(str(HOOK), os.X_OK), "git skips a hook that is not executable")
        commit = ["-c", "core.hooksPath=" + (REPO / ".githooks").as_posix(), "-c", "user.name=t",
                  "-c", "user.email=t@example.com", "commit", "-qm", "x"]
        self.stage("README.md")
        r = subprocess.run(["git", *commit], cwd=str(self.repo), env=self.env, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        self.stage("prod.keys")
        r = subprocess.run(["git", *commit], cwd=str(self.repo), env=self.env, capture_output=True)
        self.assert_stopped(r, "prod.keys")

    def stubs(self, python3_works: bool) -> dict:
        """PATH with a python that does not start (like the Store stub) and a python3 that is this Python, or not."""
        sh = find_sh()
        if not sh:
            self.skipTest("no sh here")
        bin_ = self.tmp / "bin"
        bin_.mkdir()
        exe = Path(sys.executable).as_posix()
        for name, body in (("python", "exit 9009"),
                           ("python3", f'exec "{exe}" "$@"' if python3_works else "exit 9009")):
            f = bin_ / name
            f.write_bytes(f"#!/bin/sh\n{body}\n".encode("utf-8"))
            f.chmod(0o755)
        env = dict(self.env, PATH=str(bin_) + os.pathsep + self.env["PATH"])
        return dict(args=[sh, HOOK.as_posix()], cwd=str(self.repo), env=env, capture_output=True)

    def test_python3_when_python_does_not_start(self):
        run = self.stubs(python3_works=True)
        self.stage("README.md")
        r = subprocess.run(**run)
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        self.stage("prod.keys")
        self.assert_stopped(subprocess.run(**run), "prod.keys")

    def test_neither_python_starts(self):
        r = subprocess.run(**self.stubs(python3_works=False))
        self.assertEqual(r.returncode, 1)
        self.assertIn("neither python nor python3", r.stderr.decode("utf-8", "replace"))


class ThisRepository(unittest.TestCase):
    def test_the_files_git_would_commit_pass(self):
        """Every file of this checkout that git would commit (tracked, or new and not ignored) passes the hook."""
        if not shutil.which("git") or not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, check=True).stdout
        hook = load_hook()
        for rel in sorted({p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p}):
            p = REPO / rel
            if not p.is_file():
                continue
            with self.subTest(rel):
                self.assertIsNone(hook["why"](rel))
                data = p.read_bytes()
                self.assertLessEqual(len(data), hook["LIMIT"])
                self.assertFalse(hook["holds_keys"](data), "a key line")


if __name__ == "__main__":
    unittest.main()
