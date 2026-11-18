#!/usr/bin/env python3
"""One-click HarmonyOS builds of the Switch games this project has adapted (games.tsv), from the player's own files.

    nx2h setup [--fix]                  check this PC and the phone; --fix installs what winget can
    nx2h scan <folder>...               which games and versions the folders hold, and what each still needs
    nx2h build [<folder>...] [-k <folder with prod.keys>] [--game <title id>] [--no-install] [--progress json]
               [--screen <width>x<height>]   (the screen the game's UI is laid out for; default: the phone's)
                                        prepare, build the signed HAP, install it and send the game data

(`python nx2h.py ...` from a checkout, `nx2h.exe ...` next to the packed NX2Harmony.exe.) Exit codes as nxnative's
build_ipa_from_nsp.py: 0 done, 1 stopped on an error (the last [RED] line says why), 2 bad arguments, 130 interrupted.
Nothing the build makes from game files, keys or signing ever goes into the repository."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nx2h.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
