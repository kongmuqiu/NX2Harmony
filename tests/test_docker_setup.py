"""The Environment page's Docker Desktop and WSL 2 rows. A player's one-click install left Docker Desktop installed but
not running, and the row still told them to install it; Docker Desktop then stopped at "WSL not installed". The rows now
say what is wrong (WSL missing, a restart due, the features or the firmware's virtualization off, the service agreement
not accepted), the WSL fix installs WSL 2 with an administrator prompt, and the Docker fix installs, then starts Docker
Desktop and waits for it."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nx2h import core, doctor  # noqa: E402

REAL_WANTS_RESTART = doctor._docker_install_wants_restart
ON = {"hypervisor": True, "firmware": False, "vmp": 1, "wsl_oc": 1}   # a PC where WSL 2 runs (a hypervisor is up)
EXE = Path(r"X:\Docker\Docker Desktop.exe")


class Base(unittest.TestCase):
    def setUp(self):
        self.said = []
        p = [mock.patch.object(doctor, "_say", lambda t: self.said.append(t)),
             mock.patch.object(doctor, "_restart", ""),
             mock.patch.object(doctor, "_why", ""),
             mock.patch.object(doctor, "_waits", ""),
             mock.patch.object(doctor, "_docker_install_wants_restart", return_value=False),
             mock.patch.object(doctor.time, "sleep", lambda s: None),
             mock.patch.object(doctor.config, "refresh_path", lambda: None),
             mock.patch.object(doctor.config, "local", lambda: dict(self.local)),
             mock.patch.object(doctor.config, "save_local", lambda **kw: self.local.update(kw))]
        self.local = {}
        for x in p:
            x.start()
            self.addCleanup(x.stop)


class Settings(Base):
    def test_agreement_accepted_at_install_counts(self):
        """winget's package passes --accept-license: the installer records it for all users before any first start."""
        with mock.patch.object(doctor, "_json_file", side_effect=lambda p: {"licenseTermsVersion": 2}
                               if p == doctor.DOCKER_INSTALL_SETTINGS else {}):
            self.assertTrue(doctor.docker_terms_accepted())
        with mock.patch.object(doctor, "_json_file", return_value={}):
            self.assertFalse(doctor.docker_terms_accepted())

    def test_engine(self):
        for settings, wsl in (({}, True), ({"EngineTypeWindows": "wsl2"}, True), ({"engineTypeWindows": "hyperv"}, False),
                              ({"WslEngineEnabled": False}, False)):
            with mock.patch.object(doctor, "_json_file", side_effect=lambda p: settings if p == doctor.DOCKER_SETTINGS else {}):
                self.assertEqual(doctor.docker_uses_wsl(), wsl, settings)


