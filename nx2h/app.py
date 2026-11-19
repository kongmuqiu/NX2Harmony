"""NX2Harmony: the desktop app (Qt, PySide6). Three pages, as NX2Apple has on the Mac:

  游戏库  the games found in the library folder, with their own icons (icon.py) and their state in games.tsv
  构建    the files found, the phone, and a build: `nx2h.py build --progress json` as a child, its events drawn as stages
  环境    the setup checks (doctor.py) and "install what is missing"

Everything slow (checks, scanning, icons, the build) runs off the GUI thread. Keys and signing files are never opened
here; the build's child reads them as tool inputs. One window per app folder: a second start brings the first one to
the front and exits."""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque
from pathlib import Path

from PySide6.QtCore import (QByteArray, QLockFile, QObject, QPoint, QRect, QRectF, QRunnable, QSize, Qt, QThreadPool, QTimer,
                            QUrl, Signal)
from PySide6.QtGui import (QColor, QDesktopServices, QFont, QIcon, QImage, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap,
                           QTransform)
from PySide6.QtWidgets import (QApplication, QCheckBox, QFileDialog, QFrame, QHBoxLayout, QLabel, QLayout, QMainWindow, QMessageBox,
                               QPlainTextEdit, QProgressBar, QPushButton, QScrollArea, QSizePolicy, QStackedWidget,
                               QVBoxLayout, QWidget)

from . import config, core, doctor, signing

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
THEMES = {"0100F2C0115B6000": ("#3c6b4d", "#101614"), "0100F43008C44000": ("#3a5a8c", "#0f1220"),
          "0400486024DEC000": ("#3d6b34", "#10160e")}   # OOT GameProfile.ets accent #58B947, darkened

QSS = """
* { font-family: "Microsoft YaHei UI"; color: #ececf0; }
QMainWindow, #root, QScrollArea, #page { background: #0d0d0f; border: 0; }
#topbar { background: #0d0d0f; border-bottom: 1px solid #2a2a2f; }
#brand { font-size: 20px; font-weight: 700; } #brandSub { color: #6a6a72; font-size: 11px; }
#nav { background: transparent; border: 0; color: #9a9aa3; padding: 7px 16px; border-radius: 9px; font-size: 14px; }
#nav:checked { background: #1e1e22; color: #ececf0; }
#h1 { font-size: 28px; font-weight: 700; } #h2 { font-size: 15px; font-weight: 600; color: #9a9aa3; margin-top: 10px; }
#sub, #muted { color: #8a8a93; } #muted { font-size: 12px; }
#card { background: #17171a; border: 1px solid #2a2a2f; border-radius: 14px; }
#row { border-bottom: 1px solid #2a2a2f; } #rowLast { border: 0; }
#rowTitle { font-weight: 600; font-size: 13.5px; } #rowDetail { color: #9a9aa3; font-size: 12px; } #how { color: #f1b44c; font-size: 12px; }
#err { color: #ff6b6b; font-size: 12px; } #tail { color: #7a7a83; font-family: Consolas, "Cascadia Mono", "Microsoft YaHei UI"; font-size: 11.5px; }
QPushButton { background: #1e1e22; border: 1px solid #2a2a2f; border-radius: 10px; padding: 7px 14px; font-size: 13px; }
QPushButton:hover { border-color: #46464e; }
QPushButton:disabled { color: #5a5a62; }
QPushButton#primary { background: #f4ead7; color: #1a1712; border: 0; border-radius: 15px; padding: 7px 22px; min-height: 18px; font-weight: 700; }
QPushButton#primary:disabled { background: #4a463f; color: #8a857b; }
#badge { border-radius: 9px; padding: 2px 9px; font-size: 11.5px; background: #1e1e22; color: #9a9aa3; }
#badge[kind="ok"] { color: #4cd37e; background: #16301f; } #badge[kind="warn"] { color: #f1b44c; background: #33280f; }
#badge[kind="bad"] { color: #ff6b6b; background: #351717; } #badge[kind="opt"] { color: #6aa8ff; background: #172338; }
#gameCard { background: #17171a; border: 1px solid #2a2a2f; border-radius: 14px; }
#gameCard[sel="true"] { border-color: #6a6a74; }
#gameName { font-weight: 600; font-size: 13.5px; } #gameVer { font-weight: 700; }
#bannerOk { background: #13281b; color: #4cd37e; border-radius: 12px; padding: 12px 16px; }
#bannerBad { background: #2c1515; color: #ff6b6b; border-radius: 12px; padding: 12px 16px; }
QPlainTextEdit { background: #0a0a0c; border: 1px solid #2a2a2f; border-radius: 10px; color: #9a9aa3;
  font-family: Consolas, "Cascadia Mono"; font-size: 12px; padding: 8px; }
QProgressBar { background: #1e1e22; border: 0; border-radius: 3px; max-height: 6px; }
QProgressBar::chunk { background: #f4ead7; border-radius: 3px; }
QCheckBox { color: #9a9aa3; } QCheckBox::indicator { width: 16px; height: 16px; }
QScrollBar:vertical { background: transparent; width: 10px; } QScrollBar::handle:vertical { background: #2e2e34; border-radius: 5px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
"""


def sign_in_deveco(project: str) -> bool:
    """Whether a failed build offers to sign by hand in DevEco Studio: a signing project was made (the build's "signing"
    event) and DevEco Studio's program is there; without it only the build's own reason is shown."""
    return bool(project) and signing.deveco_exe().is_file()


# ---------- background work ----------
class _Sig(QObject):
    done = Signal(object)


class _Task(QRunnable):
    live = set()

    def __init__(self, fn, cb):
        super().__init__()
        self.setAutoDelete(False)
        self.fn, self.cb, self.sig = fn, cb, _Sig()
        self.sig.done.connect(self._deliver)
        _Task.live.add(self)   # until the result is delivered: a lambda's receiver lives on self.sig

    def _deliver(self, r):   # on the GUI thread
        try:
            self.cb(r)
        finally:
            _Task.live.discard(self)

    def run(self):
        try:
            r = self.fn()
        except Exception as e:   # shown, not raised: a slow step must not take the window down
            r = e
        self.sig.done.emit(r)


def run_bg(fn, cb) -> None:
    QThreadPool.globalInstance().start(_Task(fn, cb))


class _Relay(QObject):
    """The one-click install's progress (doctor.listen), from the thread it runs on to the 环境 page."""
    note = Signal(dict)


