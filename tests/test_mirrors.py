"""nx2h/mirrors.py while it is switched off (ENABLED = False until it has been tested on a fresh PC): every caller gets the
official source alone and nothing is measured. Any network use here fails the test."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nx2h import mirrors  # noqa: E402


def no_network(*args, **kwargs):
    raise AssertionError("mirrors.py went to the network while switched off")


class SwitchedOff(unittest.TestCase):
    def setUp(self):
        for name, value in (("ENABLED", False), ("_open", no_network), ("_cache", {})):
            p = mock.patch.object(mirrors, name, value)
            p.start()
            self.addCleanup(p.stop)

    def test_ranked_gives_only_the_official_source(self):
        expect = {"debian": "https://deb.debian.org", "ubuntu": "https://archive.ubuntu.com/ubuntu",
                  "pypi": "https://pypi.org/simple/", "nuget": "https://api.nuget.org/v3/index.json"}
        for kind, url in expect.items():
            with self.subTest(kind):
                got = mirrors.ranked(kind, say=no_network)
                self.assertEqual([(s.url, s.official) for s in got], [(url, True)])

    def test_registries(self):
        hub = mirrors.ranked("registry", image="python:3.14")
        self.assertEqual([(s.url, s.official) for s in hub], [("registry-1.docker.io", True)])
        self.assertEqual(hub[0].ref("python:3.14"), "python:3.14")
        mcr = mirrors.ranked("registry", image="mcr.microsoft.com/dotnet/sdk:10.0-noble-aot")
        self.assertEqual([(s.url, s.official) for s in mcr], [("mcr.microsoft.com", True)])
        other = mirrors.ranked("registry", image="ghcr.io/owner/tool:1")
        self.assertEqual([(s.url, s.official) for s in other], [("ghcr.io", True)])

    def test_unknown_kind(self):
        with self.assertRaises(ValueError):
            mirrors.ranked("cdn")

    def test_what_the_steps_get(self):
        self.assertEqual(mirrors.xlate_envs(), [{}])
        self.assertEqual(mirrors.apt_attempts(), mirrors.LEGACY_APT)
        self.assertEqual(mirrors.apt_attempts(tries=2), mirrors.LEGACY_APT[:2])
        self.assertEqual(mirrors.pip_env(), {})
        self.assertEqual(mirrors.pip_attempts(), [""])
        self.assertEqual(mirrors.nuget_envs(), [{}])

    def test_legacy_apt_is_what_the_dockerfile_tries(self):
        self.assertEqual(len(mirrors.LEGACY_APT), 4)
        self.assertEqual(mirrors.LEGACY_APT[:2], ["", ""], "the Dockerfile's own source first, twice")


class Images(unittest.TestCase):
    def test_split_image(self):
        cases = {
            "python:3.14": ("docker.io", "library/python", "3.14"),
            "python": ("docker.io", "library/python", "latest"),
            "owner/tool:2": ("docker.io", "owner/tool", "2"),
            "mcr.microsoft.com/dotnet/sdk:10.0-noble-aot": ("mcr.microsoft.com", "dotnet/sdk", "10.0-noble-aot"),
            "localhost:5000/tools": ("localhost:5000", "tools", "latest"),
            "localhost/tools:1": ("localhost", "tools", "1"),
        }
        for image, parts in cases.items():
            with self.subTest(image):
                self.assertEqual(mirrors.split_image(image), parts)

    def test_a_mirror_names_the_image_under_its_own_host(self):
        s = mirrors.Source("mirror", "docker.mirror.example", False)
        self.assertEqual(s.ref("python:3.14"), "docker.mirror.example/library/python:3.14")
        self.assertEqual(s.ref("mcr.microsoft.com/dotnet/sdk:10.0-noble-aot"),
                         "docker.mirror.example/dotnet/sdk:10.0-noble-aot")


class Ranking(unittest.TestCase):
    """_rank, the part that orders measured sources (no measuring here: speeds are set by hand)."""

    def sources(self, official_kbps, *others):
        out = [mirrors.Source("official", "https://official.example", True)]
        out[0].kbps = official_kbps
        for i, kbps in enumerate(others):
            s = mirrors.Source(f"m{i}", f"https://m{i}.example", False)
            s.kbps = kbps
            out.append(s)
        return out

    def test_official_first_when_nearly_as_fast(self):
        ranked = mirrors._rank(self.sources(900, 1000, 500))
        self.assertEqual([s.name for s in ranked], ["official", "m0", "m1"])

    def test_fastest_first_otherwise(self):
        ranked = mirrors._rank(self.sources(100, 1000, 0))
        self.assertEqual([s.name for s in ranked], ["m0", "official"])

    def test_nothing_answered_gives_the_official_one(self):
        ranked = mirrors._rank(self.sources(0, 0, 0))
        self.assertEqual([s.name for s in ranked], ["official"])


if __name__ == "__main__":
    unittest.main()
