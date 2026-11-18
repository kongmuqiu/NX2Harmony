"""Double-click to open NX2Harmony (pythonw: no console window)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nx2h.app import main  # noqa: E402

sys.exit(main())
