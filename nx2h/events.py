"""Progress events. With --progress json each event is one JSON line on stdout, in the schema of nxnative's
build_ipa_from_nsp.py (docs/frontends.md: ev = stage / step / progress / stage_end / game / say, every object with t),
so a graphical front end reads both the same way; this app adds signing, detail (a running step's newest output
line) and awake (the phone's screen kept on). Otherwise the same events are printed for a person (detail only goes to the step's log)."""
from __future__ import annotations

import json
import sys
import threading
import time

DETAIL_EVERY = 0.5   # seconds between detail events: about two a second


class Events:
    def __init__(self, json_mode: bool = False):
        self.json = json_mode
        self.k = 0
        self.n = 0
        self.t0 = {}
        self.names = {}
        self._lock = threading.RLock()   # detail lines may be sent from a timer thread
        self._detail = ""                # the newest line not sent yet
        self._detail_at = 0.0
        self._detail_timer = None

    def _emit(self, ev: str, **fields) -> None:
        if self.json:
            fields = dict(ev=ev, t=round(time.time(), 3), **fields)
            with self._lock:
                sys.stdout.write(json.dumps(fields, ensure_ascii=False) + "\n")
                sys.stdout.flush()

    def _print(self, text: str) -> None:
        if not self.json:
            print(text, flush=True)

    def plan(self, n: int) -> None:
        self.n = n

    def stage(self, name: str, step: str = "") -> None:
        self.k += 1
        self.t0[self.k] = time.time()
        self.names[self.k] = name
        self._emit("stage", k=self.k, n=self.n, name=name, step=step)
        self._print(f"\n[{self.k}/{self.n}] {name}" + (f" · {step}" if step else ""))

    def step(self, step: str) -> None:
        self._emit("step", k=self.k, name=self.names.get(self.k, ""), step=step)
        self._print(f"  - {step}")

    def progress(self, step: str, done: float, total, unit: str = "", note: str = "") -> None:
        frac = min(done / total, 0.99) if total else None
        elapsed = time.time() - self.t0.get(self.k, time.time())
        eta = (elapsed / frac - elapsed) if frac else None
        self._emit("progress", k=self.k, name=self.names.get(self.k, ""), step=step, done=done, total=total, unit=unit, est=False, note=note,
                   frac=frac, eta=eta, elapsed=round(elapsed, 1))
        if not self.json and total:
            size = (lambda b: f"{b / 2**30:.2f} GB") if unit == "B" else str
            print(f"\r    {step}: {size(done)} / {size(total)} ({100 * done / total:.0f}%)", end="", flush=True)

    def stage_end(self, ok: bool, summary: str = "", warn: bool = False) -> None:
        elapsed = round(time.time() - self.t0.get(self.k, time.time()), 1)
        fields = dict(k=self.k, name=self.names.get(self.k, ""), ok=ok, summary=summary, elapsed=elapsed)
        if warn:
            fields["warn"] = True
        self._emit("stage_end", **fields)
        self._print(f"  {'完成' if ok else '失败'}：{summary}（{elapsed:.0f} 秒）")

    def game(self, **fields) -> None:
        self._emit("game", **fields)
        self._print(f"游戏：{fields.get('name')} {fields.get('version')}（{fields.get('title_id')}）→ {fields.get('out') or ''}")

    def signing(self, bundle: str, project: str, opened: bool) -> None:
        """The game's bundle name has no signing on this PC yet: the project to sign it with (a front end offers to
        open it in DevEco Studio again)."""
        self._emit("signing", bundle=bundle, project=project, opened=opened)

    def awake(self, on: bool, target: str = "", server: str = "", restore: str = "") -> None:
        """The phone's screen is kept on (device.Hdc.keep_awake) from now on, or no longer: JSON only. A front end that
        kills the build meanwhile runs `restore` on that phone (hdc [-s server] -t target shell <restore>)."""
        self._emit("awake", on=on, target=target, server=server, restore=restore)

    def say(self, msg: str) -> None:
        self._emit("say", msg=msg)
        self._print(msg)

    def detail(self, text: str) -> None:
        """The running step's newest output line, {"ev": "detail", "text": ...}, for a front end to show live (JSON only:
        a person reads the step's log). At most one every DETAIL_EVERY seconds: a line that comes sooner waits for its
        turn and a newer one replaces it, so the newest line wins. Empty lines are not sent."""
        text = (text or "").strip()
        if not self.json or not text:
            return
        with self._lock:
            self._detail = text
            wait = self._detail_at + DETAIL_EVERY - time.time()
            if wait <= 0:
                self._send_detail()
            elif self._detail_timer is None:
                self._detail_timer = threading.Timer(wait, self._send_detail)
                self._detail_timer.daemon = True
                self._detail_timer.start()

    def detail_end(self) -> None:
        """The step's output ended: its last line now, when it is still waiting."""
        with self._lock:
            if self._detail_timer is not None:
                self._detail_timer.cancel()
            self._send_detail()

    def _send_detail(self) -> None:
        with self._lock:
            text, self._detail, self._detail_timer = self._detail, "", None
            if text:
                self._detail_at = time.time()
                self._emit("detail", text=text)

    def red(self, msg: str) -> None:
        self.say("[RED] " + msg)