class DockerRow(Base):
    def row(self, installed=True, accepted=True, srv="", plat=ON):
        with mock.patch.object(doctor, "docker_installed", return_value=installed), \
                mock.patch.object(doctor, "docker_terms_accepted", return_value=accepted), \
                mock.patch.object(doctor, "wsl_version", return_value=(2, 6, 1)), \
                mock.patch.object(doctor, "docker_uses_wsl", return_value=True), \
                mock.patch.object(doctor, "platform_state", return_value=plat), \
                mock.patch.object(doctor, "reboot_pending", return_value=False):
            return doctor.docker_check(srv)

    def test_firmware_off_is_said_with_nothing_to_click(self):
        c = self.row(plat=dict(ON, hypervisor=False, firmware=False))
        self.assertIsNone(c.fix)
        self.assertIn("BIOS", c.how)
        self.assertEqual(c.verb, "启动")

    def test_installed_but_stopped_is_started_not_installed(self):
        c = self.row()
        self.assertFalse(c.ok)
        self.assertEqual(c.verb, "启动")
        self.assertIs(c.fix, doctor._start_docker)
        self.assertNotIn("winget", c.how)
        self.assertIn("Docker Desktop", c.how)
        self.assertEqual(c.detail, "已安装但没在运行")

    def test_agreement_not_accepted_is_said(self):
        self.assertIn("服务协议", self.row(accepted=False).detail)

    def test_missing_is_installed(self):
        c = self.row(installed=False)
        self.assertEqual((c.ok, c.verb, c.detail), (False, "安装", "未安装"))
        self.assertIn(doctor.winget_cmd("Docker.DockerDesktop"), c.how)

    def test_nothing_to_click_before_the_restart(self):
        for patch in (mock.patch.object(doctor, "_restart", "WSL"),
                      mock.patch.object(doctor, "_docker_install_wants_restart", return_value=True)):
            with patch:
                c = self.row()
            self.assertIsNone(c.fix)
            self.assertIn("重启电脑", c.how)
            self.assertEqual(c.verb, "启动")   # never "自动安装没成功" under an installed Docker Desktop

    def test_running(self):
        c = self.row(srv="29.0.1")
        self.assertTrue(c.ok)
        self.assertIsNone(c.fix)

    def test_the_page_hears_the_verb(self):
        rows = [doctor.Check("docker", "Docker Desktop 在运行", False, "已安装但没在运行", lambda: True, "x", "启动")]
        with mock.patch.object(doctor, "checks", return_value=rows):
            got = core.checks_json(False)
        self.assertEqual((got[0]["verb"], got[0]["fixable"]), ("启动", True))


class WslRow(Base):
    def rows(self, version, uses_wsl=True, plat=ON):
        with mock.patch.object(doctor, "wsl_version", return_value=version), \
                mock.patch.object(doctor, "docker_uses_wsl", return_value=uses_wsl), \
                mock.patch.object(doctor, "platform_state", return_value=plat):
            return doctor.wsl_checks()

    def test_missing(self):
        (c,) = self.rows(None)
        self.assertEqual((c.key, c.ok, c.detail), ("wsl", False, "未安装"))
        self.assertIs(c.fix, doctor._install_wsl)
        self.assertIn("wsl --install --no-distribution", c.how)

    def test_too_old(self):
        (c,) = self.rows((1, 2, 5))
        self.assertFalse(c.ok)
        self.assertIn("wsl --update", c.how)

    def test_new_enough(self):
        (c,) = self.rows((2, 6, 1))
        self.assertTrue(c.ok)

    def test_features_off(self):
        (c,) = self.rows((2, 6, 1), plat=dict(ON, vmp=2))
        self.assertFalse(c.ok)
        self.assertIs(c.fix, doctor._install_wsl)
        self.assertIn("虚拟机平台", c.detail)

    def test_features_unknown_are_not_held_against_it(self):
        (c,) = self.rows((2, 6, 1), plat={})
        self.assertTrue(c.ok)

    def test_reopened_before_the_restart_is_not_offered_again(self):
        """The Store's package may show only after the restart: the stamp in nx2h.local.json still says to restart."""
        self.local["wsl_installed_at"] = 1000.0
        with mock.patch.object(doctor, "_boot_time", return_value=900.0):
            (c,) = self.rows(None)
        self.assertIsNone(c.fix)
        self.assertIn("重启电脑", c.detail)
        with mock.patch.object(doctor, "_boot_time", return_value=1100.0):   # restarted since: really missing
            (c,) = self.rows(None)
        self.assertIs(c.fix, doctor._install_wsl)

    def test_windows_10_names_the_wsl_feature(self):
        with mock.patch.object(doctor.sys, "getwindowsversion", create=True, return_value=mock.Mock(build=19045)):
            (c,) = self.rows((2, 6, 1), plat=dict(ON, wsl_oc=2))
        self.assertIn("适用于 Linux 的 Windows 子系统", c.detail)

    def test_just_installed_says_restart(self):
        for version in ((2, 6, 1), None):   # from the Microsoft Store the package may show only after the restart
            with mock.patch.object(doctor, "_restart", "WSL"):
                (c,) = self.rows(version)
            self.assertFalse(c.ok)
            self.assertIsNone(c.fix)
            self.assertIn("重启电脑", c.detail + c.how)

    def test_docker_installer_turned_features_on(self):
        with mock.patch.object(doctor, "_docker_install_wants_restart", return_value=True):
            (c,) = self.rows((2, 6, 1), plat=dict(ON, vmp=2))
        self.assertIsNone(c.fix)
        self.assertIn("重启电脑", c.detail)

    def test_hyper_v_engine_needs_no_wsl(self):
        self.assertEqual(self.rows(None, uses_wsl=False), [])

    def test_the_version_is_never_asked_of_wsl_exe(self):
        """Without WSL, Windows' wsl.exe waits for a key press and then installs it: the row reads the registry."""
        with mock.patch.object(doctor.subprocess, "Popen", side_effect=AssertionError("ran a program")), \
                mock.patch.object(doctor.proc, "popen", side_effect=AssertionError("ran a program")), \
                mock.patch.object(doctor, "_run", side_effect=lambda cmd, timeout=60: self.assertNotIn(
                    "wsl", " ".join(map(str, cmd)).lower()) or doctor.subprocess.CompletedProcess(cmd, 1, "", "")), \
                mock.patch.object(doctor, "docker_uses_wsl", return_value=True):
            doctor.wsl_checks()


