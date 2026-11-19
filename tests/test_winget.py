"""The one-click installs and the install commands shown to the player use the winget source only: the Microsoft Store
source pins its server certificate, and anything replacing HTTPS certificates on a player's PC (an accelerator, an
antivirus's HTTPS scanning, a proxy) made every install fail with 0x8A15005E."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nx2h import doctor  # noqa: E402


class WingetSource(unittest.TestCase):
    def test_the_command_a_player_types(self):
        self.assertEqual(doctor.winget_cmd("Python.Python.3.12"),
                         "winget install -e --id Python.Python.3.12 --source winget")

    def test_the_app_installs_from_the_winget_source_only(self):
        ran = []
        with mock.patch.object(doctor, "_tool", side_effect=lambda cmd: ran.append(cmd) or True), \
                mock.patch.object(doctor, "_say", lambda *a, **k: None):
            self.assertTrue(doctor._winget("Docker.DockerDesktop")())
        cmd = ran[0]
        self.assertEqual(cmd[:2], ["winget", "install"])
        self.assertEqual(cmd[cmd.index("--source") + 1], "winget")
        self.assertEqual(cmd[cmd.index("--id") + 1], "Docker.DockerDesktop")
        for flag in ("-e", "--silent", "--accept-package-agreements", "--accept-source-agreements"):
            self.assertIn(flag, cmd)

    def test_no_install_command_without_the_source(self):
        """Every `winget install` a player is told to run, in the app and the guides, names the winget source."""
        files = [REPO / "一键安装环境.cmd", REPO / "README.md", REPO / "docs" / "install.md", REPO / "docs" / "install.zh-CN.md",
                 REPO / ".claude" / "skills" / "install-nx2harmony" / "SKILL.md"] + sorted((REPO / "nx2h").rglob("*.py"))
        for p in files:
            for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
                if "winget install" in line and "--source winget" not in line and "winget install {" not in line \
                        and "WinGet\\\\Links" not in line:
                    with self.subTest(f"{p.relative_to(REPO).as_posix()}:{n}"):
                        self.fail("a winget install without --source winget")


if __name__ == "__main__":
    unittest.main()
