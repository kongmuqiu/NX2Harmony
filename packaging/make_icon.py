"""packaging/NX2Harmony.ico from the app's own drawn icon (nx2h/app.py app_icon), at the sizes Windows uses."""
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402
from PySide6.QtCore import QBuffer, QIODevice  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from nx2h.app import app_icon  # noqa: E402

app = QApplication(sys.argv)
buf = QBuffer()
buf.open(QIODevice.WriteOnly)
app_icon().pixmap(256, 256).save(buf, "PNG")
img = Image.open(io.BytesIO(bytes(buf.data())))
out = ROOT / "packaging" / "NX2Harmony.ico"
img.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print(out)
