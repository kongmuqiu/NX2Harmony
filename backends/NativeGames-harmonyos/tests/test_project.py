"""Checks that need no game files, phone or DevEco: run with  python -m unittest discover -s tests"""
import re, subprocess, sys, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native-harmonyos"))


class Project(unittest.TestCase):
    def test_pin_is_a_commit(self):
        pin = (ROOT / "native-harmonyos" / "UPSTREAM_PIN").read_text().split()[0]
        self.assertRegex(pin, r"^[0-9a-f]{40}$")

    def test_environment_resolves_to_this_checkout(self):
        import ohos_env
        self.assertEqual(ohos_env.PORT, ROOT)
        self.assertEqual(ohos_env.REPO, ROOT)
        self.assertTrue(ohos_env.BUNDLE)

    def test_game_material_is_ignored(self):
        for path in ("prepared/x", "artifacts/x", "signing/x", "game.xci", "update.nsz", "prod.keys", "app.hap", "entry/libs/arm64-v8a/a.so",
                     "native-harmonyos/local.ohos.ps1", "build-profile.json5"):
            r = subprocess.run(["git", "check-ignore", "-q", path], cwd=ROOT)
            self.assertEqual(r.returncode, 0, path + " must be ignored")

    def test_preparation_dry_run(self):
        import ohos_env
        if not (ohos_env.UPSTREAM / "games" / "totk").is_dir():
            self.skipTest("totk-android checkout not present")
        out = subprocess.run([sys.executable, "-I", str(ROOT / "native-harmonyos" / "prepare_v143.py"), "--dry-run", "--only", "libs"],
                             capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("build_ohos.py", out.stdout)

    def test_seed_containers_follow_the_machine(self):
        import build_seed
        # (containers, translators each, RomFS readers) from Docker's CPUs and free MiB
        self.assertEqual(build_seed.allocate(32, 14600), (8, 4, 12))   # i9-13900HX, Docker Desktop 16 GB
        self.assertEqual(build_seed.allocate(8, 3500), (2, 4, 4))      # a laptop: memory decides the readers
        self.assertEqual(build_seed.allocate(6, 16000), (1, 6, 6))
        self.assertEqual(build_seed.allocate(4, 1000), (1, 1, 1))      # too little memory: one of each
        self.assertEqual(build_seed.allocate(32, 14600, containers=16), (16, 2, 12))
        self.assertEqual(build_seed.allocate(32, 14600, services=8), (4, 8, 12))

    def test_seed_dry_run(self):
        out = subprocess.run([sys.executable, "-I", str(ROOT / "native-harmonyos" / "build_seed.py"), "--dry-run", "--containers", "3"],
                             capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        for part in ("seed_in_container.py plan 3", "totk-seed-w03", "seed_in_container.py work", "seed_in_container.py pack v143 256"):
            self.assertIn(part, out.stdout)

    def test_local_settings_file_is_read_by_python(self):
        import tempfile, ohos_env
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "local.ohos.ps1"
            p.write_text("$env:TOTK_DEVECO = 'X:\\Tools\\DevEco'   # a comment\n# $env:TOTK_PWSH = 'ignored'\n$env:TOTK_HDC_TARGET = ''\n",
                         encoding="utf-8")
            self.assertEqual(ohos_env.read_local(p), {"TOTK_DEVECO": "X:\\Tools\\DevEco", "TOTK_HDC_TARGET": ""})

    def test_install_skill_matches_the_repository(self):
        if not ((ROOT / ".claude" / "skills" / "install-totk-harmonyos" / "SKILL.md").is_file() and (ROOT / "AGENTS.md").is_file()):
            self.skipTest("the standalone project's install skill and AGENTS.md are not in this copy (NX2Harmony leaves them out)")
        import ohos_env, prepare_v143
        text = (ROOT / ".claude" / "skills" / "install-totk-harmonyos" / "SKILL.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"^---\r?\nname: install-totk-harmonyos\r?\ndescription: .+\r?\n---\r?\n")
        scripts = set(re.findall(r"native-harmonyos[\\/][\w./\\-]+\.(?:py|ps1|sh)", text))
        self.assertGreater(len(scripts), 5)
        for rel in scripts:
            if Path(rel.replace("\\", "/")).name.startswith("local.ohos."):
                continue   # this machine's own settings: ignored by git, so a fresh clone or a package has none
            self.assertTrue((ROOT / rel.replace("\\", "/")).is_file(), rel)
        for step in re.findall(r"--only (\w+)", text):
            self.assertIn(step, prepare_v143.STEPS)
        self.assertIn(ohos_env.BUNDLE, text)
        self.assertIn(".claude/skills/install-totk-harmonyos/SKILL.md", (ROOT / "AGENTS.md").read_text(encoding="utf-8"))

    def test_no_machine_paths_in_tracked_text(self):
        files = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True).stdout.split()
        pattern = re.compile(r"[A-Z]:\\Users\\[^\\\s]+|/Users/[a-z0-9_-]+/")
        for f in files:
            if f.endswith((".md", ".py", ".ps1", ".sh", ".ets", ".json5")):
                text = (ROOT / f).read_text(encoding="utf-8", errors="replace")
                self.assertIsNone(pattern.search(text), f)


if __name__ == "__main__":
    unittest.main()
