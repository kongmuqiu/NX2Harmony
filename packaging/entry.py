"""The packed app's one entry: nx2h.exe is the command line, NX2Harmony.exe the desktop app (both built from this file
by packaging/NX2Harmony.spec, sharing one _internal folder)."""
import sys
from pathlib import Path


def mistaken_for_python(args: list) -> bool:
    """Started the way a Python interpreter is (`-I`, `-c`, `-m`, a .py script): packed, sys.executable is this exe, so
    code that runs sys.executable as Python lands here. The desktop app then opens no window (each window checks the PC
    again, and a check that does the same would open the next one)."""
    return bool(args) and (args[0] in ("-I", "-c", "-m", "-u", "-E", "-s", "-S", "-B") or args[0].lower().endswith((".py", ".pyw")))


def main() -> int:
    if Path(sys.executable).stem.lower() == "nx2h":
        from nx2h.cli import main as cli
        return cli()
    if mistaken_for_python(sys.argv[1:]):
        return 2
    from nx2h.app import main as gui
    return gui()


if __name__ == "__main__":
    sys.exit(main())
