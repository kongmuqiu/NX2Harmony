# -*- mode: python -*-
# PyInstaller spec: dist/NX2Harmony/{NX2Harmony.exe (desktop app), nx2h.exe (command line), _internal/}.
#   python -m PyInstaller packaging/NX2Harmony.spec --noconfirm --distpath dist --workpath build
# One Analysis, two executables over the same files: the app runs nx2h.exe for a build (packaging/entry.py picks by
# the executable's name). Game icons are read in-process (nx2h/vendor/switchfs and cryptography, packed here); the
# backends' own scripts (prepare_v143.py) still run with the PC's Python.
from pathlib import Path

root = Path(SPECPATH).parent
icon = str(root / "packaging" / "NX2Harmony.ico")

a = Analysis(
    [str(root / "packaging" / "entry.py")],
    pathex=[str(root)],
    datas=[(str(root / "games.tsv"), "."), (str(root / "THIRD_PARTY_NOTICES.md"), "."), (str(root / "LICENSE"), "."),
           (str(root / "nx2h" / "vendor" / "switchfs" / "LICENSE"), "nx2h/vendor/switchfs"),
           (str(root / "nx2h" / "vendor" / "nxnative-LICENSE"), "nx2h/vendor")],
    hiddenimports=["nx2h.app", "nx2h.cli", "nx2h.backends.totk_v143", "nx2h.backends.za_202", "nx2h.backends.common", "nx2h.bundle", "nx2h.signing", "nx2h.agc", "nx2h.fonts", "nx2h.vendor.switchfs.extract_app_icon",
                   "nx2h.vendor.switchfs.extract_exefs", "nx2h.vendor.switchfs.extract_romfs",
                   "nx2h.vendor.switchfs.extract_shared_fonts", "cryptography.hazmat.primitives.ciphers", "cryptography.hazmat.primitives.ciphers.aead"],
    excludes=["tkinter", "PySide6.QtNetwork", "PySide6.QtQml", "PySide6.QtQuick", "PySide6.QtOpenGL",
              "PySide6.QtSql", "PySide6.QtTest", "PySide6.QtXml", "PySide6.QtDBus", "PySide6.QtConcurrent"],
    noarchive=False,
)
# opengl32sw.dll is Mesa's software OpenGL (llvmpipe, 20.6 MB) that PySide6 brings for Qt Quick / OpenGL on PCs
# without a GPU driver; this Qt Widgets app never uses OpenGL, so it is left out (and with it Mesa's and LLVM's terms).
a.binaries = [b for b in a.binaries if Path(b[0]).name.lower() != "opengl32sw.dll"]
# the licence texts: packaging/build.py writes dist/NX2Harmony/licenses after this run; upx=False keeps the Qt DLLs
# byte for byte as the PySide6 wheel has them (LGPL: a player can replace them, packaging/licenses/QT-SOURCES.txt)
pyz = PYZ(a.pure)
gui = EXE(pyz, a.scripts, [], exclude_binaries=True, name="NX2Harmony", console=False, icon=icon, upx=False)
cli = EXE(pyz, a.scripts, [], exclude_binaries=True, name="nx2h", console=True, icon=icon, upx=False)
coll = COLLECT(gui, cli, a.binaries, a.datas, name="NX2Harmony", upx=False)
