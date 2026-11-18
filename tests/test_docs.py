"""The repository's own Markdown: every relative link leads to a file in the repository, every #anchor (on the same page
or another one) to a heading by GitHub's rules, Chinese ones included; the install guides keep the section names other
pages link to; and nothing names the maintainer's PC, network or e-mail, nor the PC these tests run on. Files are found
at run time: a page that is not there (yet) is not checked."""
from __future__ import annotations

import getpass
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import unittest
from pathlib import Path
from urllib.parse import unquote

REPO = Path(__file__).resolve().parents[1]

FENCE = re.compile(r"^ {0,3}(```|~~~).*?^ {0,3}\1[^\n]*$", re.M | re.S)
CODE = re.compile(r"(`+)(?:(?!\1).)+?\1", re.S)
COMMENT = re.compile(r"<!--.*?-->", re.S)
INLINE = re.compile(r"\]\(\s*<?([^)\s>]*)>?(?:\s+(?:\"[^\"]*\"|'[^']*'))?\s*\)")
REFDEF = re.compile(r"^ {0,3}\[(?!\^)[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$", re.M)   # not a footnote [^1]: ...
HTML = re.compile(r"\b(?:href|src)\s*=\s*[\"']([^\"']+)[\"']", re.I)
SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
HEADING = re.compile(r"^ {0,3}(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$", re.M)
ID = re.compile(r"<[a-z][a-z0-9]*\s[^>]*\b(?:id|name)\s*=\s*[\"']([^\"']+)[\"']", re.I)


def git_files():
    """The files git would commit (tracked, or new and not ignored), as / paths; None without git."""
    if not shutil.which("git") or not (REPO / ".git").exists():
        return None
    r = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                       capture_output=True)
    if r.returncode:
        return None
    return {p.decode("utf-8", "surrogateescape") for p in r.stdout.split(b"\0") if p}


def pages() -> list:
    """README.md, CONTRIBUTING.md and the other top-level pages, docs/*.md and every skill's SKILL.md that exist and that
    git does not ignore (a working note in docs/ that stays on the maintainer's PC is not a page)."""
    found = sorted(REPO.glob("*.md")) + sorted((REPO / "docs").glob("*.md")) + sorted(REPO.glob(".claude/skills/*/SKILL.md"))
    known = git_files()
    known = None if known is None else {k.lower() for k in known}   # a name whose case changed on Windows is still a page
    return [p for p in found if p.is_file() and (known is None or p.relative_to(REPO).as_posix().lower() in known)]


def blocks(text: str) -> str:
    """The text without fenced code blocks and comments."""
    return FENCE.sub("", COMMENT.sub("", text))


def prose(text: str) -> str:
    """The text without code blocks, code spans and comments: a link-like thing in code is not a link."""
    return CODE.sub("", blocks(text))


def exact(target: Path) -> str:
    """'' when every part of target's path below the repository exists with this very case (GitHub's file names are
    case-sensitive, Windows' are not), else the part that does not."""
    here = REPO
    for part in Path(os.path.normpath(str(target))).relative_to(REPO).parts:
        try:
            names = os.listdir(str(here))
        except OSError:
            return part
        if part not in names:
            return part
        here = here / part
    return ""


def links(text: str) -> list:
    t = prose(text)
    return [m.group(1) for rx in (INLINE, REFDEF, HTML) for m in rx.finditer(t)]


def slug(heading: str) -> str:
    """GitHub's anchor for a heading, made from the text the heading shows: code spans as they are; outside them tags
    dropped, links and images reduced to their text, _emphasis_ marks dropped. Then lower case, and every character
    that is not a letter, mark, digit, connector (_), hyphen or space dropped (Chinese characters are letters and stay;
    full-width punctuation such as （）：、 goes), and each space a hyphen."""
    shown = []
    for i, part in enumerate(re.split(r"(`[^`]*`)", heading)):
        if i % 2:
            shown.append(part[1:-1])
            continue
        part = re.sub(r"<[^>]+>", "", part)
        part = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", part)
        shown.append(re.sub(r"(?<!\w)_+|_+(?!\w)", "", part))
    s = "".join(shown).lower()
    s = "".join(c for c in s if c in " -" or unicodedata.category(c)[0] in "LMN" or unicodedata.category(c) == "Pc")
    return s.replace(" ", "-")