class BuildRunner(QObject):
    """`nx2h.py build --progress json` as a child without a console window (its own children inherit the hidden one)."""
    event = Signal(dict)
    finished = Signal(int)

    def __init__(self):
        super().__init__()
        self.proc = None
        self.awake = None   # the build's last "awake" event while it keeps the phone's screen on (cli.py, keep_awake)

    def running(self) -> bool:
        return bool(self.proc and self.proc.poll() is None)

    def start(self, args: list) -> None:
        cmd = config.cli()
        if not config.FROZEN:
            exe = cmd[0]
            if exe.lower().endswith("pythonw.exe"):
                exe = exe[:-5] + ".exe"   # python.exe: its stdout is the pipe
            cmd = [exe, "-u"] + cmd[1:]
        self.proc = subprocess.Popen(cmd + ["build", *args, "--progress", "json"],
                                     cwd=str(config.APP_DIR), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", env=dict(os.environ, PYTHONIOENCODING="utf-8"),
                                     creationflags=NO_WINDOW)
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()

    def _read(self, proc) -> None:
        self.awake = None
        for line in proc.stdout:
            line = line.strip()
            if line:
                try:
                    e = json.loads(line)
                except ValueError:
                    e = {"ev": "say", "msg": line}
                if isinstance(e, dict) and e.get("ev") == "awake":
                    self.awake = e if e.get("on") else None
                self.event.emit(e)
        self.finished.emit(proc.wait())

    def stop(self) -> None:
        if self.running():   # the whole tree: the build starts docker, powershell and python children
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True, creationflags=NO_WINDOW)
            awake, self.awake = self.awake, None
            if awake and awake.get("target"):
                # the killed build cannot put the phone's screen-off time back (keep_awake's finally): done here, off the
                # GUI thread; not a daemon thread, so a closing window still lets it finish (a few seconds)
                threading.Thread(target=self._restore_screen, args=(awake,), name="nx2h-restore-screen").start()

    @staticmethod
    def _restore_screen(awake: dict) -> None:
        from .device import Hdc
        try:
            h = Hdc()
            h.target = str(awake.get("target") or "")
            h.server = str(awake.get("server") or h.server)
            h.shell(str(awake.get("restore") or Hdc.restore_command(None)), timeout=15)
        except Exception:   # best effort: the next build's keep_awake knows a left-over override as its own
            pass


# ---------- small widgets ----------
_SEG = r"[^\s/\\·，；、：:\"'<>|]+"
_WINPATH = re.compile(rf"(?<![\w:/\\])(?:[A-Za-z]:/|//(?={_SEG}))(?:{_SEG})?(?:/{_SEG}| (?![A-Za-z]:){_SEG})*/?")


def native(text: str) -> str:
    """Paths in `text` with Windows' separators (D:\\x, \\\\server\\share), for display only: Qt's folder dialog and
    hand-written settings give D:/x and //server/share. URLs and relative paths are left alone."""
    return _WINPATH.sub(lambda m: m.group(0).replace("/", "\\"), text or "")


def label(text="", name="", wrap=False) -> QLabel:
    l = QLabel(text)
    if name:
        l.setObjectName(name)
    l.setWordWrap(wrap)
    if name in ("rowDetail", "err"):   # paths and errors can be copied
        l.setTextInteractionFlags(Qt.TextSelectableByMouse)
    return l


DOTS = {"ok": "#4cd37e", "bad": "#ff6b6b", "run": "#6aa8ff"}


def dot(state: str) -> QLabel:
    d = QLabel()
    d.setFixedSize(8, 8)
    paint_dot(d, state)
    return d


def paint_dot(d: QLabel, state: str) -> None:
    d.setStyleSheet(f"background:{DOTS.get(state, '#5a5a62')}; border-radius:4px;")


class Elided(QLabel):
    """One line, cut with … to the width it is given (a running tool's latest output line); hidden while empty."""
    def __init__(self, name: str = ""):
        super().__init__()
        if name:
            self.setObjectName(name)
        self.full = ""
        self.setTextFormat(Qt.PlainText)
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
        self.hide()

    def set(self, text: str) -> None:
        self.full = " ".join(str(text or "").split())
        self.setToolTip(self.full)
        self._fit()
        self.setVisible(bool(self.full))

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._fit()

    def _fit(self) -> None:
        self.setText(self.fontMetrics().elidedText(self.full, Qt.ElideRight, max(self.width() - 2, 40)))


def badge(g: dict) -> QLabel:
    st = g.get("status")
    if g.get("buildable"):   # 优化中 builds like 已验证 and says so (config.BUILDABLE)
        text, kind = ("优化中", "opt") if st == "优化中" else ("已适配 · 可构建", "ok")
    elif st in config.BUILDABLE:
        text, kind = ("已适配 · 缺文件" if st == "已验证" else f"{st} · 缺文件"), "warn"
    elif st == "适配中":
        text, kind = "适配中", "warn"
    else:
        text, kind = "未适配", ""
    b = label(text, "badge")
    b.setProperty("kind", kind)
    return b


def card() -> tuple:
    f = QFrame()
    f.setObjectName("card")
    v = QVBoxLayout(f)
    v.setContentsMargins(0, 0, 0, 0)
    v.setSpacing(0)
    return f, v


def row(state, title, detail="", right=None, how="", last=False, err="") -> QWidget:
    w = QWidget()
    w.setObjectName("rowLast" if last else "row")
    w.setAttribute(Qt.WA_StyledBackground, True)
    h = QHBoxLayout(w)
    h.setContentsMargins(18, 12, 18, 12)
    h.setSpacing(14)
    if state is not None:
        h.addWidget(dot(state), 0, Qt.AlignVCenter)
    v = QVBoxLayout()
    v.setSpacing(2)
    v.addWidget(label(title, "rowTitle", True))
    if detail:
        v.addWidget(label(detail, "rowDetail", True))
    if err:
        v.addWidget(label(err, "err", True))
    if how:
        v.addWidget(label("→ " + how, "how", True))
    h.addLayout(v, 1)
    if right is not None:
        h.addWidget(right, 0, Qt.AlignVCenter)
    return w


# a web address inside a check's hint (it ends at a space or Chinese punctuation)
WEB_LINK = re.compile(r"https?://[^\s，。；、：）)\"'<>]+")


def open_link_button(url: str, beside=None) -> QWidget:
    """「打开下载页」: opens url in the default browser; beside (a row's badge) stays to its left."""
    b = QPushButton("打开下载页")
    b.setToolTip(url)
    b.setCursor(Qt.PointingHandCursor)
    b.clicked.connect(lambda _=False, u=url: QDesktopServices.openUrl(QUrl(u)))
    if beside is None:
        return b
    w = QWidget()
    h = QHBoxLayout(w)
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(8)
    h.addWidget(beside, 0, Qt.AlignVCenter)
    h.addWidget(b, 0, Qt.AlignVCenter)
    return w


def clear(layout) -> None:
    while layout.count():
        it = layout.takeAt(0)
        if it.widget():
            it.widget().deleteLater()
        elif it.layout():
            clear(it.layout())


def rounded(pix: QPixmap, size: int, radius: int) -> QPixmap:
    out = QPixmap(size, size)
    out.fill(Qt.transparent)
    p = QPainter(out)
    p.setRenderHint(QPainter.Antialiasing)
    path = QPainterPath()
    path.addRoundedRect(QRectF(0, 0, size, size), radius, radius)
    p.setClipPath(path)
    p.drawPixmap(0, 0, pix.scaled(size, size, Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation))
    p.end()
    return out


