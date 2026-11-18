"""Pack dist/NX2Harmony again: the icon, then PyInstaller (packaging/NX2Harmony.spec), then the licence texts. PyInstaller
empties the output folder, so the portable data/ beside the exe (settings, icon cache, logs) is moved aside first and
put back after.

    python packaging/build.py [--zip]

dist/NX2Harmony/licenses/ (beside NX2Harmony.exe) gets every licence text the packed app needs: NX2Harmony's own
LICENSE and THIRD_PARTY_NOTICES.md; from the Python that runs this build, CPython's LICENSE.txt and Doc/html/license.html
(the libraries CPython bundles: OpenSSL, libffi, bzip2, expat, zlib, libmpdec, ...), PyInstaller's COPYING.txt (the
bootloader in both exes), pyinstaller-hooks-contrib's LICENSE, cffi's LICENSE, cryptography's licences and SBOMs; and
the texts committed in packaging/licenses (COMMITTED: the LGPL and GPL 3 for Qt / PySide6 / shiboken6, the third-party
code inside the Qt binaries, where Qt's source is, cryptography's Rust crates). Those committed texts describe Qt
QT_VERSION and cryptography CRYPTOGRAPHY_VERSION. Refused, before anything is built, when a text is missing or empty or
the build Python has other versions of those two, so a release never goes out without its licences.
--zip also writes dist/NX2Harmony-<version>-windows-x64.zip, the app-only release asset: dist/NX2Harmony without its
data/, every entry under NX2Harmony/ (packaging/full_bundle.py makes the full package with the backends)."""
from __future__ import annotations

import argparse
import importlib.metadata as metadata
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "dist" / "NX2Harmony"
KEEP = ROOT / "build" / "data-kept"
COMMITTED_DIR = ROOT / "packaging" / "licenses"
# the texts committed in packaging/licenses, copied as they are (tests/test_project.py checks each is there)
COMMITTED = ("LGPL-3.0.txt", "GPL-3.0.txt", "QT-THIRD-PARTY.txt", "QT-SOURCES.txt", "CRYPTOGRAPHY-RUST.txt")
# the versions QT-THIRD-PARTY.txt, QT-SOURCES.txt and CRYPTOGRAPHY-RUST.txt describe
QT_VERSION = "6.10.3"
CRYPTOGRAPHY_VERSION = "50.0.2"
# licence files from the build Python's packages: distribution -> (file names it must have, name prefix in licenses/)
PACKAGES = {
    "pyinstaller": ({"COPYING.txt"}, "PyInstaller-"),
    "pyinstaller-hooks-contrib": ({"LICENSE"}, "pyinstaller-hooks-contrib-"),
    "cffi": ({"LICENSE"}, "cffi-"),
    "cryptography": ({"LICENSE", "LICENSE.APACHE", "LICENSE.BSD", "sbom.json", "cryptography-rust.cyclonedx.json"},
                     "cryptography/"),
}
LICENCE_NAME = re.compile(r"(LICEN[CS]E|COPYING|NOTICE)", re.I)


def _version_of(dist: str) -> str | None:
    try:
        return metadata.version(dist)
    except metadata.PackageNotFoundError:
        return None


def _package_licences(dist: str, needed: set, prefix: str) -> list[tuple[str, Path]]:
    """(name in licenses/, file) for each licence file and SBOM in dist's .dist-info: what is in its licenses/ and
    sboms/ folders (sboms/ kept as a folder), or a LICENSE* / COPYING* at its top (older wheels)."""
    try:
        files = metadata.distribution(dist).files or []
    except metadata.PackageNotFoundError:
        sys.exit(f"refusing: {dist} is not installed in the Python that runs this build (packaging/requirements-build.txt)")
    out = []
    for f in files:
        parts = f.parts
        if not parts[0].endswith(".dist-info") or len(parts) < 2:
            continue
        if len(parts) == 3 and parts[1] == "licenses":
            name = parts[2]
        elif len(parts) == 3 and parts[1] == "sboms":
            name = "sboms/" + parts[2]
        elif len(parts) == 2 and LICENCE_NAME.match(parts[1]):
            name = parts[1]
        else:
            continue
        if not prefix.endswith("/"):   # one file beside the others: PyInstaller-COPYING.txt, cffi-LICENSE.txt
            name = name.replace("/", "-") + ("" if "." in name else ".txt")
        out.append((prefix + name, Path(f.locate())))
    missing = needed - {src.name for _, src in out}
    if missing:
        sys.exit(f"refusing: {dist} {_version_of(dist)} has no {', '.join(sorted(missing))} in its .dist-info "
                 "(the licence texts the packed app must carry)")
    return out