def anchors(path: Path) -> set:
    text = blocks(path.read_text(encoding="utf-8"))
    out, seen = set(ID.findall(text)), {}
    for m in HEADING.finditer(text):
        s = slug(m.group(2))
        n = seen.get(s, 0)
        seen[s] = n + 1
        out.add(s if n == 0 else f"{s}-{n}")
    return {a.lower() for a in out}


def inside_repo(p: Path) -> bool:
    try:
        Path(os.path.normpath(str(p))).relative_to(REPO)
        return True
    except ValueError:
        return False


class Links(unittest.TestCase):
    def test_some_pages_are_found(self):
        names = [p.relative_to(REPO).as_posix() for p in pages()]
        self.assertIn("README.md", names)

    def targets(self):
        """(page, link, file it names, anchor) for every relative link; a bare #anchor names the page it is on."""
        for page in pages():
            for link in links(page.read_text(encoding="utf-8")):
                if not link or link.startswith("//") or SCHEME.match(link):
                    continue
                path, _, frag = link.partition("#")
                path = unquote(path.split("?", 1)[0])
                if not path:
                    yield page, link, page, unquote(frag)
                    continue
                target = REPO / path.lstrip("/") if path.startswith("/") else page.parent / path
                yield page, link, target, unquote(frag)

    def test_relative_links_lead_to_files_in_the_repository(self):
        for page, link, target, _ in self.targets():
            with self.subTest(page=page.relative_to(REPO).as_posix(), link=link):
                self.assertTrue(inside_repo(target), "the link leaves the repository (it breaks on GitHub)")
                self.assertTrue(target.exists(), "no such file or folder")
                self.assertEqual(exact(target), "", "named in another case than the file has (breaks on GitHub)")

    def test_anchors_find_a_heading(self):
        """#anchor on the same page and page.md#anchor into another one."""
        for page, link, target, frag in self.targets():
            if not frag or target.suffix.lower() != ".md" or not target.is_file():
                continue
            with self.subTest(page=page.relative_to(REPO).as_posix(), link=link):
                self.assertIn(frag.lower(), anchors(target), f"no heading or <a id> for #{frag}")

    def test_slug(self):
        cases = {
            "What's new": "whats-new",
            "Recommended install (hand it to an AI assistant)": "recommended-install-hand-it-to-an-ai-assistant",
            "准备电脑": "准备电脑",
            "推荐安装（交给 AI 助手）": "推荐安装交给-ai-助手",
            "签名、安装和第一次启动：常见问题": "签名安装和第一次启动常见问题",
            "“环境”页 / 「一键准备」": "环境页--一键准备",
            "`nx2h.py build` options": "nx2hpy-build-options",
            "`full_bundle.py` and __init__": "full_bundlepy-and-init",
            "Tears of the Kingdom 1.4.3": "tears-of-the-kingdom-143",
            "[Install](docs/install.md) — Windows 10/11": "install--windows-1011",
            "Z-A: shaders <sup>1</sup>": "z-a-shaders-1",
        }
        for heading, want in cases.items():
            with self.subTest(heading):
                self.assertEqual(slug(heading), want)

    def test_repeated_headings_get_a_number(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "x.md"
            p.write_text("# 说明\n\n## Notes\n\n## Notes\n\n```\n## Notes\n```\n\n## 说明\n", encoding="utf-8")
            self.assertEqual(anchors(p), {"说明", "notes", "notes-1", "说明-1"})


INSTALL = {
    "docs/install.md": ["# Install", "## What you need", "## Set up the PC", "## Set up the phone", "## Build with the window",
                        "## Build from the command line", "## Signing", "## Where things are", "## When a build stops",
                        "## Known issues", "## Uninstall"],
    "docs/install.zh-CN.md": ["# 安装", "## 需要准备什么", "## 准备电脑", "## 准备手机", "## 用窗口构建", "## 用命令行构建", "## 签名",
                              "## 文件在哪", "## 构建停下时", "## 已知问题", "## 卸载"],
}


class InstallGuides(unittest.TestCase):
    def test_sections_other_pages_link_to_are_there_in_order(self):
        for rel, want in INSTALL.items():
            p = REPO / rel
            if not p.is_file():
                continue
            with self.subTest(rel):
                have = [f"{m.group(1)} {m.group(2)}" for m in HEADING.finditer(blocks(p.read_text(encoding="utf-8")))
                        if len(m.group(1)) <= 2]
                at = [have.index(h) if h in have else -1 for h in want]
                self.assertNotIn(-1, at, "missing: " + ", ".join(h for h, i in zip(want, at) if i < 0))
                self.assertEqual(at, sorted(at), "the sections are out of order")

    def test_the_install_skill_names_itself(self):
        p = REPO / ".claude" / "skills" / "install-nx2harmony" / "SKILL.md"
        if not p.is_file():
            self.skipTest("no install skill yet")
        self.assertRegex(p.read_text(encoding="utf-8"), r"^---\r?\nname: install-nx2harmony\r?\ndescription: .+\r?\n")


# What would tell who maintains the project or from where. Tests are left out of these fixed patterns: they hold
# made-up values on purpose. Failures name the line, never the text found.
USER_FOLDER = re.compile(r"(?i)\b[A-Z]:[\\/]+Users[\\/]+([A-Za-z0-9_][\w.-]*)")
GENERIC_USERS = {"public", "default", "you", "yourname", "your_name", "name", "username", "user", "example",
                 "owner", "guest", "runneradmin", "runner", "admin", "administrator", "root", "test", "tests", "dev", "home"}
LAN = re.compile(r"(?<![\d.])(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}(?::\d{2,5})(?!\d)")
EXAMPLE_LAN = "10.1.2.3"   # the documented example address
MAIL = re.compile(r"(?<![\w.+-])([\w.+-]+)@([\w-]+(?:\.[\w-]+)*\.[A-Za-z]{2,})\b")
PC_BUNDLE = re.compile(r"\bnx2h\.(u[0-9a-z]{6})\.")
EXAMPLE_PC_PARTS = {"uk3f9a2", "u000000"}   # the documented example and the neutral one of the full package
TEXT = {".md", ".py", ".pyw", ".txt", ".tsv", ".json", ".json5", ".yml", ".yaml", ".cmd", ".ps1", ".sh", ".spec",
        ".toml", ".cfg", ".ini", "",
        # the game backends' sources (backends/)
        ".ets", ".ts", ".c", ".h", ".cpp", ".hpp", ".inc", ".s", ".comp", ".map", ".dockerfile", ".html", ".js",
        ".css", ".svg", ".patch", ".gitignore", ".gitattributes", ".cmake", ".totk-android"}


def third_party_text(rel: str) -> bool:
    """A file that is someone else's: vendored code, licence and author lists (their authors' e-mail addresses are
    theirs to publish)."""
    name = Path(rel).name
    return ("vendor/" in rel or "third_party/" in rel or "/rawfile/licenses/" in rel or rel.startswith("packaging/licenses/")
            or "LICENSE" in name
            or "NOTICES" in name or "AUTHORS" in name or "COPYING" in name)


def ok_mail(local: str, domain: str) -> bool:
    d = domain.lower()
    return (local.lower() == "noreply" or d.endswith("noreply.github.com") or d.split(".")[-2:-1] == ["example"]
            or d == "example.com" or (local.lower() == "git" and d == "github.com"))


def line_of(text: str, at: int) -> int:
    return text.count("\n", 0, at) + 1


def this_pc() -> dict:
    """What names the PC these tests run on, worked out here and now: the folder this checkout sits in, the home
    folder, the user name, git's user.name and this PC's part of bundle names (nx2h/bundle.py, only when it comes from
    the MachineGuid and so writes nothing). What it is -> pattern. A drive root, a short or generic name is left out,
    so that only what is specific to one PC can fail the test."""
    out = {}
    for what, folder in (("the folder this checkout is in", REPO.parent), ("the home folder", Path.home())):
        s = str(folder)
        if folder == Path(folder.anchor) or len(folder.parts) < 2 or len(s) < 6:
            continue
        forms = {s, s.replace("\\", "/"), s.replace("\\", "\\\\")}
        out[what] = re.compile("(?:" + "|".join(re.escape(f) for f in sorted(forms)) + r")(?![\w-])", re.I)
    names = {Path.home().name}
    try:
        names.add(getpass.getuser())
    except (KeyError, OSError, ImportError):
        pass
    if shutil.which("git"):
        r = subprocess.run(["git", "-C", str(REPO), "config", "user.name"], capture_output=True, text=True)
        names.add(r.stdout.strip())
    names = {n for n in names if len(n) >= 3 and n.lower() not in GENERIC_USERS}
    if names:
        out["a user name of this PC"] = re.compile(
            r"(?<![\w.-])(?:" + "|".join(re.escape(n) for n in sorted(names)) + r")(?![\w-])", re.I)
        # in someone else's text (backends/, licences) a short name is an ordinary word ("lin", "david"): there only as
        # a folder
        out["a user name of this PC, as a folder"] = re.compile(
            r"(?i)(?:users|home)[\\/]+(?:" + "|".join(re.escape(n) for n in sorted(names)) + r")(?![\w.-])")
    import socket
    host = socket.gethostname()
    if len(host) >= 4 and host.lower() not in GENERIC_USERS:
        out["this PC's name"] = re.compile(r"(?<![\w.-])" + re.escape(host) + r"(?![\w-])", re.I)
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    try:
        from nx2h import bundle
    except ImportError:
        return out
    if bundle._machine_guid():
        out["this PC's part of bundle names"] = re.compile(r"(?<![\w-])" + re.escape(bundle.pc_part()) + r"(?![\w-])")
    return out


class Privacy(unittest.TestCase):
    def test_no_maintainer_machine_data(self):
        known = git_files()
        if known is None:
            self.skipTest("not a git checkout")
        for rel in sorted(known):
            p = REPO / rel
            if rel.startswith("tests/") or p.suffix.lower() not in TEXT or not p.is_file():
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            third_party = third_party_text(rel)
            with self.subTest(rel):
                for m in USER_FOLDER.finditer(text):
                    if m.group(1).lower() not in GENERIC_USERS:
                        self.fail(f"line {line_of(text, m.start())}: a Windows user folder")
                for m in LAN.finditer(text):
                    if not m.group(0).startswith(EXAMPLE_LAN + ":"):
                        self.fail(f"line {line_of(text, m.start())}: a LAN address with a port")
                for m in PC_BUNDLE.finditer(text):
                    if m.group(1) not in EXAMPLE_PC_PARTS:
                        self.fail(f"line {line_of(text, m.start())}: a bundle name made on a real PC")
                if not third_party:
                    for m in MAIL.finditer(text):
                        if not ok_mail(m.group(1), m.group(2)):
                            self.fail(f"line {line_of(text, m.start())}: an e-mail address")

    def test_nothing_names_this_pc(self):
        """The marks of the PC these tests run on, in every text file git would commit, tests included (their fixtures
        put such strings together at run time). Fails only on the PC whose data got in, and names the line only."""
        known = git_files()
        if known is None:
            self.skipTest("not a git checkout")
        marks = this_pc()
        if not marks:
            self.skipTest("nothing specific to this PC to look for")
        for rel in sorted(known):
            p = REPO / rel
            if p.suffix.lower() not in TEXT or not p.is_file():
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            others = rel.startswith("backends/") or third_party_text(rel)
            for what, rx in marks.items():
                if what == ("a user name of this PC" if others else "a user name of this PC, as a folder"):
                    continue
                m = rx.search(text)
                if m:
                    with self.subTest(rel, what=what):
                        self.fail(f"line {line_of(text, m.start())}: {what}")

    def test_this_pc_marks_find_what_they_should(self):
        """Each mark finds the folder it was made from, written in any of the usual ways, and not a longer name that
        starts the same. Only True or False is compared, so no path of this PC shows up in a failure."""
        marks = this_pc()
        for what, folder in (("the folder this checkout is in", REPO.parent), ("the home folder", Path.home())):
            rx = marks.get(what)
            if rx is None:
                continue
            s = str(folder)
            with self.subTest(what):
                for form in {s, s.replace("\\", "/"), s.replace("\\", "\\\\"), s.upper()}:
                    found = rx.search("log = '" + form + "/x.log'") is not None
                    self.assertTrue(found, "a way of writing it is missed")
                self.assertTrue(rx.search("log = '" + s + "x2/x.log'") is None, "a longer folder name is taken for it")


if __name__ == "__main__":
    unittest.main()