class FlowLayout(QLayout):
    """Cards left to right, wrapping (Qt's flow layout example)."""
    def __init__(self, spacing=14):
        super().__init__()
        self.items = []
        self.space = spacing

    def addItem(self, it): self.items.append(it)
    def count(self): return len(self.items)
    def itemAt(self, i): return self.items[i] if 0 <= i < len(self.items) else None
    def takeAt(self, i): return self.items.pop(i) if 0 <= i < len(self.items) else None
    def expandingDirections(self): return Qt.Orientations(0)
    def hasHeightForWidth(self): return True
    def heightForWidth(self, w): return self._do(QRect(0, 0, w, 0), True)
    def setGeometry(self, r): super().setGeometry(r); self._do(r, False)
    def sizeHint(self): return self.minimumSize()

    def minimumSize(self):
        s = QSize()
        for it in self.items:
            s = s.expandedTo(it.minimumSize())
        return s

    def _do(self, r, test):
        x, y, line = r.x(), r.y(), 0
        for it in self.items:
            hint = it.sizeHint()
            if x + hint.width() > r.right() + 1 and line > 0:
                x, y, line = r.x(), y + line + self.space, 0
            if not test:
                it.setGeometry(QRect(QPoint(x, y), hint))
            x += hint.width() + self.space
            line = max(line, hint.height())
        return y + line - r.y()


class Hero(QWidget):
    """The selected game: its icon over a blurred copy of itself (a small scaled-down image drawn large)."""
    def __init__(self):
        super().__init__()
        self.setMinimumHeight(230)
        self.bg = None
        self.colors = ("#26262c", "#121215")

    def set_game(self, g) -> None:
        self.colors = THEMES.get(g["title_id"], ("#26262c", "#121215")) if g else ("#26262c", "#121215")
        self.bg = None
        if g and g.get("icon"):
            img = QImage(g["icon"])
            if not img.isNull():   # 12 px wide and back: a blur without any filter
                small = img.scaled(10, 10, Qt.IgnoreAspectRatio, Qt.SmoothTransformation)
                self.bg = QPixmap.fromImage(small.scaled(160, 160, Qt.IgnoreAspectRatio, Qt.SmoothTransformation))
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setRenderHint(QPainter.SmoothPixmapTransform)
        path = QPainterPath()
        path.addRoundedRect(QRectF(self.rect()), 18, 18)
        p.setClipPath(path)
        if self.bg:
            side = max(self.width(), self.height()) + 120
            p.drawPixmap(QRect((self.width() - side) // 2, (self.height() - side) // 2, side, side), self.bg)
        else:
            g = QLinearGradient(0, 0, self.width(), self.height())
            g.setColorAt(0, QColor(self.colors[0]))
            g.setColorAt(1, QColor(self.colors[1]))
            p.fillRect(self.rect(), g)
        shade = QLinearGradient(0, 0, self.width(), 0)
        shade.setColorAt(0, QColor(10, 10, 12, 225))
        shade.setColorAt(1, QColor(10, 10, 12, 90))
        p.fillRect(self.rect(), shade)


class PhoneBadge(QWidget):
    """The phone's state in the top bar, drawn (no image file): connected, a phone turning about its upright axis in
    perspective, its back (camera module) showing half the turn, with a green light; not connected, still and grey with a
    red one. Only this small widget repaints, about 30 times a second while it turns."""
    CREAM, DIM = QColor("#f4ead7"), QColor("#5a5a62")

    def __init__(self, size: int = 34):
        super().__init__()
        self.setFixedSize(size, size)
        self.connected = False
        self.angle = 0.0
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)

    def set_connected(self, on: bool) -> None:
        self.connected = on
        if on and not self.timer.isActive():
            self.timer.start(33)
        elif not on:
            self.timer.stop()
            self.angle = 0.0
        self.update()

    TURN, REST = 2.4, 2.6   # seconds: one eased turn, then facing front

    def _tick(self) -> None:
        self.t = (getattr(self, "t", 0.0) + 0.033) % (self.TURN + self.REST)
        x = min(self.t / self.TURN, 1.0)
        ease = 4 * x ** 3 if x < 0.5 else 1 - (-2 * x + 2) ** 3 / 2   # cubic in-out
        self.angle = 360.0 * ease % 360.0
        self.update()

    def paintEvent(self, _):
        import math
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w / 2, h / 2 - 1
        bw, bh = h * 0.44, h * 0.78   # the phone's body
        rad = math.radians(self.angle)
        front = math.cos(rad) >= 0
        # a soft shadow that narrows as the phone turns edge-on
        sw = max(bw * abs(math.cos(rad)) + 4, 6)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(0, 0, 0, 90))
        p.drawEllipse(QRectF(cx - sw / 2, cy + bh / 2 + 0.5, sw, 3))
        t = QTransform()
        t.translate(cx, cy)
        t.rotate(self.angle, Qt.YAxis, 70)   # a short distance to the plane: visible perspective at this size
        t.translate(-cx, -cy)
        p.setTransform(t)
        body = QRectF(cx - bw / 2, cy - bh / 2, bw, bh)
        edge = self.CREAM if self.connected else self.DIM
        pen = QPen(edge, 1.2)
        p.setPen(pen)
        if front:
            p.setBrush(QColor("#1a1a1e"))
            p.drawRoundedRect(body, bw * 0.22, bw * 0.22)
            screen = body.adjusted(2.2, 2.6, -2.2, -2.6)
            g = QLinearGradient(screen.topLeft(), screen.bottomRight())
            if self.connected:
                g.setColorAt(0, QColor("#3c6b4d"))
                g.setColorAt(0.55, QColor("#2c4f6e"))
                g.setColorAt(1, QColor("#1b2238"))
            else:
                g.setColorAt(0, QColor("#2a2a30"))
                g.setColorAt(1, QColor("#1c1c20"))
            p.setPen(Qt.NoPen)
            p.setBrush(g)
            p.drawRoundedRect(screen, bw * 0.12, bw * 0.12)
            p.setBrush(QColor("#0d0d0f"))   # the front camera
            p.drawEllipse(QRectF(cx - 1.1, screen.top() + 1.0, 2.2, 2.2))
        else:
            p.setBrush(QColor("#25252b"))
            p.drawRoundedRect(body, bw * 0.22, bw * 0.22)
            d = bw * 0.62   # a round camera ring in the upper middle, as on Huawei's phones
            ring = QRectF(cx - d / 2, body.top() + bh * 0.12, d, d)
            p.setBrush(QColor("#34343c"))
            p.setPen(QPen(edge, 0.9))
            p.drawEllipse(ring)
            p.setPen(Qt.NoPen)
            p.setBrush(QColor("#0d0d0f"))
            r = d * 0.15
            for dx, dy in ((0.5, 0.3), (0.3, 0.64), (0.7, 0.64)):
                p.drawEllipse(QRectF(ring.left() + d * dx - r, ring.top() + d * dy - r, 2 * r, 2 * r))
        # light across the body as it turns
        shine = QLinearGradient(body.topLeft(), body.topRight())
        a = int(70 * max(0.0, math.sin(rad)) ** 2)
        shine.setColorAt(0, QColor(255, 255, 255, a))
        shine.setColorAt(1, QColor(255, 255, 255, 0))
        p.setBrush(shine)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(body, bw * 0.22, bw * 0.22)
        p.resetTransform()
        # the state light
        p.setBrush(QColor("#4cd37e") if self.connected else QColor("#ff6b6b"))
        p.setPen(QPen(QColor("#0d0d0f"), 1.5))
        p.drawEllipse(QRectF(cx + bw / 2 - 2.5, cy + bh / 2 - 5.5, 6.5, 6.5))