class InstallWsl(Base):
    def install(self, versions, codes, plat=ON, build=26100):
        it = iter(versions)
        last = versions[-1]
        with mock.patch.object(doctor, "wsl_version", side_effect=lambda: next(it, last)), \
                mock.patch.object(doctor, "platform_state", return_value=plat), \
                mock.patch.object(doctor.sys, "getwindowsversion", create=True,
                                  return_value=mock.Mock(build=build)), \
                mock.patch.object(doctor, "_elevated", side_effect=list(codes)) as el:
            ok = doctor._install_wsl()
        return ok, [c[0] for c in el.call_args_list]

    def test_declined_second_prompt_says_so(self):
        ok, calls = self.install([None], [1, -1], build=19045)
        self.assertFalse(ok)
        self.assertEqual(len(calls), 2)
        self.assertIn("管理员", doctor._why)

    def test_no_github_retry_with_the_installer_only_wsl_exe(self):
        """24H2's own wsl.exe is only an installer: it has no --web-download and reaches GitHub by itself."""
        ok, calls = self.install([None], [1])
        self.assertFalse(ok)
        self.assertEqual(len(calls), 1)
        self.assertIn("wsl --install --no-distribution", doctor._why)

    def test_no_restart_when_nothing_waits_for_one(self):
        ok, _ = self.install([None, (2, 6, 1)], [0])
        self.assertTrue(ok)
        self.assertEqual(doctor._restart, "")
        self.assertNotIn("wsl_installed_at", self.local)

    def test_restart_remembered_after_the_app_is_reopened(self):
        ok, _ = self.install([None], [3010])
        self.assertTrue(ok)
        self.assertIn("wsl_installed_at", self.local)
        with mock.patch.object(doctor, "_restart", ""), \
                mock.patch.object(doctor, "_boot_time", return_value=self.local["wsl_installed_at"] - 60):
            self.assertEqual(doctor.restart_due(), "WSL 刚装好")
        with mock.patch.object(doctor, "_restart", ""), \
                mock.patch.object(doctor, "_boot_time", return_value=self.local["wsl_installed_at"] + 60):
            self.assertEqual(doctor.restart_due(), "")   # Windows restarted since

    def test_player_says_no(self):
        ok, calls = self.install([None], [-1])
        self.assertFalse(ok)
        self.assertEqual(len(calls), 1)
        self.assertIn("管理员", doctor._why)
        self.assertEqual(doctor._restart, "")

    def test_installed_asks_for_a_restart(self):
        for rc in (0, 3010):   # 3010: done, a restart is needed
            with mock.patch.object(doctor, "_restart", ""):
                ok, calls = self.install([None], [rc])
                self.assertTrue(ok)
                self.assertEqual(calls, [(doctor.WSL_INBOX, "--install --no-distribution")])
                self.assertEqual(doctor._restart, "WSL")

    def test_store_unreachable_then_github(self):
        ok, calls = self.install([None], [1, 0], build=19045)
        self.assertTrue(ok)
        self.assertEqual([c[1] for c in calls], ["--install --no-distribution", "--install --no-distribution --web-download"])

    def test_old_one_is_updated_without_a_restart(self):
        ok, calls = self.install([(1, 2, 5), (2, 6, 1)], [0])
        self.assertTrue(ok)
        self.assertEqual(calls, [(doctor.WSL_EXE, "--update")])
        self.assertEqual(doctor._restart, "")

    def test_features_still_off_after_a_failed_run_is_a_failure(self):
        """With the package there already, only the exit code says whether the features were turned on."""
        ok, calls = self.install([(2, 6, 1)], [0x800F0922, 0x800F0922], plat=dict(ON, hypervisor=False, vmp=3))
        self.assertFalse(ok)
        self.assertEqual(len(calls), 2)   # the package's wsl.exe knows --web-download
        self.assertIn("0x800F0922", doctor._why)
        self.assertEqual(doctor._restart, "")
        self.assertNotIn("wsl_installed_at", self.local)

    def test_features_off_use_the_package(self):
        ok, calls = self.install([(2, 6, 1)], [3010])
        self.assertTrue(ok)
        self.assertEqual(calls, [(doctor.WSL_EXE, "--install --no-distribution")])
        self.assertEqual(doctor._restart, "WSL")

    def test_still_missing_says_the_command(self):
        ok, _ = self.install([None], [0x80070005, 0x80070005], build=19045)
        self.assertFalse(ok)
        self.assertIn("0x80070005", doctor._why)
        self.assertIn("wsl --install --no-distribution", doctor._why)