def licences() -> list[tuple[str, Path]]:
    """Every (name in dist/NX2Harmony/licenses/, source file) the packed app carries; exits refusing when one is missing
    or empty, or when the committed texts describe other versions than this build packs."""
    base = Path(sys.base_prefix)
    pairs = [("NX2Harmony-LICENSE.txt", ROOT / "LICENSE"),
             ("switchfs-LICENSE.txt", ROOT / "nx2h" / "vendor" / "switchfs" / "LICENSE"),
             ("nxnative-LICENSE.txt", ROOT / "nx2h" / "vendor" / "nxnative-LICENSE"),
             ("THIRD_PARTY_NOTICES.md", ROOT / "THIRD_PARTY_NOTICES.md"),
             ("Python-LICENSE.txt", base / "LICENSE.txt"),
             ("Python-license.html", base / "Doc" / "html" / "license.html")]
    for dist, (needed, prefix) in PACKAGES.items():
        pairs += _package_licences(dist, needed, prefix)
    pairs += [(name, COMMITTED_DIR / name) for name in COMMITTED]
    for name, src in pairs:
        if not src.is_file() or src.stat().st_size == 0:
            why = {"Python-LICENSE.txt": " (CPython's licence: build with a python.org installation of Python, not the "
                                         "Microsoft Store or embeddable one)",
                   "Python-license.html": " (the licences of expat, zlib and libmpdec that CPython carries: install "
                                          "Python with its documentation)"}.get(name, "")
            sys.exit(f"refusing: {src} is missing or empty{why}")
    qt = _version_of("PySide6_Essentials") or _version_of("PySide6")
    for what, have, want in (("PySide6 (Qt)", qt, QT_VERSION), ("shiboken6", _version_of("shiboken6"), QT_VERSION),
                             ("cryptography", _version_of("cryptography"), CRYPTOGRAPHY_VERSION)):
        if have != want:
            sys.exit(f"refusing: this Python has {what} {have}, but packaging/licenses describes {want} "
                     "(update QT-THIRD-PARTY.txt, QT-SOURCES.txt, CRYPTOGRAPHY-RUST.txt and the versions in build.py, "
                     "or install the pinned versions)")
    return pairs


def write_licences(pairs: list[tuple[str, Path]]) -> Path:
    out = APP / "licenses"
    if out.exists():
        shutil.rmtree(out)
    for name, src in pairs:
        dst = out / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    return out


def version() -> str:
    m = re.search(r'^__version__\s*=\s*"([^"]+)"', (ROOT / "nx2h" / "__init__.py").read_text(encoding="utf-8"), re.M)
    if not m:
        sys.exit("refusing: no __version__ in nx2h/__init__.py")
    return m.group(1)


def write_zip() -> Path:
    """dist/NX2Harmony-<version>-windows-x64.zip: dist/NX2Harmony without data/, under NX2Harmony/."""
    z = ROOT / "dist" / f"NX2Harmony-{version()}-windows-x64.zip"
    tmp = z.with_name(z.name + ".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(APP.rglob("*")):
            rel = p.relative_to(APP)
            if p.is_file() and rel.parts[0] != "data":
                zf.write(p, Path("NX2Harmony") / rel)
    tmp.replace(z)
    return z


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", action="store_true", help="also write dist/NX2Harmony-<version>-windows-x64.zip")
    a = ap.parse_args()
    pairs = licences()   # refused here, before dist/ is touched
    subprocess.run([sys.executable, str(ROOT / "packaging" / "make_icon.py")], check=True)
    if (APP / "data").is_dir():
        if KEEP.exists():
            shutil.rmtree(KEEP)
        KEEP.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(APP / "data"), str(KEEP))
    try:
        r = subprocess.run([sys.executable, "-m", "PyInstaller", str(ROOT / "packaging" / "NX2Harmony.spec"),
                            "--noconfirm", "--distpath", str(ROOT / "dist"), "--workpath", str(ROOT / "build")])
        if r.returncode == 0:
            lic = write_licences(pairs)
            print(f"licenses: {len(pairs)} files in {lic}")
    finally:
        if KEEP.is_dir():
            APP.mkdir(parents=True, exist_ok=True)
            shutil.move(str(KEEP), str(APP / "data"))
    print(f"{'packed' if r.returncode == 0 else 'PyInstaller failed'}: {APP}")
    if r.returncode == 0 and a.zip:
        z = write_zip()
        print(f"zip: {z} ({z.stat().st_size / 2**20:.0f} MB)")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