class GameCard(QFrame):
    clicked = Signal(dict)

    def __init__(self, g: dict, selected: bool):
        super().__init__()
        self.g = g
        self.setObjectName("gameCard")
        self.setProperty("sel", "true" if selected else "false")
        self.setFixedWidth(206)
        self.setCursor(Qt.PointingHandCursor)
        v = QVBoxLayout(self)
        v.setContentsMargins(14, 14, 14, 14)
        v.setSpacing(6)
        cover = QLabel()
        cover.setFixedSize(178, 178)
        if g.get("icon"):
            cover.setPixmap(rounded(QPixmap(g["icon"]), 178, 12))
        else:
            c0, c1 = THEMES.get(g["title_id"], ("#2a2a30", "#18181c"))
            cover.setStyleSheet(f"background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 {c0}, stop:1 {c1}); border-radius: 12px;"
                                "color: #d8dde0; font-weight: 700; padding: 10px;")
            cover.setAlignment(Qt.AlignLeft | Qt.AlignBottom)
            cover.setText(("正在取图标…\n\n" if g.get("icon_pending") else "") + g["name"])
            cover.setWordWrap(True)
        v.addWidget(cover)
        v.addWidget(label(g["name"], "gameName"))
        h = QHBoxLayout()
        h.addWidget(label(g["version"] or "—", "gameVer"))
        h.addStretch(1)
        h.addWidget(badge(g))
        v.addLayout(h)
        v.addWidget(label(f'{g["title_id"]} · {len(g["files"])} 个文件', "muted"))

    def mousePressEvent(self, e):
        self.clicked.emit(self.g)


