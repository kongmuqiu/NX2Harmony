#!/usr/bin/env python3
"""Bring a game backend's sources into backends/<folder> of this repository, or record NX2's own changes to them.

    python packaging/backend_import.py <backend> <project folder> [--rev <commit>]               import
    python packaging/backend_import.py <backend> <project folder> [--rev <commit>] --save-patch  record NX2's changes
    python packaging/backend_import.py <backend> <project folder> [--rev <commit>] --check       compare, change nothing

Without --rev an import takes a git project's HEAD; --save-patch and --check take the commit backends/SOURCES.tsv
records (what was imported), and refuse a working folder that is no longer the recorded snapshot.

<backend> is a games.tsv backend name (totk_v143, za_202, oot_v131072); its copy here is backends/<folder>, the folder
nx2h/backends MARKERS names. Game work belongs to the backend's own project; NX2Harmony only takes its results in,
with NX2's own changes kept in backends/patches/<folder>.patch for that project to take over. The patches stay on
the maintainer's PC (ignored by git): they quote the project's original lines, which can name the PC they were
written on; an import refuses to run without the patch SOURCES.tsv names, so NX2's changes are never dropped.

The project's files: what a git project holds at --rev (HEAD when not given, never its working tree), else the project
folders and root files of a working folder (packaging/full_bundle.py SOURCE_DIRS / SOURCE_FILES); without what the full
package never carries (full_bundle._allowed: game material, build output, machine settings, signing, backups) and
without the standalone project's own AI-install instructions and hooks (SKIP: NX2Harmony's are the ones that apply
here). Text files are taken with LF line ends and a bundle name made on some PC (nx2h.u<6>.<slug>) becomes
nx2h.u000000.<slug>.

Import: those files with the patch applied (git apply), checked as the full package is (full_bundle._private: the
packing user's folder, git e-mail and name, this PC's name, a LAN address with a port, a cloud-drive folder; this PC's
own top folders; the phone the project's machine settings name), then written over backends/<folder> (files this
repository ignores there, such as prepared/ and artifacts/, are left alone; changes there that are not committed are
refused unless --force); backends/SOURCES.tsv records where they came from. When the patch no longer applies the import
is refused; --merge takes the parts that apply and leaves <file>.rej beside each file for the rest; when the project
already holds all of NX2's changes the patch is dropped. --save-patch: the text differences between the project's files
and backends/<folder> become the patch (after changing backends/<folder> here). --check: 0 when backends/<folder> is
SOURCES.tsv's revision with the patch applied (with --rev HEAD: when importing the project now would change nothing).

Pictures never come from a project unless packaging/bundle_pictures.tsv lists their sha256 (a port's working folder
holds the games' official art), and only at a path backends/<folder> has no listed picture for: NX2's pictures stay on
every import. Pictures are not in the patch (a binary patch would carry the replaced picture too)."""
from __future__ import annotations