@unittest.skipUnless(sys.platform == "win32", "Windows' ShellExecuteEx")
class Elevated(unittest.TestCase):
    """The launcher behind the UAC prompt, with the verb that asks for nothing (a test must not raise a prompt)."""
    CMD = Path(doctor.os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "cmd.exe"

    def test_waits_and_returns_the_exit_code(self):
        self.assertEqual(doctor._elevated(self.CMD, "/c exit 7", "open"), 7)
        self.assertEqual(doctor._elevated(self.CMD, "/c exit -2147024891", "open") & 0xFFFFFFFF, 0x80070005)

    def test_a_program_that_cannot_start(self):
        self.assertEqual(doctor._elevated(Path(r"C:\no such folder\none.exe"), "", "open"), -1)


class PlatformParse(unittest.TestCase):
    def read(self, out, rc=0):
        with mock.patch.object(doctor.sys, "platform", "win32"), \
                mock.patch.object(doctor, "_plat", (0.0, {})), \
                mock.patch.object(doctor, "_run", return_value=doctor.subprocess.CompletedProcess([], rc, out, "")):
            return doctor.platform_state(fresh=True)   # the reading made up here is not kept for other tests

    def test_values(self):
        self.assertEqual(self.read("True|False|1|1\n"), ON)
        self.assertEqual(self.read("False|True|2|\n"), {"hypervisor": False, "firmware": True, "vmp": 2, "wsl_oc": None})

    def test_a_broken_wmi_is_unknown_not_off(self):
        for out in ("|||\n", "||1|1\n", ""):
            self.assertEqual(self.read(out), {}, out)
        self.assertEqual(self.read("True|False|1|1", rc=1), {})


class InstallLog(unittest.TestCase):
    def test_only_the_last_run_counts(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "install-log-admin.txt"
            old = "-------->8\nEnabled Windows features that require computer restart: VirtualMachinePlatform\n"
            log.write_text(old + "-------->8\nAll features are already installed.\n", encoding="utf-8")
            with mock.patch.object(doctor, "DOCKER_INSTALL_LOGS", (log,)), \
                    mock.patch.object(doctor, "_boot_time", return_value=0.0):
                self.assertFalse(REAL_WANTS_RESTART())
                log.write_text(old, encoding="utf-8")
                self.assertTrue(REAL_WANTS_RESTART())


@unittest.skipUnless(sys.platform == "win32", "Windows' CIM")
class Platform(unittest.TestCase):
    def test_reads_without_administrator_rights(self):
        p = doctor.platform_state(fresh=True)
        self.assertEqual(set(p), {"hypervisor", "firmware", "vmp", "wsl_oc"})
        self.assertIn(p["vmp"], (1, 2, 3, 4, None))


class StartDocker(Base):
    def start(self, installed=True, wsl=(2, 6, 1), servers=("",), accepted=True, winget_ok=True, exe=EXE, due=""):
        answers = iter(servers)
        self.popen = mock.MagicMock()
        self.winget = mock.MagicMock(return_value=winget_ok)
        with mock.patch.object(doctor, "docker_installed", return_value=installed), \
                mock.patch.object(doctor, "wsl_version", return_value=wsl), \
                mock.patch.object(doctor, "docker_uses_wsl", return_value=True), \
                mock.patch.object(doctor, "docker_server", side_effect=lambda: next(answers, servers[-1])), \
                mock.patch.object(doctor, "docker_terms_accepted", return_value=accepted), \
                mock.patch.object(doctor, "docker_desktop_exe", return_value=exe), \
                mock.patch.object(doctor, "restart_due", return_value=due), \
                mock.patch.object(doctor, "platform_state", return_value=ON), \
                mock.patch.object(doctor, "reboot_pending", return_value=False), \
                mock.patch.object(doctor, "server_service_off", return_value=False), \
                mock.patch.object(doctor, "_running", return_value=True), \
                mock.patch.object(doctor, "_winget", return_value=self.winget), \
                mock.patch.object(doctor.subprocess, "Popen", self.popen):
            return doctor._start_docker()

    def test_installed_then_started_in_one_click(self):
        self.assertTrue(self.start(installed=False, servers=("", "", "29.0.1")))
        self.winget.assert_called_once()
        self.popen.assert_called_once_with([str(EXE)])

    def test_failed_install_does_not_start(self):
        self.assertFalse(self.start(installed=False, winget_ok=False))
        self.popen.assert_not_called()

    def test_waits_for_a_restart(self):
        self.assertFalse(self.start(due="WSL 刚装好"))
        self.popen.assert_not_called()
        self.assertIn("重启电脑", doctor._why)
        self.assertEqual(doctor._waits, doctor._why)   # waiting, not failed

    def test_installed_then_waits_for_a_restart(self):
        self.assertFalse(self.start(installed=False, due="WSL 刚装好"))
        self.winget.assert_called_once()
        self.popen.assert_not_called()
        self.assertTrue(doctor._waits)

    def test_virtualization_off_is_said_before_starting(self):
        with mock.patch.object(doctor, "platform_state", return_value=dict(ON, hypervisor=False, firmware=False)):
            answers = iter([""])
            with mock.patch.object(doctor, "docker_installed", return_value=True), \
                    mock.patch.object(doctor, "wsl_version", return_value=(2, 6, 1)), \
                    mock.patch.object(doctor, "docker_uses_wsl", return_value=True), \
                    mock.patch.object(doctor, "docker_server", side_effect=lambda: next(answers, "")), \
                    mock.patch.object(doctor, "restart_due", return_value=""), \
                    mock.patch.object(doctor.subprocess, "Popen") as popen:
                self.assertFalse(doctor._start_docker())
        popen.assert_not_called()
        self.assertIn("BIOS", doctor._why)
        self.assertEqual(doctor._waits, "")

    def test_no_wsl_does_not_start(self):
        self.assertFalse(self.start(wsl=None))
        self.popen.assert_not_called()
        self.assertIn("WSL", doctor._why)

    def test_per_user_install_not_found(self):
        self.assertFalse(self.start(exe=None))
        self.popen.assert_not_called()

    def test_started_and_waited_for(self):
        self.assertTrue(self.start(servers=("", "", "", "29.0.1")))
        self.popen.assert_called_once()
        self.assertTrue(any("Accept" in t for t in self.said))

    def test_never_ready_says_why(self):
        self.assertFalse(self.start(accepted=False))
        self.assertIn("服务协议还没接受", doctor._why)
        self.assertFalse(self.start())
        self.assertIn("Engine running", doctor._why)


class Outcome(Base):
    def test_a_fix_that_waits_is_not_a_failure(self):
        notes = []
        rows = [doctor.Check("docker", "Docker Desktop 在运行", False, "", lambda: doctor._wait("WSL 刚装好，要重启电脑"))]
        import contextlib
        import io
        with mock.patch.object(doctor, "checks", return_value=[]), \
                contextlib.redirect_stdout(io.StringIO()):   # its console lines are Chinese; CI's console is cp1252
            doctor.fix(rows, report=notes.append)
        self.assertEqual([n["kind"] for n in notes], ["item", "wait", "recheck"])
        self.assertIn("重启电脑", notes[1]["why"])

    def test_found_beside_its_docker_command(self):
        """Installed with --installation-dir: Docker Desktop.exe sits three folders above resources\\bin\\docker.exe."""
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            root = Path(d).resolve()   # the long form: %TEMP% may be a short name (RUNNER~1), the lookup resolves
            (root / "resources" / "bin").mkdir(parents=True)
            (root / "resources" / "bin" / "docker.exe").write_bytes(b"")
            (root / "Docker Desktop.exe").write_bytes(b"")
            with mock.patch.object(doctor, "DOCKER_DESKTOP", root / "none.exe"), \
                    mock.patch.object(doctor, "_uninstall_places", return_value=[]), \
                    mock.patch.dict(doctor.os.environ, {"LOCALAPPDATA": str(root / "nothing")}), \
                    mock.patch.object(doctor.shutil, "which", return_value=str(root / "resources" / "bin" / "docker.exe")):
                self.assertEqual(doctor.docker_desktop_exe(), root / "Docker Desktop.exe")


class WaitWhy(Base):
    def why(self, **kw):
        state = dict(wsl=(2, 6, 1), due="", plat=ON, reboot=False, server_off=False, accepted=True, running=True)
        state.update(kw)
        p = mock.MagicMock()
        p.poll.return_value = None if state["running"] else 1
        with mock.patch.object(doctor, "wsl_version", return_value=state["wsl"]), \
                mock.patch.object(doctor, "docker_uses_wsl", return_value=True), \
                mock.patch.object(doctor, "restart_due", return_value=state["due"]), \
                mock.patch.object(doctor, "platform_state", return_value=state["plat"]), \
                mock.patch.object(doctor, "reboot_pending", return_value=state["reboot"]), \
                mock.patch.object(doctor, "server_service_off", return_value=state["server_off"]), \
                mock.patch.object(doctor, "docker_terms_accepted", return_value=state["accepted"]), \
                mock.patch.object(doctor, "_running", return_value=state["running"]):
            return doctor._docker_wait_why(p)

    def test_each_cause(self):
        self.assertIn("WSL", self.why(wsl=None))
        self.assertIn("重启", self.why(due="Docker Desktop 安装时打开了 Windows 的虚拟化组件"))
        self.assertIn("虚拟机平台", self.why(plat=dict(ON, vmp=2)))
        self.assertIn("BIOS", self.why(plat=dict(ON, hypervisor=False, firmware=False)))
        self.assertIn("Windows 在等重启", self.why(plat=dict(ON, hypervisor=False, firmware=True), reboot=True))
        self.assertIn("hypervisorlaunchtype", self.why(plat=dict(ON, hypervisor=False, firmware=True)))
        self.assertIn("LanmanServer", self.why(server_off=True))
        self.assertIn("Accept", self.why(accepted=False))
        self.assertIn("退出", self.why(running=False))
        self.assertIn("Engine running", self.why())
        self.assertIn("Engine running", self.why(plat={}))   # nothing known about the platform


if __name__ == "__main__":
    unittest.main()