# ---------- pages ----------
def page() -> tuple:
    sc = QScrollArea()
    sc.setWidgetResizable(True)
    inner = QWidget()
    inner.setObjectName("page")
    outer = QHBoxLayout(inner)
    outer.setContentsMargins(0, 0, 0, 0)
    body = QWidget()
    body.setMaximumWidth(980)
    v = QVBoxLayout(body)
    v.setContentsMargins(28, 24, 28, 40)
    v.setSpacing(12)
    outer.addWidget(body)
    sc.setWidget(inner)
    return sc, v


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("NX2Harmony")
        self.resize(1180, 840)
        self.games, self.selected, self.checks = [], None, []
        self.runner = BuildRunner()
        self.runner.event.connect(self.on_event)
        self.runner.finished.connect(self.on_finished)
        self.stages = []
        self.last_detail = ""   # the build's latest tool output line ("detail" events)
        self.fixing = False   # a one-click install is running
        self.fix_state, self.fix_why = {}, {}   # check label → run / ok / bad / skip, and why it failed or was skipped
        self.fix_lines = deque(maxlen=4)   # (line, redrawn in place) of the running install tool
        self.relay = _Relay()
        self.relay.note.connect(self.on_fix_note)

        root = QWidget()
        root.setObjectName("root")
        rv = QVBoxLayout(root)
        rv.setContentsMargins(0, 0, 0, 0)
        rv.setSpacing(0)
        top = QWidget()
        top.setObjectName("topbar")
        top.setAttribute(Qt.WA_StyledBackground, True)
        th = QHBoxLayout(top)
        th.setContentsMargins(28, 12, 28, 12)
        th.setSpacing(28)
        bv = QVBoxLayout()
        bv.setSpacing(0)
        bv.addWidget(label("NX2Harmony", "brand"))
        bv.addWidget(label("by nxnative-harmonyos", "brandSub"))
        th.addLayout(bv)
        self.navs = {}
        nh = QHBoxLayout()
        nh.setSpacing(6)
        for key, text in (("library", "游戏库"), ("build", "构建"), ("setup", "环境")):
            b = QPushButton(text)
            b.setObjectName("nav")
            b.setCheckable(True)
            b.clicked.connect(lambda _=False, k=key: self.show_page(k))
            self.navs[key] = b
            nh.addWidget(b)
        th.addLayout(nh)
        th.addStretch(1)
        self.phone_badge = PhoneBadge()
        self.phone = label("手机未连接", "muted")
        ph = QHBoxLayout()
        ph.setSpacing(8)
        ph.addWidget(self.phone_badge)
        ph.addWidget(self.phone)
        th.addLayout(ph)
        rv.addWidget(top)
        self.stack = QStackedWidget()
        rv.addWidget(self.stack, 1)
        self.setCentralWidget(root)

        self.pages = {}
        self._library_page()
        self._build_page()
        self._setup_page()
        self.show_page("library")
        geo = config.local().get("window")
        if geo:
            self.restoreGeometry(QByteArray.fromBase64(geo.encode()))
        self.refresh_state()
        self.load_games()
        self.load_checks(False, first=True)
        self.hap_out = ""
        self._phone_busy = False
        self.phone_timer = QTimer(self)   # the phone, plugged in or out while the app is open
        self.phone_timer.timeout.connect(self.poll_phone)
        self.phone_timer.start(5000)

    def poll_phone(self):
        if self._phone_busy or not self.checks:
            return
        self._phone_busy = True
        run_bg(core.phone_status, self.on_phone)

    def on_phone(self, st):
        self._phone_busy = False
        if isinstance(st, Exception):
            return
        ok, detail = st
        ph = next((c for c in self.checks if c["key"] == "phone"), None)
        if ph is None or (bool(ph["ok"]) == ok and (not ok or ph["detail"] == detail)):
            return
        was = bool(ph["ok"])
        ph.update(ok=ok, detail=detail if ok else "没有连接的手机", how="" if ok else ph.get("how", ""))
        self.render_device()
        if ok and not was:
            self.install.setChecked(True)

    def closeEvent(self, e):
        if self.runner.running():
            r = QMessageBox.question(self, "构建还在进行", "关闭窗口会停止正在进行的构建（已完成的步骤下次会跳过）。确定关闭吗？")
            if r != QMessageBox.Yes:
                e.ignore()
                return
            self.runner.stop()
        config.save_local(window=bytes(self.saveGeometry().toBase64()).decode())
        e.accept()

    def show_page(self, key):
        for k, b in self.navs.items():
            b.setChecked(k == key)
        self.stack.setCurrentWidget(self.pages[key])

    # ----- 游戏库 -----
    def _library_page(self):
        sc, v = page()
        self.hero = Hero()
        hv = QHBoxLayout(self.hero)
        hv.setContentsMargins(34, 28, 34, 28)
        hv.setSpacing(24)
        self.hero_icon = QLabel()
        self.hero_icon.setFixedSize(132, 132)
        hv.addWidget(self.hero_icon, 0, Qt.AlignBottom)
        tv = QVBoxLayout()
        tv.addStretch(1)
        self.hero_meta = label("还没有选择游戏", "sub")
        self.hero_title = label("把你的游戏放进游戏库", "h1")
        self.hero_title.setStyleSheet("font-size: 32px; font-weight: 700;")
        tv.addWidget(self.hero_meta)
        tv.addWidget(self.hero_title)
        bh = QHBoxLayout()
        self.hero_build = QPushButton("构建游戏")
        self.hero_build.setObjectName("primary")
        self.hero_build.clicked.connect(lambda: self.show_page("build"))
        self.hero_note = label("", "muted")
        self.hero_web = QPushButton("从网上取图标")
        self.hero_web.setToolTip("游戏文件里没有图标时，按 title ID 下载它在 eShop 上的图标（只在点这里时联网）")
        self.hero_web.clicked.connect(self.fetch_icon)
        bh.addWidget(self.hero_build)
        bh.addWidget(self.hero_web)
        bh.addWidget(self.hero_note, 1)
        tv.addLayout(bh)
        hv.addLayout(tv, 1)
        v.addWidget(self.hero)

        f, fv = card()
        self.lib_btn = QPushButton("更换…")
        self.lib_btn.clicked.connect(lambda: self.choose("library"))
        self.keys_btn = QPushButton("选择…")
        self.keys_btn.clicked.connect(lambda: self.choose("keys"))
        self.folders = fv
        v.addWidget(f)
        gh = QHBoxLayout()
        gh.addWidget(label("你的游戏", "h2"))
        gh.addStretch(1)
        refresh = QPushButton("刷新")
        refresh.setToolTip("重新扫描游戏文件夹（放进新游戏后用）")
        refresh.clicked.connect(self.rescan)
        gh.addWidget(refresh)
        v.addLayout(gh)
        grid = QWidget()
        self.flow = FlowLayout()
        grid.setLayout(self.flow)
        v.addWidget(grid)
        self.empty = label("选择放游戏文件的文件夹后，会扫描里面所有子文件夹的 .nsp / .nsz / .xci / .xcz / .nup，一个文件夹里可以放多个游戏。", "muted", True)
        v.addWidget(self.empty)
        v.addStretch(1)
        self.pages["library"] = sc
        self.stack.addWidget(sc)

    def refresh_state(self):
        self.lib_btn.setParent(None)   # kept across redraws: out of the rows clear() deletes
        self.keys_btn.setParent(None)
        clear(self.folders)
        lib = config.local().get("library", "")
        keys = core.keys_folder()
        self.folders.addWidget(row("ok" if lib else "", "游戏文件夹", native(lib) or "未选择", self.lib_btn))
        self.folders.addWidget(row("ok" if keys else "", "主机密钥 prod.keys", native(keys) or "未找到（游戏数据已准备好时可以不选；取游戏图标需要它）",
                                   self.keys_btn, last=True))

    def choose(self, kind):
        start = config.local().get(kind, "") or str(Path.home())
        title = "选择放游戏文件的文件夹（里面的子文件夹都会扫描，可以放多个游戏）" if kind == "library" else "选择 prod.keys 所在的文件夹（你自己的 Switch 导出的）"
        d = QFileDialog.getExistingDirectory(self, title, start)
        if not d:
            return
        if not core.set_folder(kind, d):
            self.hero_note.setText("这个文件夹里没有 prod.keys")
            return
        self.refresh_state()
        self.load_games()

    def fetch_icon(self):
        g = self.selected
        if not g:
            return
        from . import icon
        self.hero_web.setEnabled(False)
        self.hero_note.setText("正在下载图标…")

        def done(ok):
            self.hero_web.setEnabled(True)
            if ok is True:
                self.load_games()
            else:
                self.hero_note.setText("没下载到图标（网络不通，或者这个 title ID 没有 eShop 图标）")
        run_bg(lambda: icon.fetch_online(g["title_id"]), done)

    def rescan(self):
        from . import icon
        icon.forget_tried()
        self.load_games()

    def load_games(self):
        run_bg(lambda: core.library_games(config.local().get("library", "")), self.on_games)

    def on_games(self, games):
        if isinstance(games, Exception):
            self.hero_note.setText(native(f"读取游戏库出错：{games}"))
            return
        self.games = games
        keep = self.selected and next((g for g in games if g["title_id"] == self.selected["title_id"]), None)
        self.select(keep or next((g for g in games if g["buildable"]), None) or (games[0] if games else None))
        if any(g["icon_pending"] for g in games):
            QTimer.singleShot(1500, self.load_games)

    def render_cards(self):
        clear(self.flow)
        for g in self.games:
            c = GameCard(g, bool(self.selected and g["title_id"] == self.selected["title_id"]))
            c.clicked.connect(self.select)
            self.flow.addWidget(c)
        self.empty.setVisible(not self.games)

    def select(self, g):
        self.selected = g
        self.render_cards()
        self.hero.set_game(g)
        if g and g.get("icon"):
            self.hero_icon.setPixmap(rounded(QPixmap(g["icon"]), 132, 24))
            self.hero_icon.show()
        else:
            self.hero_icon.hide()
        self.hero_title.setText(g["name"] if g else "把你的游戏放进游戏库")
        self.hero_meta.setText((f'版本 {g["version"]} · ' if g and g["version"] else "") + (g["title_id"] if g else "还没有选择游戏"))
        self.hero_build.setEnabled(bool(g and g["buildable"]))
        self.hero_web.setVisible(bool(g and not g.get("icon") and not g.get("icon_pending")))
        if not g:
            note = ("先点下面「游戏文件夹」的「更换…」，选放游戏文件的文件夹" if not config.local().get("library")
                    else "这个文件夹里没找到游戏文件（.nsp / .nsz / .xci / .xcz / .nup）")
        elif g["buildable"]:
            note = "游戏数据已准备好，构建只需重新打包" if g["prepared"] else "首次构建需要较长时间（解包游戏、改写代码、翻译着色器）"
            if g["status"] == "优化中":
                note += "（优化中：能玩，帧率和个别画面还在调）"
        elif g["status"] == "适配中":
            note = "这个游戏还在适配，暂时不能一键构建"
        elif g["status"] in config.BUILDABLE:
            note = "；".join(g["problems"])
        else:
            note = "没有登记的游戏：需要先适配"
        self.hero_note.setText(note)
        self.render_build_files()

    # ----- 构建 -----
    def _build_page(self):
        sc, v = page()
        v.addWidget(label("构建游戏", "h1"))
        v.addWidget(label("识别到的文件、运行设备和构建进度。", "sub"))
        f, self.bfiles = card()
        v.addWidget(f)
        v.addWidget(label("在哪里玩", "h2"))
        f, self.bdev = card()
        self.install = QCheckBox("构建完装到手机并推送游戏数据")
        self.install.setChecked(True)
        v.addWidget(f)
        h = QHBoxLayout()
        self.bhint = label("", "muted")
        h.addWidget(self.bhint, 1)
        self.stop_btn = QPushButton("停止")
        self.stop_btn.clicked.connect(self.runner.stop)
        self.stop_btn.hide()
        self.start_btn = QPushButton("开始构建")
        self.start_btn.setObjectName("primary")
        self.start_btn.clicked.connect(self.start_build)
        h.addWidget(self.stop_btn)
        h.addWidget(self.start_btn)
        v.addSpacing(8)
        v.addLayout(h)
        self.banner = label("", "bannerOk", True)
        self.banner.hide()
        v.addWidget(self.banner)
        ah = QHBoxLayout()
        self.open_hap_btn = QPushButton("打开 HAP 所在文件夹")
        self.open_hap_btn.clicked.connect(lambda: self.open_folder(Path(self.hap_out).parent))
        self.open_log_btn = QPushButton("打开日志文件夹")
        self.open_log_btn.clicked.connect(lambda: self.open_folder(config.LOGS))
        self.sign_project = ""
        self.sign_btn = QPushButton("打开 DevEco Studio 签名")
        self.sign_btn.clicked.connect(lambda: signing.open_in_deveco(Path(self.sign_project)))
        for b in (self.sign_btn, self.open_hap_btn, self.open_log_btn):
            b.hide()
            ah.addWidget(b)
        ah.addStretch(1)
        v.addLayout(ah)
        v.addWidget(label("构建进度", "h2"))
        f, self.bstages = card()
        v.addWidget(f)
        self.cur_step = label("", "muted")
        self.cur_note = label("", "muted")
        ch = QHBoxLayout()
        ch.addWidget(self.cur_step, 1)
        ch.addWidget(self.cur_note)
        v.addLayout(ch)
        self.cur_detail = Elided("tail")   # under the current step: its tool's latest output line
        v.addWidget(self.cur_detail)
        self.bar = QProgressBar()
        self.bar.setTextVisible(False)
        self.bar.hide()
        v.addWidget(self.bar)
        v.addWidget(label("构建日志", "h2"))
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMinimumHeight(220)
        v.addWidget(self.log)
        v.addStretch(1)
        self.pages["build"] = sc
        self.stack.addWidget(sc)
        self.render_stages()

    def render_build_files(self):
        clear(self.bfiles)
        g = self.selected
        if not g:
            self.bfiles.addWidget(row(None, "先在游戏库里选一个游戏", last=True))
            self.start_btn.setEnabled(False)
            return
        self.bfiles.addWidget(row(None, f'{g["name"]} {g["version"]}', g["needs"], badge(g)))
        names = {"base": "本体", "update": "更新", "dlc": "追加内容"}
        items = [row("ok", "、".join(f"{names.get(r, r)} {v or ''}" for _, v, r in f["titles"]) or "未识别", f["name"], label(f["kind"].upper(), "badge"))
                 for f in g["files"]] + [row("bad", p) for p in g["problems"]]
        for i, w in enumerate(items):
            if i == len(items) - 1:
                w.setObjectName("rowLast")
            self.bfiles.addWidget(w)
        self.start_btn.setEnabled(bool(g["buildable"]) and not self.runner.running())
        self.bhint.setText("" if g["buildable"] else "这个游戏现在不能一键构建")

    def render_device(self):
        self.install.setParent(None)
        clear(self.bdev)
        ph = next((c for c in self.checks if c["key"] == "phone"), None)
        ok = bool(ph and ph["ok"])
        self.bdev.addWidget(row("ok" if ok else "bad", "鸿蒙手机",
                                ph["detail"] if ok else "没有连接的手机：打开开发者模式和 USB 调试后连上电脑（也可以只出 HAP）",
                                self.install, last=True))
        if not ok:
            self.install.setChecked(False)
        self.phone.setText(ph["detail"] if ok else "手机未连接")
        self.phone_badge.set_connected(ok)

    def render_stages(self):
        clear(self.bstages)
        if not self.stages:
            self.bstages.addWidget(row(None, "还没有开始", last=True))
            return
        for i, s in enumerate(self.stages):
            t = label(f'{s["elapsed"]:.0f} 秒' if s.get("elapsed") is not None else "", "muted")
            self.bstages.addWidget(row(s["state"], s["name"], native(s.get("detail", "")), t, last=i == len(self.stages) - 1))

    def start_build(self):
        g = self.selected
        if not g or self.runner.running():
            return
        self.stages, self.hap_out, self.sign_project = [], "", ""
        self.show_detail("")
        self.log.clear()
        self.banner.hide()
        self.sign_btn.hide()
        self.open_hap_btn.hide()
        self.open_log_btn.hide()
        self.render_stages()
        args = [config.local().get("library", ""), "--game", g["title_id"]]
        if not self.install.isChecked():
            args.append("--no-install")
        keys = core.keys_folder()
        if keys:
            args += ["-k", keys]
        self.runner.start(args)
        self.start_btn.setEnabled(False)
        self.stop_btn.show()

    def show_detail(self, text: str) -> None:
        self.last_detail = " ".join(str(text or "").split())
        self.cur_detail.set(self.last_detail)

    def on_event(self, e):
        ev = e.get("ev") or e.get("event")
        if ev == "detail":   # the running tool's latest output line (common._run, about two a second)
            if (e.get("text") or "").strip():
                self.show_detail(e["text"])
            return
        if ev in ("stage", "step"):   # the next step: the last one's output line no longer applies
            self.show_detail("")
        if ev == "stage":
            self.stages.append({"name": e["name"], "detail": e.get("step", ""), "state": "run"})
            self.cur_step.setText(e["name"])
            self.bar.setRange(0, 0)
            self.bar.show()
        elif ev == "step":
            if self.stages:
                self.stages[-1]["detail"] = e["step"]
            self.cur_step.setText(e["step"])
            self.log.appendPlainText("· " + e["step"])
        elif ev == "progress":
            if e.get("frac") is not None:
                self.bar.setRange(0, 1000)
                self.bar.setValue(int(e["frac"] * 1000))
            if e.get("unit") == "B" and e.get("total"):
                eta = f' · 约 {int(e["eta"] // 60) + 1} 分钟' if e.get("eta") else ""
                self.cur_note.setText(f'{e["done"] / 2**30:.2f} / {e["total"] / 2**30:.2f} GB{eta}')
            else:
                self.cur_note.setText((e.get("note") or "")[-80:])
        elif ev == "stage_end":
            k = e.get("k", 0) - 1
            if 0 <= k < len(self.stages):
                self.stages[k].update(state="ok" if e.get("ok") else "bad", detail=e.get("summary", ""), elapsed=e.get("elapsed"))
        elif ev == "say":
            self.log.appendPlainText(e.get("msg", ""))
        elif ev == "game":
            self.hap_out = e.get("out") or ""
            self.log.appendPlainText(f'游戏：{e.get("name")} {e.get("version")} · 包名 {e.get("bundle_id")} → {native(e.get("out") or "")}')
        elif ev == "signing":
            self.sign_project = e.get("project") or ""
        self.render_stages()

    def on_finished(self, code):
        if code != 0 and self.last_detail:   # often the failing tool's own error line
            self.log.appendPlainText("最后一行工具输出：" + self.last_detail)
        self.show_detail("")
        self.bar.hide()
        self.cur_step.setText("")
        self.cur_note.setText("")
        self.stop_btn.hide()
        self.banner.setObjectName("bannerOk" if code == 0 else "bannerBad")
        self.banner.style().unpolish(self.banner)   # restyle for the new object name
        self.banner.style().polish(self.banner)
        sign_by_hand = sign_in_deveco(self.sign_project)
        self.banner.setText(("完成：已构建" + ("、装到手机并打开。" if self.install.isChecked() else "。"))
                            if code == 0 else
                            ("这个游戏的包名还没签名。DevEco Studio 打开签名工程后：登录华为账号 → File → Project Structure → "
                             "Signing Configs → 勾选 Automatically generate signature → OK（手机连着时签），然后重新开始构建。")
                            if sign_by_hand else "构建停下了，原因见构建日志里 [RED] 开头的那一行，详细输出在日志文件夹。")
        self.banner.show()
        self.sign_btn.setVisible(code != 0 and sign_by_hand)
        self.open_hap_btn.setVisible(code == 0 and bool(self.hap_out) and Path(self.hap_out).is_file())
        self.open_log_btn.show()
        self.load_games()

    @staticmethod
    def open_folder(path: Path) -> None:
        if Path(path).is_dir():
            os.startfile(str(path))   # Explorer, a window of its own (no console)

    # ----- 环境 -----
    FIX_MARKS = {"run": ("正在安装", "opt"), "ok": ("已装好", "ok"), "bad": ("没装好", "bad"), "skip": ("已跳过", ""),
                 "wait": ("等重启电脑", "warn")}
    START_MARKS = {"run": "正在启动", "ok": "已启动", "bad": "没启动起来"}   # a fix that starts a program (Docker Desktop)

    def verb_of(self, name: str) -> str:
        """What the fix of the check called `name` does: 安装 (install) or 启动 (start)."""
        return next((c.get("verb") or "安装" for c in self.checks if c["label"] == name), "安装")

    def fix_mark(self, st: str, name: str):
        text, kind = self.FIX_MARKS[st]
        return (self.START_MARKS.get(st, text) if self.verb_of(name) == "启动" else text), kind

    def _setup_page(self):
        sc, v = page()
        self.setup_scroll = sc
        v.addWidget(label("准备好你的电脑", "h1"))
        v.addWidget(label("能自动装的会用 winget 装好；DevEco Studio、签名和手机的开发者模式需要你自己操作，下面写了怎么做。", "sub", True))
        f, self.checks_box = card()
        v.addWidget(f)
        h = QHBoxLayout()
        self.checks_sum = label("", "muted")
        h.addWidget(self.checks_sum, 1)
        self.re_btn = QPushButton("重新检查")
        self.re_btn.clicked.connect(lambda: self.load_checks(False))
        self.fix_btn = QPushButton("一键安装缺少的")
        self.fix_btn.setObjectName("primary")
        self.fix_btn.clicked.connect(lambda: self.load_checks(True))
        h.addWidget(self.re_btn)
        h.addWidget(self.fix_btn)
        v.addLayout(h)
        # the one-click install: how each item went, then the running one with its tool's last lines
        self.fix_head = label("安装进度", "h2")
        self.fix_card, fv = card()
        self.fix_rows = QVBoxLayout()
        self.fix_rows.setSpacing(0)
        fv.addLayout(self.fix_rows)
        self.fix_cur = QWidget()
        self.fix_cur.setObjectName("rowLast")
        self.fix_cur.setAttribute(Qt.WA_StyledBackground, True)
        ch = QHBoxLayout(self.fix_cur)
        ch.setContentsMargins(18, 12, 18, 12)
        ch.setSpacing(14)
        self.fix_dot = dot("run")
        ch.addWidget(self.fix_dot, 0, Qt.AlignVCenter)
        cv = QVBoxLayout()
        cv.setSpacing(2)
        self.fix_title = label("", "rowTitle", True)
        cv.addWidget(self.fix_title)
        self.fix_tail = [Elided("tail") for _ in range(self.fix_lines.maxlen)]
        for t in self.fix_tail:
            cv.addWidget(t)
        ch.addLayout(cv, 1)
        fv.addWidget(self.fix_cur)
        for w in (self.fix_head, self.fix_card):
            w.hide()
            v.addWidget(w)
        v.addStretch(1)
        self.pages["setup"] = sc
        self.stack.addWidget(sc)

    def load_checks(self, fix, first=False):
        self.fix_btn.setEnabled(False)
        self.re_btn.setEnabled(False)
        self.fix_state, self.fix_why = {}, {}
        if not fix:   # a check again: the last install's report is out of date
            self.fix_head.hide()
            self.fix_card.hide()
            clear(self.checks_box)
            self.checks_box.addWidget(row(None, "检查中…", last=True))
            run_bg(lambda: core.checks_json(False), lambda cs: self.on_checks(cs, first))
            return
        self.fixing = True   # the rows stay: the one being installed says so
        self.fix_lines.clear()
        clear(self.fix_rows)
        self.fix_now("正在检查要装哪些…", "run")
        QTimer.singleShot(100, lambda: self.setup_scroll.ensureWidgetVisible(self.fix_card))
        report = self.relay.note.emit

        def work():
            doctor.listen(report)
            try:
                return core.checks_json(True)
            finally:
                doctor.listen(None)
        run_bg(work, lambda cs: self.on_checks(cs, first))

    def fix_now(self, title: str, state: str) -> None:
        self.fix_title.setText(title)
        paint_dot(self.fix_dot, state)
        lines = [t for t, _ in self.fix_lines]
        for i, w in enumerate(self.fix_tail):
            w.set(lines[i] if i < len(lines) else "")
        self.fix_head.show()
        self.fix_card.show()

    def on_fix_note(self, n):
        kind = n.get("kind")
        if not self.fixing:   # a line from a tool that outlived its install: the summary stays
            return
        if kind in ("line", "progress"):   # a progress bar redraws its own line
            text = native(n.get("text", ""))
            if kind == "progress" and self.fix_lines and self.fix_lines[-1][1]:
                self.fix_lines[-1] = (text, True)
            else:
                self.fix_lines.append((text, kind == "progress"))
            self.fix_now(self.fix_title.text(), "run")
            return
        if kind == "item":
            self.fix_state[n["label"]] = "run"
            self.fix_lines.clear()
            self.fix_now(f'正在{self.verb_of(n["label"])}：{n["label"]}（{n["n"]}/{n["of"]}）', "run")
        elif kind in ("ok", "fail", "skip", "wait"):
            self.fix_state[n["label"]] = {"ok": "ok", "fail": "bad", "skip": "skip", "wait": "wait"}[kind]
            if n.get("why"):
                self.fix_why[n["label"]] = n["why"]
            self.render_fix_rows()
        elif kind == "recheck":
            self.fix_lines.clear()
            self.fix_now("装完了，正在重新检查…", "run")
        self.render_checks()

    def render_fix_rows(self):
        clear(self.fix_rows)
        for name, st in self.fix_state.items():
            if st == "run":
                continue
            text, kind = self.fix_mark(st, name)
            b = label(text, "badge")
            b.setProperty("kind", kind)
            self.fix_rows.addWidget(row("" if st in ("skip", "wait") else st, name, native(self.fix_why.get(name, "")), b))

    def fix_end(self, cs) -> None:
        """The install is over: the running row becomes the summary under the per-item results."""
        self.fix_lines.clear()
        if isinstance(cs, Exception):
            self.fix_now(native(f"安装时出错：{cs}"), "bad")
            return
        for name, st in self.fix_state.items():
            if st == "run":   # never told how it ended
                self.fix_state[name] = "bad"
        self.render_fix_rows()
        done = list(self.fix_state.values())
        parts = [f"{done.count(s)} 项{t}" for s, t in (("ok", "完成"), ("wait", "等重启电脑"), ("bad", "没成功"), ("skip", "跳过"))
                 if done.count(s)]
        self.fix_now("安装结束：" + "，".join(parts) if parts else "没有能自动安装的项",
                     "bad" if "bad" in done else ("" if "wait" in done or not parts else "ok"))

    def render_checks(self):
        """The check rows. During a one-click install the item being installed says so; a failed install says why."""
        clear(self.checks_box)
        for i, c in enumerate(self.checks):
            mark = self.fix_state.get(c["label"]) if self.fixing else None
            right = None
            verb = c.get("verb") or "安装"
            if mark or c["fixable"]:
                text, kind = self.fix_mark(mark, c["label"]) if mark else (f"可自动{verb}", "warn")
                right = label(text, "badge")
                right.setProperty("kind", kind)
            link = WEB_LINK.search(c["how"] or "") if c["ok"] is False else None
            if link:   # a download page the player has to visit (DevEco Studio): one click, nothing to copy
                right = open_link_button(link.group(0), right)
            state = "run" if mark == "run" else ("ok" if c["ok"] else ("" if c["ok"] is None else "bad"))
            why = self.fix_why.get(c["label"], "") if c["ok"] is False else ""
            if why:
                st = self.fix_state.get(c["label"])   # waiting for a restart: the row's own line says what to do
                why = "" if st == "wait" else ("跳过：" if st == "skip" else f"自动{verb}没成功：") + why
            self.checks_box.addWidget(row(state, c["label"], native(c["detail"]), right, native(c["how"]),
                                          last=i == len(self.checks) - 1, err=native(why)))

    def on_checks(self, cs, first):
        fixing, self.fixing = self.fixing, False
        self.re_btn.setEnabled(True)
        if fixing:
            if not isinstance(cs, Exception):
                self.checks = cs   # the words for each item (installed, or started) as things are now
            self.fix_end(cs)
        if isinstance(cs, Exception):
            clear(self.checks_box)
            self.checks_box.addWidget(row("bad", "检查出错", native(str(cs)), last=True))
            return
        self.checks = cs
        self.render_checks()
        bad = [c for c in cs if c["ok"] is False]
        self.checks_sum.setText(f"还有 {len(bad)} 项没准备好" if bad else "环境就绪")
        self.fix_btn.setEnabled(any(c["fixable"] for c in bad))
        self.render_device()
        if first and any(c["key"] not in ("phone", "keys") for c in bad):
            self.show_page("setup")


