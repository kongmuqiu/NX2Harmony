"""packaging/full_bundle.py, the full package for a clean PC: which backend files may go in, what stops the package, and
how a PC's bundle name is made neutral. Backends here are made-up folders in a temporary directory; the packing user's
home folder and git e-mail are faked, so nothing depends on this PC."""
from __future__ import annotations

import contextlib
import hashlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
for p in (REPO, REPO / "packaging"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import full_bundle as fb  # noqa: E402
from nx2h import bundle  # noqa: E402

PACKER = "nx2htestpacker"              # the faked Windows user name of the packing PC
PACKER_MAIL = "packer@example.com"      # the faked git e-mail of the packing PC
ICON = b"\x89PNG\r\n\x1a\n" + b"\0" * 24 + b"neutral icon"


def lan(port: str = "5555") -> str:
    """The documented example LAN address with a port, put together here so that this file itself names none."""
    return ".".join(("10", "1", "2", "3")) + ":" + port


def user_folder(name: str) -> str:
    return "\\".join(("C:", "Users", name, "AppData", "x.log"))


class Allowed(unittest.TestCase):
    def test_refused(self):
        for rel in ("build-profile.json5", "nx2h.p12", "debug.p7b", "debug.cer", "debug.csr", "prod.keys",
                    "native-harmonyos/local.ohos.env", "native-harmonyos/local.ohos.env.bak-1",
                    "native-harmonyos/local.ohos.ps1", "native-harmonyos/local.ohos.ps1.usb", "local.properties",
                    "screenshot.png", "docs/shot.JPG", "native-harmonyos/art.webp",
                    "native-harmonyos/vk_draw.c.a121pre", "tools/gmemfast.py.a108", "native-harmonyos/nvn_vk.c.a121b.post",
                    "native-harmonyos/x.py.bak", "native-harmonyos/x.py.bak2", "native-harmonyos/x.c.orig",
                    "prepared/totk-v143/x.bin", "artifacts/native-opus/libopus.a", "entry/build/default/x.so",
                    "native-harmonyos/__pycache__/x.cpython-312.pyc", "entry/libs/arm64-v8a/libgame.so",
                    "game.nsp", "base.xci", "oot.nup", "out.hap", "build.log"):
            with self.subTest(rel):
                self.assertFalse(fb._allowed(PurePosixPath(rel)), rel)

    def test_allowed(self):
        for rel in ("build-profile.template.json5", "entry/build-profile.json5", "native-harmonyos/local.ohos.example.ps1",
                    "native-harmonyos/prepare_v143.py", "tools/docker/d2r.Dockerfile", "LICENSE",
                    "entry/src/main/resources/base/media/icon.png", "AppScope/resources/base/media/app_icon.PNG",
                    "native-harmonyos/build_vpx_ohos.sh"):
            with self.subTest(rel):
                self.assertTrue(fb._allowed(PurePosixPath(rel)), rel)
        self.assertTrue(fb._allowed(PurePosixPath("build-profile.json5"), root_profile=False))


class Vet(unittest.TestCase):
    """_vet / _files on a backend that is a working folder (no .git)."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.root = self.tmp / "BK"
        self.put("native-harmonyos/env.py", 'BUNDLE = "nx2h.uk3f9a2.totk"\nSERVER = "127.0.0.1:9000"\nVER = "10.0.0.0"\n')
        self.put("native-harmonyos/upstream.py", 'URL = "https://example.com/totk-android.git"\n')
        self.put("build-profile.template.json5", "{}\n")
        self.put("LICENSE", "MIT\n")
        self.put("entry/src/main/resources/base/media/icon.png", ICON)
        # the porter's own files of a working folder: never packed
        self.put("docs/notes.md", "phone at " + lan() + ", files in " + user_folder(PACKER) + "\n")
        self.put("README.md", "porter's notes\n")
        self.put("build-profile.json5", '{"signingConfigs": []}\n')
        self.put("entry/screenshot.png", ICON + b"a game's picture")
        self.put("native-harmonyos/local.ohos.env", "TARGET=" + lan() + "\n")
        listed = self.tmp / "pictures.tsv"
        listed.write_text("# sha256\twhere\twhat\n" + hashlib.sha256(ICON).hexdigest() + "\ttest\tneutral\n",
                          encoding="utf-8")
        real_run = subprocess.run

        def run(cmd, *args, **kwargs):
            if list(cmd[:3]) == ["git", "config", "--global"]:
                return SimpleNamespace(stdout=PACKER_MAIL + "\n", stderr="", returncode=0)
            return real_run(cmd, *args, **kwargs)

        for target, name, value in ((fb, "PICTURES_OK", listed), (fb.subprocess, "run", run),
                                    (fb.Path, "home", lambda: Path("C:/Users") / PACKER)):
            p = mock.patch.object(target, name, value)
            p.start()
            self.addCleanup(p.stop)

    def put(self, rel: str, data):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            data = data.encode("utf-8")
        p.write_bytes(data)
        return p

    def vet(self):
        with contextlib.redirect_stdout(io.StringIO()):   # the list of this PC's own folders it may print
            return fb._vet([("bk", "BK", self.root)])

    def refused(self, rel: str, text) -> str:
        self.put(rel, text)
        with self.assertRaises(SystemExit) as e:
            self.vet()
        return str(e.exception.code)

    def test_a_clean_backend_passes(self):
        vetted = self.vet()
        self.assertEqual(len(vetted), 1)
        name, folder, src, files = vetted[0]
        self.assertEqual((name, folder, src), ("bk", "BK", self.root))
        self.assertEqual(sorted(f.as_posix() for f in files),
                         ["LICENSE", "build-profile.template.json5", "entry/src/main/resources/base/media/icon.png",
                          "native-harmonyos/env.py", "native-harmonyos/upstream.py"])

    def test_docs_and_the_porters_files_of_a_working_folder_stay_behind(self):
        files = {f.as_posix() for f in fb._files(self.root)}
        for rel in ("docs/notes.md", "README.md", "build-profile.json5", "entry/screenshot.png",
                    "native-harmonyos/local.ohos.env"):
            self.assertNotIn(rel, files)

    def test_a_lan_address_with_a_port_is_refused(self):
        why = self.refused("native-harmonyos/leak.py", 'TARGET = "' + lan() + '"\n')
        self.assertIn("BK/native-harmonyos/leak.py:1", why)
        self.assertIn("LAN address", why)

    def test_the_packing_users_folder_is_refused(self):
        why = self.refused("tools/run.ps1", "# made on this PC\n$log = '" + user_folder(PACKER) + "'\n")
        self.assertIn("BK/tools/run.ps1:2", why)
        self.assertIn("the packing user's folder", why)

    def test_another_users_folder_is_only_a_path(self):
        self.put("tools/run.ps1", "$log = '" + user_folder(PACKER + "2") + "'\n")
        self.assertEqual(len(self.vet()), 1)

    def test_the_packing_users_git_mail_is_refused(self):
        why = self.refused("native-harmonyos/AUTHORS.txt", "Packer <" + PACKER_MAIL.upper() + ">\n")
        self.assertIn("git e-mail", why)

    def test_a_cloud_drive_download_folder_is_refused(self):
        # the folder name is put together here, so that this file names none
        why = self.refused("native-harmonyos/paths.py", "CACHE = r'E:\\" + "Baidu" + "Net" + "disk" + "Download\\x'\n")
        self.assertIn("BK/native-harmonyos/paths.py:1", why)
        self.assertIn("download folder", why)

    def test_an_unlisted_picture_under_resources_is_refused(self):
        why = self.refused("entry/src/main/resources/base/media/game_icon.png", ICON + b"official art")
        self.assertIn("entry/src/main/resources/base/media/game_icon.png", why)
        self.assertIn("pictures.tsv", why)

    def test_a_pc_bundle_name_is_not_a_reason_to_refuse(self):
        self.put("native-harmonyos/env2.py", 'B = "nx2h.ua1b2c3.oot"\n')
        self.assertEqual(len(self.vet()), 1)


class Copy(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def copy(self, data: bytes):
        src, dst = self.tmp / "src.bin", self.tmp / "out" / "sub" / "dst.bin"
        src.write_bytes(data)
        changed = fb._copy(src, dst)
        self.assertEqual(src.read_bytes(), data, "the backend's own file stays as it is")
        return changed, dst.read_bytes()

    def test_text_gets_the_neutral_bundle_name(self):
        changed, out = self.copy(b'A = "nx2h.uk3f9a2.totk"\nB = "nx2h.ua1b2c3.oot_x"\nC = "com.example.app"\n')
        self.assertTrue(changed)
        self.assertEqual(out, b'A = "nx2h.u000000.totk"\nB = "nx2h.u000000.oot_x"\nC = "com.example.app"\n')
        self.assertTrue(bundle.valid((fb.NEUTRAL_BUNDLE + b"totk").decode()))

    def test_text_without_one_is_copied_as_it_is(self):
        data = b"nx2h.u12345.totk is not a PC part (5 characters)\r\n"
        self.assertEqual(self.copy(data), (False, data))

    def test_binaries_are_left_alone(self):
        data = b"\x7fELF\0\0\0" + b"nx2h.uk3f9a2.totk" + b"\0" * 16
        self.assertEqual(self.copy(data), (False, data))


class Helpers(unittest.TestCase):
    def test_safe_parts(self):
        self.assertEqual(fb._safe_parts("a/b/c.txt"), ("a", "b", "c.txt"))
        self.assertEqual(fb._safe_parts("./a//b"), ("a", "b"))
        for bad in ("../x", "a/../../x", "/etc/passwd", "C:x", "a\\..\\x", ""):
            with self.subTest(bad):
                with self.assertRaises(SystemExit):
                    fb._safe_parts(bad)

    def test_lf_sh(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / "a").mkdir()
            (root / "a" / "run.sh").write_bytes(b"#!/bin/sh\r\necho hi\r\n")
            (root / "ok.sh").write_bytes(b"#!/bin/sh\necho hi\n")
            (root / "note.txt").write_bytes(b"x\r\n")
            self.assertEqual(fb._lf_sh(root), 1)
            self.assertEqual((root / "a" / "run.sh").read_bytes(), b"#!/bin/sh\necho hi\n")
            self.assertEqual((root / "note.txt").read_bytes(), b"x\r\n")

    def test_upstream_pin_must_agree(self):
        pin, other = "a" * 40, "b" * 40
        with tempfile.TemporaryDirectory() as t:
            roots = []
            for i, value in enumerate((pin, pin.upper())):
                na = Path(t) / f"B{i}" / "native-harmonyos"
                na.mkdir(parents=True)
                (na / "UPSTREAM_PIN").write_text(value + "\n", encoding="utf-8")
                (na / "upstream.py").write_text('URL = "https://example.com/totk-android.git"\n', encoding="utf-8")
                roots.append(na.parent)
            self.assertEqual(fb._upstream_pin(roots), (pin, "https://example.com/totk-android.git"))
            (roots[1] / "native-harmonyos" / "UPSTREAM_PIN").write_text(other, encoding="utf-8")
            with self.assertRaises(SystemExit):
                fb._upstream_pin(roots)


if __name__ == "__main__":
    unittest.main()