import argparse
import datetime
import fnmatch
import hashlib
import io
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parents[1]
for p in (REPO, REPO / "packaging"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import full_bundle as fb  # noqa: E402

BACKENDS = REPO / "backends"
PATCHES = BACKENDS / "patches"
SOURCES = BACKENDS / "SOURCES.tsv"
SOURCES_HEADER = ("# Where each folder of backends/ comes from (packaging/backend_import.py keeps the backends' rows).\n"
                  "# revision: a commit of a git project, or snapshot-<digest of the files taken> for a working folder;\n"
                  "# patch: NX2's own changes against that revision (kept on the maintainer's PC, not committed), - for none;\n"
                  "# the public upstreams (nxnative, totk-android) are pinned here and fetched, not copied (totk-android\n"
                  "# also in each backend's native-harmonyos/UPSTREAM_PIN).\n"
                  "folder\tbackend\torigin\trevision\timported\tpatch\n")
# the standalone project's own AI-install instructions, hooks and CI: in this repository NX2Harmony's apply
SKIP = (".claude/", ".githooks/", ".github/", "AGENTS.md")
GIT = ["git", "-c", "core.autocrlf=false", "-c", "core.safecrlf=false", "-c", "core.quotepath=false",
       "-c", "diff.noprefix=false", "-c", "diff.mnemonicPrefix=false"]


def _picture(rel: str) -> bool:
    return any(fnmatch.fnmatch(PurePosixPath(rel).name.lower(), pat) for pat in fb.PICTURES)


def _taken(rel: str) -> bool:
    return fb._allowed(Path(rel)) and not any(rel == s or rel.startswith(s) for s in SKIP)


def _normal(data: bytes) -> bytes:
    """Text with LF line ends and a neutral bundle name; binaries as they are."""
    if not fb._text(data):
        return data
    return fb.MACHINE_BUNDLE.sub(fb.NEUTRAL_BUNDLE, data.replace(b"\r\n", b"\n"))


def _run(cmd: list, cwd=None, data=None) -> bytes:
    r = subprocess.run(cmd, cwd=cwd, input=data, capture_output=True)
    if r.returncode:
        sys.exit(f"refusing: {' '.join(str(c) for c in cmd[:6])} ... failed: "
                 + r.stderr.decode("utf-8", "replace").strip())
    return r.stdout


def folder_of(backend: str) -> str:
    from nx2h.backends import MARKERS
    if backend not in MARKERS:
        sys.exit(f"no backend {backend!r}: one of {', '.join(MARKERS)}")
    return MARKERS[backend][1]


def project_files(project: Path, rev: str = "") -> tuple:
    """(files, origin, revision): the project's files (path -> bytes, normalised), where it is published and which
    revision they are."""
    if (project / ".git").exists() or rev:
        rev = _run(GIT + ["-C", str(project), "rev-parse", "--verify", (rev or "HEAD") + "^{commit}"]).decode().strip()
        odd = [ln.split("\t", 1)[-1] for ln in _run(GIT + ["-C", str(project), "ls-tree", "-r", rev]).decode(
            "utf-8").splitlines() if ln.split(" ", 1)[0] in ("120000", "160000")]
        if odd:   # git archive would turn a symbolic link into a file and leave a submodule out
            sys.exit("refusing: the project holds symbolic links or submodules, which an import cannot carry: "
                     + ", ".join(odd[:10]))
        tar = _run(GIT + ["-c", "core.eol=lf", "-C", str(project), "archive", "--format=tar", rev])
        files = {}
        with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
            for m in tf:
                if m.isfile():
                    rel = "/".join(fb._safe_parts(m.name))
                    if _taken(rel):
                        files[rel] = _normal(tf.extractfile(m).read())
                elif not m.isdir() and m.name != "pax_global_header":
                    sys.exit(f"refusing: {m.name} at {rev[:12]} is not a plain file")
        url = subprocess.run(GIT + ["-C", str(project), "remote", "get-url", "origin"], capture_output=True,
                             text=True).stdout.strip()
        # the project's name, not its URL: the URL names its owner's account (a person), and may carry a token
        name = re.sub(r"\.git$", "", url.rstrip("/").replace("\\", "/").rsplit("/", 1)[-1]) if url else project.name
        return files, f"{name} (git project)", rev
    files = {rel.as_posix(): _normal((project / rel).read_bytes()) for rel in fb._files(project)}
    files = {rel: data for rel, data in files.items() if _taken(rel)}
    digest = hashlib.sha256()
    for rel in sorted(files):
        digest.update(rel.encode("utf-8") + b"\0" + hashlib.sha256(files[rel]).digest())
    return files, f"{project.name} working folder (unpublished)", "snapshot-" + digest.hexdigest()[:16]


def repo_files(folder: str) -> dict:
    """backends/<folder> as this repository has it: tracked files and new ones it does not ignore (normalised)."""
    out = _run(GIT + ["-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--",
                      f"backends/{folder}/"])
    files = {}
    for p in out.decode("utf-8").split("\0"):
        if p and (REPO / p).is_file():
            files[p[len(f"backends/{folder}/"):]] = _normal((REPO / p).read_bytes())
    return files


def _pictures(files: dict, have: dict) -> tuple:
    """The pictures after an import: every listed picture backends/<folder> already has stays (NX2 looked at them;
    they are not in the patch), and a project's picture comes in only at a path backends/ has none for, and only when
    bundle_pictures.tsv lists it. Returns (pictures, notes, refused)."""
    ok = fb._pictures_ok()
    out = {rel: data for rel, data in have.items() if _picture(rel) and hashlib.sha256(data).hexdigest() in ok}
    notes, refused = [], []
    for rel, data in sorted(files.items()):
        if not _picture(rel):
            continue
        if rel in out:
            if out[rel] != data:
                notes.append(f"  {rel}: backends/ keeps its own picture (the project's differs"
                             + ("" if hashlib.sha256(data).hexdigest() in ok else " and is not listed") + ")")
        elif hashlib.sha256(data).hexdigest() in ok:
            out[rel] = data
        else:
            refused.append(f"  {rel}: not in {fb.PICTURES_OK.name} and backends/ has no listed picture there")
    return out, notes, refused


def _tree(files: dict, root: Path) -> None:
    for rel, data in files.items():
        p = root.joinpath(*rel.split("/"))
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def _git_tmp(root: Path) -> None:
    """A throwaway repository holding root's files as committed (no line-end conversion), so git apply and git diff
    work with paths relative to it whatever folder it is in."""
    _run(GIT + ["init", "-q", str(root)])
    (root / ".git" / "info" / "attributes").write_text("* -text\n", encoding="utf-8")
    _run(GIT + ["-C", str(root), "add", "-A", "-f"])   # -f: the backend's own .gitignore must not hide its files
    _run(GIT + ["-C", str(root), "-c", "user.name=nx2h", "-c", "user.email=nx2h@localhost", "commit", "-q",
                "--allow-empty", "--no-verify", "-m", "project"])


def _text_only(files: dict) -> dict:
    return {rel: data for rel, data in files.items() if not _picture(rel)}


def make_patch(files: dict, have: dict) -> bytes:
    """The text differences from the project's files to backends/<folder>, as git diff writes them."""
    binaries = sorted(rel for rel in set(files) | set(have) if not _picture(rel)
                      and not (fb._text(files.get(rel, b"")) and fb._text(have.get(rel, b"")))
                      and files.get(rel) != have.get(rel))
    if binaries:
        sys.exit("refusing: binary files differ from the project's (a patch holds text only):\n"
                 + "\n".join(f"  {b}" for b in binaries))
    with tempfile.TemporaryDirectory() as t:
        root = Path(t) / "tree"
        _tree(_text_only(files), root)
        _git_tmp(root)
        for rel in files:
            if not _picture(rel) and rel not in have:
                root.joinpath(*rel.split("/")).unlink()
        _tree(_text_only(have), root)
        _run(GIT + ["-C", str(root), "add", "-A", "-f"])
        return _run(GIT + ["-C", str(root), "diff", "--cached", "--no-color", "--no-ext-diff", "--no-renames",
                           "--full-index"])


def apply_patch(files: dict, patch: bytes, merge: bool = False) -> tuple:
    """The project's text files with the patch applied (pictures are not in it), and how: "applied", "none" (no
    patch), "taken over" (the project already holds all of NX2's changes), or "rejected" (with merge: the parts that
    apply, plus <file>.rej for each part that does not, to resolve by hand). Without merge a patch that does not apply
    is refused."""
    if not patch.strip():
        return _text_only(files), "none"
    with tempfile.TemporaryDirectory() as t:
        root = Path(t) / "tree"
        _tree(_text_only(files), root)
        _git_tmp(root)

        def apply(*opts) -> subprocess.CompletedProcess:
            return subprocess.run(GIT + ["-C", str(root), "apply", "--whitespace=nowarn", *opts, "-"], input=patch,
                                  capture_output=True)

        how = "applied"
        r = apply()
        if r.returncode:
            if apply("--reverse", "--check").returncode == 0:
                return _text_only(files), "taken over"
            if not merge:
                sys.exit("refusing: NX2's patch no longer applies to the project's files. Run the import again with "
                         "--merge: the parts that apply go in, each part that does not is left beside its file as "
                         "<file>.rej to resolve by hand in backends/, then --save-patch:\n"
                         + r.stderr.decode("utf-8", "replace").strip())
            apply("--reject")
            how = "rejected"
        out = {}
        for p in root.rglob("*"):
            rel = p.relative_to(root).as_posix()
            if p.is_file() and not rel.startswith(".git/"):
                out[rel] = p.read_bytes()
        return out, how


# top folders every Windows PC has (a default such as C:\Program Files\Huawei\DevEco Studio names no one)
COMMON_TOPS = {"windows", "program files", "program files (x86)", "programdata", "users", "$recycle.bin",
               "system volume information", "recovery", "perflogs", "msocache", "intel", "amd", "nvidia"}


def pc_folders() -> list:
    """This PC's own top folders on every drive (X:\\Projects, X:\\Work ...), as (drive, folder)."""
    import string
    tops = []
    for letter in string.ascii_uppercase[2:]:
        root = Path(f"{letter}:\\")
        try:
            tops += [(f"{letter}:", d.name) for d in root.iterdir() if d.is_dir() and d.name.lower() not in COMMON_TOPS]
        except OSError:
            continue
    return tops


def _device_marks(project: Path) -> dict:
    """The phone a porter's machine settings name (local.ohos.env / .ps1 and their copies, which never come in): its
    hdc serial or Wi-Fi address, and a private hdc server, so a script that keeps one as a default is refused."""
    values = set()
    for p in list(project.glob("native-harmonyos/local.ohos.*")) + list(project.glob("local.ohos.*")):
        if p.is_file() and "example" not in p.name:
            text = p.read_text(encoding="utf-8", errors="replace")
            values |= set(re.findall(r"TOTK_HDC_(?:TARGET|SERVER)\s*=?\s*['\"]?([^'\"\s#;]+)", text))
    values = {v for v in values if len(v) >= 8 and not v.startswith("127.0.0.1")
              and v not in ("localhost", "$env:TOTK_HDC_TARGET")}
    return {"the porter's phone or hdc server": re.compile(b"|".join(re.escape(v.encode("utf-8")) for v in sorted(values)))} \
        if values else {}


def vet(files: dict, project: Path) -> list:
    """What in the files would tell who made them or where (full_bundle's checks; this PC's own top folders, refused
    here: a repository is no place for them; the phone the project's machine settings name)."""
    marks = dict(fb._private())
    marks.update(_device_marks(project))
    tops = {(Path(r).resolve().drive, Path(r).resolve().parts[1]) for r in (REPO, project) if len(Path(r).resolve().parts) > 1}
    tops |= set(pc_folders()) if sys.platform == "win32" else set()
    if tops:
        marks["this PC's own folder"] = re.compile(rb"(?i)(?<![\w])(?:" + b"|".join(
            re.escape(d.encode("utf-8")) + rb"[\\/]+" + re.escape(t.encode("utf-8")) for d, t in sorted(tops))
            + rb")(?![\w-])")
    found = []
    for rel, data in sorted(files.items()):
        if not fb._text(data):
            continue
        for what, rx in marks.items():
            for m in rx.finditer(data):
                line = data.count(b"\n", 0, m.start()) + 1
                found.append(f"  {rel}:{line}: {what}")
    return found


def read_sources() -> list:
    if not SOURCES.is_file():
        return []
    rows = []
    for line in SOURCES.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("#") and not line.startswith("folder\t"):
            rows.append(line.split("\t"))
    return rows


def write_sources(row: list) -> None:
    rows = [r for r in read_sources() if r[0] != row[0]] + [row]
    text = SOURCES_HEADER + "".join("\t".join(r) + "\n" for r in sorted(rows, key=lambda r: r[0]))
    SOURCES.write_bytes(text.encode("utf-8"))   # LF (write_text has no newline= before Python 3.10)


def write_folder(folder: str, result: dict, have: dict) -> tuple:
    """backends/<folder> becomes result; returns (written, removed)."""
    dst = BACKENDS / folder
    removed = 0
    for rel in sorted(set(have) - set(result)):
        dst.joinpath(*rel.split("/")).unlink()
        removed += 1
    written = 0
    for rel, data in sorted(result.items()):
        p = dst.joinpath(*rel.split("/"))
        if p.is_file() and _normal(p.read_bytes()) == data:
            continue
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        written += 1
    for d in sorted((p for p in dst.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        try:
            d.rmdir()   # only empty ones
        except OSError:
            pass
    return written, removed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("backend")
    ap.add_argument("project", type=Path)
    ap.add_argument("--rev", default="", help="the commit of a git project (default HEAD)")
    ap.add_argument("--date", default="", help="the import date for SOURCES.tsv (default today)")
    ap.add_argument("--merge", action="store_true", help="when NX2's patch no longer applies: take the parts that do and "
                    "leave <file>.rej for each part that does not, to resolve by hand, then --save-patch")
    ap.add_argument("--force", action="store_true", help="import over changes in backends/<folder> that are not "
                    "committed (they are lost)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--save-patch", action="store_true", help="record backends/<folder>'s differences as the patch")
    mode.add_argument("--check", action="store_true", help="exit 1 when backends/<folder> is not SOURCES.tsv's revision "
                      "with the patch applied (--rev HEAD: what an import of the project now would change)")
    a = ap.parse_args(argv)
    from nx2h.backends import MARKERS
    folder = folder_of(a.backend)
    project = a.project.resolve()
    if not project.is_dir():
        sys.exit(f"no project folder {project}")
    row = next((r for r in read_sources() if r[0] == folder), None)
    rev = a.rev
    if not rev and (a.save_patch or a.check) and row and re.fullmatch(r"[0-9a-f]{40}", row[3]):
        rev = row[3]   # the patch is against what was imported, not wherever the project is now
    files, origin, revision = project_files(project, rev)
    if (a.save_patch or a.check) and row and row[3].startswith("snapshot-") and revision != row[3]:
        sys.exit(f"refusing: {project} is no longer the snapshot backends/{folder} was imported from ({row[3]}, now "
                 f"{revision}): commit your backends/ changes first, import the project (that carries NX2's patch "
                 "over; --merge when it no longer applies), then change backends/ and --save-patch")
    if not files:
        sys.exit(f"refusing: no project files in {project}")
    have = repo_files(folder)
    patch_file = PATCHES / f"{folder}.patch"
    date = a.date or datetime.date.today().isoformat()
    if a.save_patch:
        if any((BACKENDS / folder).rglob("*.rej")):
            sys.exit(f"refusing: backends/{folder} still holds .rej files: resolve and delete them first")
        patch = make_patch(files, have)
        PATCHES.mkdir(parents=True, exist_ok=True)
        if patch.strip():
            patch_file.write_bytes(patch)
        elif patch_file.exists():
            patch_file.unlink()
        if apply_patch(files, patch)[0] != _text_only(have):
            sys.exit("refusing: the saved patch does not give backends/ back (report this)")
        for rel in sorted(rel for rel in files if _picture(rel) and rel in have and files[rel] != have[rel]):
            print(f"  {rel}: backends/ has its own picture here (kept on every import; not in the patch)")
        write_sources([folder, a.backend, origin, revision, date,
                       f"patches/{folder}.patch" if patch.strip() else "-"])
        print(f"{folder}: patch {len(patch)} bytes against {revision[:16]} ({origin})")
        return 0
    if row and row[5] != "-" and not patch_file.is_file():
        sys.exit(f"refusing: NX2's patch {patch_file} is not on this PC (SOURCES.tsv names it): without it the import "
                 f"would drop NX2's changes to backends/{folder}")
    if not a.check and not a.force:
        dirty = _run(GIT + ["-C", str(REPO), "status", "--porcelain", "--untracked-files=all", "--",
                            f"backends/{folder}/"]).decode("utf-8").strip()
        if dirty:
            sys.exit(f"refusing: backends/{folder} has changes that are not committed (an import writes over them): "
                     "commit them (and --save-patch if they are NX2's), or --force to lose them:\n"
                     + "\n".join("  " + ln for ln in dirty.splitlines()[:20]))
    patch = patch_file.read_bytes() if patch_file.is_file() else b""
    result, how = apply_patch(files, patch, merge=a.merge and not a.check)
    pictures, notes, refused = _pictures(files, have)
    result.update(pictures)
    found = vet(result, project)
    marker = MARKERS[a.backend][0]
    problems = []
    if refused:
        problems.append("pictures nobody has looked at (check each: no game's or company's art; then list its sha256 "
                        f"in {fb.PICTURES_OK.name}):\n" + "\n".join(refused))
    if found:
        problems.append("files that would tell who made them or where (clean them in the project, or in backends/ "
                        "and --save-patch):\n" + "\n".join(found))
    if marker not in result:
        problems.append(f"the project has no {marker}: not the {a.backend} backend")
    if problems:
        sys.exit("refusing: " + "\n".join(problems))
    if notes:
        print("\n".join(notes))
    changed = sorted(rel for rel in set(result) | set(have) if result.get(rel) != have.get(rel))
    if a.check:
        for rel in changed[:40]:
            print(f"  differs: {rel}" + ("" if rel in result else " (not in the project)")
                  + ("" if rel in have else " (not in backends/)"))
        print(f"{folder}: {len(changed)} files differ from {revision[:16]} with NX2's patch"
              + (" (the project already holds NX2's changes)" if how == "taken over" else ""))
        return 1 if changed else 0
    written, removed = write_folder(folder, result, have)
    hidden = subprocess.run(GIT + ["-C", str(REPO), "check-ignore", "--no-index", "-z", "--stdin"],
                            input=b"".join(f"backends/{folder}/{rel}".encode("utf-8") + b"\0" for rel in result),
                            capture_output=True).stdout
    hidden = [p for p in hidden.decode("utf-8").split("\0") if p]
    if hidden:
        print("WARNING: this repository's ignore rules hide these backend files from git (fix .gitignore):\n"
              + "\n".join(f"  {p}" for p in hidden[:40]))
    if how == "taken over" and patch_file.exists():
        patch_file.unlink()   # the project holds all of NX2's changes now
    keep = how in ("applied", "rejected")
    write_sources([folder, a.backend, origin, revision, date, f"patches/{folder}.patch" if keep else "-"])
    print(f"{folder}: {len(result)} files from {revision[:16]} ({origin}); {written} written, {removed} removed"
          + {"applied": f"; NX2's patch applied ({len(patch)} bytes)", "none": "",
             "taken over": "; the project holds all of NX2's changes now: no patch any more",
             "rejected": "; NX2's patch only partly applied: resolve each .rej file in backends/ by hand, delete it, "
                         "then --save-patch"}[how])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