def app_icon() -> QIcon:
    pix = QPixmap(256, 256)
    pix.fill(Qt.transparent)
    p = QPainter(pix)
    p.setRenderHint(QPainter.Antialiasing)
    g = QLinearGradient(0, 0, 256, 256)
    g.setColorAt(0, QColor("#f4ead7"))
    g.setColorAt(1, QColor("#c9b994"))
    path = QPainterPath()
    path.addRoundedRect(QRectF(8, 8, 240, 240), 56, 56)
    p.fillPath(path, g)
    p.setPen(QColor("#1a1712"))
    f = QFont("Segoe UI", 82)
    f.setBold(True)
    p.setFont(f)
    p.drawText(QRect(0, 0, 256, 256), Qt.AlignCenter, "NX")
    p.end()
    return QIcon(pix)


def bring_to_front(pid: int) -> bool:
    """The NX2Harmony window of process `pid` restored and in front (the process the player just started may hand the
    foreground over). False while that process shows no such window yet."""
    if sys.platform != "win32":
        return False
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32")   # its own function table: the argument types set here stay here
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = user32.IsIconic.argtypes = user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
    user32.GetWindow.restype = wintypes.HWND
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):   # GW_OWNER: a main window
            title = ctypes.create_unicode_buffer(64)
            user32.GetWindowTextW(hwnd, title, 64)
            if title.value == "NX2Harmony":
                found.append(hwnd)
                return False
        return True
    user32.EnumWindows(each, 0)
    if not found:
        return False
    if user32.IsIconic(found[0]):
        user32.ShowWindow(found[0], 9)   # SW_RESTORE
    user32.SetForegroundWindow(found[0])
    return True


def first_instance():
    """One window per app folder and user: a lock in %TEMP% held while the window is open (a crashed owner's lock is
    stale, its PID gone). Returns the lock to hold, or None when another window has it: that one is brought to the
    front instead. The command line (nx2h.exe) takes no lock."""
    key = hashlib.sha1(str(config.APP_DIR).lower().encode("utf-8")).hexdigest()[:12]
    lock = QLockFile(str(Path(tempfile.gettempdir()) / f"NX2Harmony-{key}.lock"))
    lock.setStaleLockTime(0)   # held all session long: only an owner that is gone makes it stale
    if lock.tryLock(300) or lock.error() != QLockFile.LockError.LockFailedError:
        return lock   # ours, or no lock can be made here (an unwritable %TEMP%): run without the guard
    for _ in range(25):   # the other one may still be opening its window
        info = lock.getLockInfo()   # (pid, hostname, appname)
        pid = next((x for x in info if isinstance(x, int) and not isinstance(x, bool)), 0) if isinstance(info, tuple) else 0
        if pid and bring_to_front(pid):
            break
        time.sleep(0.2)
    return None


def main() -> int:
    QApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    app = QApplication(sys.argv)
    app.setApplicationName("NX2Harmony")
    shot = os.environ.get("NX2H_SNAPSHOT")   # a check of the packed app: the window as a PNG after 6 s, then quit
    lock = None if shot else first_instance()   # held until the window closes
    if not shot and lock is None:
        return 0
    app.setFont(QFont("Microsoft YaHei UI", 10))
    app.setStyleSheet(QSS)
    app.setWindowIcon(app_icon())
    w = MainWindow()
    w.show()
    if shot:
        QTimer.singleShot(6000, lambda: (w.grab().save(shot), app.quit()))
    return app.exec()
