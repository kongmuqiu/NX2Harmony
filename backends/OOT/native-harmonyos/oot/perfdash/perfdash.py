#!/usr/bin/env python3
"""Live performance pages for the Ocarina of Time HarmonyOS port (standard library only).

  http://127.0.0.1:8766/           monitor.html, the simple page: the GPU's and the CPU's frequency and usage over time in
                                   the manner of a SmartPerf monitor: GPU MHz (with the 933 MHz ceiling) and GPU load, MHz
                                   per CPU cluster, usage per core, one status row. Data: /api/sys?since=<time>, which
                                   returns only the samples after <time>.
  http://127.0.0.1:8766/analysis   index.html, the engineer's page: is the GPU busy all the time? Most important first:
     1. the GPU's busy share of each 300-frame window (GPU ms per frame / frame ms) and, when the game runs with
        TOTK_FRAME_TIMELINE=1, the same per frame with a picture of WHEN inside the frame the GPU has nothing to do;
     2. the system's view: gpuLoad and GPU frequency (SmartPerf), the device-state levels the app logs every 5 s;
     3. frame rate and where the frame's time goes (recording, waiting for the GPU, the rest), walker CPU, draws, slots;
     4. the CPU cores, where the critical threads ran, the frequency keeper.
     Data: /api?span=<seconds>.

Sources, sampling only (nothing on the phone is changed, started or stopped; -lockfreq is never sent):
  runtime log   hdc file recv -b <bundle> <files>/android/totk-native.log     every 5 s, only the new bytes are parsed
  SmartPerf     one continuous SP_daemon editor session (sp_session.py):      asked for a sample every 500 ms; this phone's
                hdc shell "SP_daemon -editorServer:<token>", a forward to     SP_daemon delivers one a second whatever is
                the device's tcp:8284, init / start / keepalive / stop        asked. Started again by itself when it ends.
  hilog         hdc shell "hilog -x | grep 'device state'"                    every 5 s

SmartPerf takes one client: while another SP_daemon is on the phone the session waits and the page says so (starting
ours would end the other one); --no-sp runs without a session.
Stopping: Ctrl+C, or `curl -X POST -H "X-Perfdash: quit" http://127.0.0.1:8766/api/quit`, ends the session (stop, the
forward removed, the daemon gone). If the process is killed instead, sp_session.py's watchdog does the same; the forward
a killed process tree leaves in hdc's list is removed by the next start.

    python -I native-harmonyos\\oot\\perfdash\\perfdash.py                       live
    python -I native-harmonyos\\oot\\perfdash\\perfdash.py --replay <log> [--speed N] [--replay-sys <file>]
                                                                               a saved log through the same parser, no hdc

hdc, its server and the device come from the same places as ohos_env.ps1 / ohos_env.py (TOTK_HDC, TOTK_HDC_SERVER,
TOTK_HDC_TARGET in the environment or native-harmonyos\\local.ohos.ps1); without them: hdc's own server and the
only connected device. --hdc / --server / --target / --bundle override.
"""
import argparse
import bisect
import datetime
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.normpath(os.path.join(HERE, "..", ".."))            # native-harmonyos
RUNS = os.path.normpath(os.path.join(NATIVE, "..", "artifacts", "runs"))
LOG_REMOTE = "/data/storage/el2/base/haps/entry/files/android/totk-native.log"
LOG_LOCAL = os.path.join(RUNS, "perfdash-live.log")
DEFAULT_SERVER, DEFAULT_TARGET, DEFAULT_BUNDLE = "", "", "nx2h.u000000.oot"   # "": hdc's own server, the only connected device
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
CLUSTERS = ((0, 1, 2, 3), (4, 5, 6, 7, 8, 9, 10, 11), (12, 13))    # little, middle, Prime (logical CPUs)
CLUSTER_NAMES = ("小核", "中核", "超大核")
GPU_MAX_MHZ = 933                                                   # the GPU's highest frequency step on this phone
SYS_KEEP_S = 2400.0                                                 # /api/sys keeps this many seconds of samples
BIN_MS, BINS = 2.0, 100                                             # the in-frame profile: 2 ms steps up to 200 ms


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return float("nan")


def fin(v, digits=3):
    """A number for JSON: None for missing / NaN / infinite."""
    if v is None or isinstance(v, bool):
        return v
    try:
        return round(v, digits) if math.isfinite(v) else None
    except TypeError:
        return None


# ---- intervals (lists of [begin, end]) -------------------------------------------------------------------------------
def merge(iv):
    out = []
    for a, b in sorted(iv):
        if not b > a:
            continue
        if out and a <= out[-1][1]:
            if b > out[-1][1]:
                out[-1][1] = b
        else:
            out.append([a, b])
    return out


def clip(iv, lo, hi):
    return [[max(a, lo), min(b, hi)] for a, b in iv if b > lo and a < hi]


def complement(iv, lo, hi):
    """[lo, hi] without the merged intervals iv."""
    out, x = [], lo
    for a, b in iv:
        if a > x:
            out.append([x, min(a, hi)])
        x = max(x, b)
        if x >= hi:
            break
    if hi > x:
        out.append([x, hi])
    return out


def subtract(a, b):
    out = []
    for lo, hi in a:
        out += complement(clip(b, lo, hi), lo, hi)
    return out


def total(iv):
    return sum(b - a for a, b in iv)


def inside(iv, x):
    for a, b in iv:
        if a <= x < b:
            return True
    return False


def rounded(iv):
    return [[round(a, 2), round(b, 2)] for a, b in iv]


# ---- the clock: wall time, or the log's own time in a replay ---------------------------------------------------------
class Clock:
    """Live: time.time(). Replay: starts at the moment the replay began, runs `speed` times as fast and stops at the end
    of the log, so the finished replay stays on the page."""

    def __init__(self):
        self.speed, self.r0, self.v0, self.limit = 0.0, time.time(), time.time(), float("inf")

    def start_replay(self, speed, length):
        self.speed, self.r0, self.v0, self.limit = max(speed, 0.01), time.time(), time.time(), length

    def elapsed(self):
        return min((time.time() - self.r0) * self.speed, self.limit) if self.speed else time.time() - self.r0

    def now(self):
        return self.v0 + self.elapsed() if self.speed else time.time()


CLOCK = Clock()


# ---- everything the page shows ---------------------------------------------------------------------------------------
class State:
    def __init__(self):
        self.lock = threading.RLock()
        self.windows = {}            # (run, frame) -> one 300-frame window
        self.order = []              # their keys, oldest first
        self.sp_t, self.sp = [], []  # SmartPerf samples as the analysis page takes them: times, rows
        self.sys_t, self.sys, self.sys_cores, self.sys_extra = [], [], [], {}   # every sample for the simple page (add_sys)
        self.sys_epoch = f"{os.getpid()}-{time.time():.3f}"                      # changes when the rows start again
        self.sampler = None          # sp_session.Sampler (live, unless --no-sp)
        self.cores, self.core_hist = [], {}
        self.dev_t, self.dev = [], []
        self.pins, self.spin_hist, self.keeper, self.topology = {}, [], "", []
        self.tl_rows, self.tl_series, self.tl_profile = [], [], None
        self.status = {k: {"ok": None, "msg": "", "t_ok": None, "t_try": None} for k in ("log", "sp", "hilog")}
        self.mode, self.replay = "live", None

    def say(self, key, ok, msg):
        with self.lock:
            s = self.status[key]
            s["ok"], s["msg"], s["t_try"] = ok, str(msg)[:240], time.time()
            if ok:
                s["t_ok"] = s["t_try"]


S = State()

# ---- the runtime log -------------------------------------------------------------------------------------------------
N = r"(-?[\d.]+)"
RE_FPS = re.compile(r"^\[nvn-vk\] frame (\d+): ([\d.]+) fps over the last (\d+)")
RE_STUTTER = re.compile(r"^\[nvn-vk\] frame (\d+), stutter: longest frame ([\d.]+) ms, (\d+) frames over 33 ms, (\d+) over 50 ms")
RE_PER = re.compile(rf"^\[nvn-vk\] frame (\d+), per frame: recording {N} ms, waiting for the GPU {N} ms, GPU {N} ms, {N} submissions")
RE_PASSES = re.compile(r"^\[nvn-vk\] frame (\d+), pass ends per frame \(([\d.]+)\)")
RE_WALK = re.compile(rf"^\[nvn-vk\] walker cpu frame (\d+): cpu {N} ms/frame, wall {N} ms/frame, draws {N}/frame, cpu {N} us/draw, offcpu {N} ms/frame")
RE_WAITS = re.compile(rf"^\[nvn-vk\] waits/frame by kind: ring slot {N} ms \({N} blocked\), uniform-snapshot arena {N} ms \({N} blocked\), "
                      rf"other fences {N} ms \({N} blocked\); slots sealed with fewer than (\d+) draws {N}/frame(?: \({N} of them inside a game submission\))?, "
                      rf"chunk submissions {N}/frame; ring (\d+), snapshot arenas (\d+)")
RE_SLOTS = re.compile(rf"^\[nvn-vk\] slot submission/frame: all/game/handed {N}/{N}/{N}, game slots/NVN submit {N}; "
                      rf"walking publication join {N} ms, driver QueueSubmit {N} ms")
RE_PIN = re.compile(r"^\[ohos-pin\] (\S+) (\d+) \(role (\d+)\) ran on((?: cpu\d+:\d+%)*); QoS (-?\d+), nice (-?\d+), uclamp (-?\d+)/(-?\d+), mask (\S*)")
RE_TOPO = re.compile(r"^\[ohos-pin\] topology \(cpu:\[thread siblings\]max MHz\):(.*)")
RE_SPIN = re.compile(r"^\[ohos-spin\] the last (\d+) s: spun ([\d.]+) s, slept ([\d.]+) s, on cpu (-?\d+) now")
# The frame timeline rows: the same expression and field order as native-harmonyos/oot/frametl.py (that script runs its
# main() when imported, so its parsing is repeated here; keep the two in step).
ROW = re.compile(r'\[frame-tl late\] frame=(\d+) target=([\d.]+) game_iv=([\d.nan]+).*?wake_gap=([\d.nan]+) dispatch=([\d.]+) walk=([\d.]+) offcpu=([\d.]+) join=([\d.]+) '
                 r'submit_latency=([\d.]+) queue_cpu=([\d.]+) gpu=([\d.]+) gpu_tail=([\d.]+) gpu_backlog_or_observe=([\d.]+) gpu_complete=(\d) acquire=([\d.]+).*?'
                 r'relative_prev_game_ms\([^)]*\)=\[([^\]]*)\] walks=(.*?) slots=(.*?) draws=(\d+) passes=(\d+)')
RE_BRACKET = re.compile(r'\[([^\]]*)\]')


def tl_parse(line):
    """One [frame-tl late] row as frametl.py reads it: times are ms after the game's previous present."""
    m = ROW.search(line)
    if not m:
        return None
    g = m.groups()
    walks = [tuple(num(x.replace('cpu=', '')) for x in w.split(',')) for w in RE_BRACKET.findall(g[16])]
    slots = []
    for s in RE_BRACKET.findall(g[17]):
        t, rest = s.split(':', 1)
        v = [num(x.replace('gpu=', '')) for x in rest.split(',')]
        slots.append((int(t), *v))   # ticket, seal, queue begin, queue end, gpu done, gpu ms
    return dict(frame=int(g[0]), iv=num(g[2]), walk=num(g[5]), offcpu=num(g[6]), join=num(g[7]), submit_latency=num(g[8]),
                queue_cpu=num(g[9]), gpu=num(g[10]), gpu_tail=num(g[11]), complete=int(g[13]), walks=walks, slots=slots,
                draws=int(g[18]), passes=int(g[19]))


class LogParser:
    """Takes the log's lines as they arrive (feed) and keeps the windows, the timeline and the thread lines in S."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.run = getattr(self, "run", -1) + 1
        self.pending, self.pending_frame = {}, {}       # statistics printed before their window's fps line
        self.last_frame, self.win_n = 0, 300
        self.tl_prev, self.tl_abs, self.tl_agg, self.tl_bad = None, [], {}, 0

    def restart(self):
        """The game started again (the log began anew): thread lines of the old process no longer apply."""
        with S.lock:
            self.reset()
            S.pins, S.keeper, S.tl_rows, S.tl_series, S.tl_profile = {}, "", [], [], None

    def feed(self, lines, t):
        """`lines` arrived at time t (the clock's): the last window among them ended about then."""
        with S.lock:
            new, touched = [], set()
            for l in lines:
                self.line(l, t, new, touched)
            x = t
            for w in reversed(new):                     # several windows at once: each ended one duration earlier
                w["t"] = x
                x -= w["dur"] or 0.0
            for wf, a in self.tl_agg.items():
                w = S.windows.get((self.run, wf))
                if w is not None and a["n"]:
                    w["tl"] = self.tl_summary(a)
                    touched.add((self.run, wf))
            for wf in sorted(self.tl_agg)[:-3]:
                del self.tl_agg[wf]
            if self.tl_agg:
                wf = max(self.tl_agg)
                a = self.tl_agg[wf]
                if a["np"]:
                    last = max(k for k in range(BINS) if a["cover"][k]) + 1 if any(a["cover"]) else 0
                    S.tl_profile = {"frame": wf, "n": a["np"], "bin": BIN_MS, "cover": a["cover"][:last],
                                    "idle": a["idle"][:last], "walk": a["walkb"][:last]}
            for key in touched:
                derive(S.windows[key])
            if len(S.order) > 400:
                for key in S.order[:100]:
                    S.windows.pop(key, None)
                del S.order[:100]

    def window(self, frame):
        return S.windows.get((self.run, frame))

    def put(self, frame, values, touched):
        w = self.window(frame)
        if w is None:
            self.pending_frame.setdefault(frame, {}).update(values)
        else:
            w.update(values)
            touched.add((self.run, frame))

    def line(self, l, t, new, touched):
        if l.startswith("[frame-tl late]"):
            self.tl_row(l)
        elif l.startswith("[nvn-vk] "):
            if l.startswith("[nvn-vk] frame "):
                m = RE_FPS.match(l)
                if m:
                    frame, fps, n = int(m.group(1)), num(m.group(2)), int(m.group(3))
                    if frame <= self.last_frame:        # a new process whose log replaced the old one unnoticed
                        held = self.pending, self.pending_frame
                        self.restart()
                        self.pending, self.pending_frame = held
                    self.last_frame, self.win_n = frame, n or 300
                    w = {"run": self.run, "frame": frame, "n": n, "fps": fps, "t": t,
                         "dur": n / fps if fps > 0 else None, "frame_ms": 1000.0 / fps if fps > 0 else None}
                    w.update(self.pending)
                    w.update(self.pending_frame.pop(frame, {}))
                    self.pending, self.pending_frame = {}, {}
                    S.windows[(self.run, frame)] = w
                    S.order.append((self.run, frame))
                    new.append(w)
                    touched.add((self.run, frame))
                    return
                m = RE_PER.match(l)
                if m:
                    self.put(int(m.group(1)), {"rec_ms": num(m.group(2)), "wait_ms": num(m.group(3)), "gpu_ms": num(m.group(4)),
                                               "subs": num(m.group(5))}, touched)
                    return
                m = RE_STUTTER.match(l)
                if m:
                    self.put(int(m.group(1)), {"longest_ms": num(m.group(2)), "over33": int(m.group(3)), "over50": int(m.group(4))}, touched)
                    return
                m = RE_PASSES.match(l)
                if m:
                    self.put(int(m.group(1)), {"passes": num(m.group(2))}, touched)
            elif l.startswith("[nvn-vk] walker cpu frame "):
                m = RE_WALK.match(l)
                if m:
                    self.put(int(m.group(1)), {"w_cpu": num(m.group(2)), "w_wall": num(m.group(3)), "draws": num(m.group(4)),
                                               "us_draw": num(m.group(5)), "w_off": num(m.group(6))}, touched)
            elif l.startswith("[nvn-vk] waits/frame by kind"):
                m = RE_WAITS.match(l)
                if m:
                    g = m.groups()
                    self.pending.update({"ring_ms": num(g[0]), "ring_blk": num(g[1]), "arena_ms": num(g[2]), "arena_blk": num(g[3]),
                                         "fence_ms": num(g[4]), "fence_blk": num(g[5]), "small_lim": int(g[6]), "small": num(g[7]),
                                         "small_mid": num(g[8]) if g[8] is not None else None, "chunks": num(g[9]),
                                         "ring": int(g[10]), "arenas": int(g[11])})
            elif l.startswith("[nvn-vk] slot submission/frame"):
                m = RE_SLOTS.match(l)
                if m:
                    g = m.groups()
                    self.pending.update({"slots_all": num(g[0]), "slots_game": num(g[1]), "slots_handed": num(g[2]),
                                         "per_submit": num(g[3]), "join_ms": num(g[4]), "qsubmit_ms": num(g[5])})
        elif l.startswith("[ohos-pin] "):
            m = RE_PIN.match(l)
            if m:
                cpus = {int(c): int(p) for c, p in re.findall(r"cpu(\d+):(\d+)%", m.group(4))}
                S.pins[int(m.group(2))] = {"name": m.group(1), "tid": int(m.group(2)), "role": int(m.group(3)), "cpus": cpus,
                                           "qos": int(m.group(5)), "nice": int(m.group(6)), "uclamp": f"{m.group(7)}/{m.group(8)}",
                                           "mask": m.group(9), "t": t}
                return
            m = RE_TOPO.match(l)
            if m:
                S.topology = [{"cpu": int(c), "sib": s, "max": int(x)} for c, s, x in re.findall(r"(\d+):\[([\d,\-]+)\](\d+)", m.group(1))]
        elif l.startswith("[ohos-spin] "):
            m = RE_SPIN.match(l)
            if m:
                S.spin_hist.append({"t": t, "win": int(m.group(1)), "spun": num(m.group(2)), "slept": num(m.group(3)), "cpu": int(m.group(4))})
                del S.spin_hist[:-40]
            else:
                S.keeper = l[len("[ohos-spin] "):].strip()[:200]

    def tl_row(self, l):
        r = tl_parse(l)
        if r is None:
            self.tl_bad += 1
            return
        frame, iv = r["frame"], r["iv"]
        if not (iv == iv and iv > 0):
            self.tl_prev, self.tl_abs = None, []
            return
        prev = self.tl_prev
        chained = prev is not None and frame == prev["frame"] + 1   # the rows before it are known: so is what the GPU still had
        origin = prev["origin"] + prev["iv"] if chained else 0.0
        if not chained:
            self.tl_abs = []
        if origin > 1e7:                                            # keep the running sums small
            self.tl_abs = [(a - origin, b - origin) for a, b in self.tl_abs]
            origin = 0.0
        earlier = [(a, b) for a, b in self.tl_abs if b > origin]
        # the GPU has submitted, unfinished work from a slot's QueueSubmit to its completion
        own = merge([(s[3], s[4]) for s in r["slots"] if s[3] == s[3] and s[4] == s[4]])
        carry = clip(merge([(a - origin, b - origin) for a, b in earlier]), 0.0, iv)
        own_in = clip(own, 0.0, iv)
        busy = merge(carry + own_in)
        pend = total(busy)
        walks = clip(merge([(w[0], w[1]) for w in r["walks"] if w[0] == w[0] and w[1] == w[1]]), 0.0, iv)
        # slots sealed while no recorded walk was running: the row's walks are not all of the replaying (the walker seals)
        unrec = sum(1 for s in r["slots"] if s[1] == s[1] and not any(w[0] - 0.5 <= s[1] <= w[1] + 0.5 for w in r["walks"]))
        self.tl_abs = earlier + [(origin + a, origin + b) for a, b in own]
        self.tl_prev = {"frame": frame, "origin": origin, "iv": iv}
        gpu = r["gpu"] if r["gpu"] == r["gpu"] else 0.0
        S.tl_rows.append({
            "frame": frame, "iv": fin(iv), "gpu": fin(gpu), "pend": fin(pend) if chained else None,
            "idle_ms": fin(iv - pend) if chained else None, "known": chained, "complete": r["complete"],
            "draws": r["draws"], "passes": r["passes"], "unrec": unrec, "walk": fin(r["walk"]), "offcpu": fin(r["offcpu"]), "join": fin(r["join"]),
            "sublat": fin(r["submit_latency"]), "tail": fin(r["gpu_tail"]),
            "walks": [[fin(w[0], 2), fin(w[1], 2), fin(w[2], 2)] for w in r["walks"]],
            "slots": [[s[0]] + [fin(x, 2) for x in s[1:6]] for s in r["slots"]],
            "carry": rounded(carry), "own": rounded(subtract(own_in, carry)), "idle": rounded(complement(busy, 0.0, iv)) if chained else []})
        del S.tl_rows[:-16]
        S.tl_series.append([frame, fin(iv, 2), fin(gpu, 2), fin(pend, 2) if chained else None, len(r["slots"])])
        del S.tl_series[:-600]
        n = self.win_n or 300
        a = self.tl_agg.setdefault(((frame - 1) // n + 1) * n, dict(n=0, iv=0.0, gpu=0.0, np=0, piv=0.0, pend=0.0, tail=0.0, sublat=0.0,
                                                                     slots=0, walk=0.0, short=0, cover=[0] * BINS, idle=[0] * BINS, walkb=[0] * BINS))
        a["n"] += 1; a["iv"] += iv; a["gpu"] += gpu; a["slots"] += len(r["slots"]); a["short"] += unrec >= 3
        a["tail"] += r["gpu_tail"] if r["gpu_tail"] == r["gpu_tail"] else 0.0
        a["sublat"] += r["submit_latency"] if r["submit_latency"] == r["submit_latency"] else 0.0
        a["walk"] += r["walk"] if r["walk"] == r["walk"] else 0.0
        if chained:
            a["np"] += 1; a["piv"] += iv; a["pend"] += pend
            for k in range(BINS):
                c = (k + 0.5) * BIN_MS
                if c >= iv:
                    break
                a["cover"][k] += 1
                if not inside(busy, c):
                    a["idle"][k] += 1
                if inside(walks, c):
                    a["walkb"][k] += 1

    @staticmethod
    def tl_summary(a):
        n, np_ = a["n"], a["np"]
        return {"n": n, "iv": fin(a["iv"] / n), "gpu": fin(a["gpu"] / n), "busy": fin(a["gpu"] / a["iv"], 4) if a["iv"] else None,
                "np": np_, "pend": fin(a["pend"] / np_) if np_ else None, "pend_share": fin(a["pend"] / a["piv"], 4) if a["piv"] else None,
                "idle": fin((a["piv"] - a["pend"]) / np_) if np_ else None, "tail": fin(a["tail"] / n), "sublat": fin(a["sublat"] / n),
                "slots": fin(a["slots"] / n, 2), "walk": fin(a["walk"] / n), "short": fin(a["short"] / n, 3)}


def derive(w):
    f, g, r, wt = w.get("frame_ms"), w.get("gpu_ms"), w.get("rec_ms"), w.get("wait_ms")
    ok = lambda v: v is not None and v == v
    w["busy"] = g / f if ok(f) and ok(g) and f > 0 else None
    w["idle_ms"] = f - g if ok(f) and ok(g) else None
    w["rest_ms"] = f - r - wt if ok(f) and ok(r) and ok(wt) else None


# ---- the system's samples --------------------------------------------------------------------------------------------
RE_KV = re.compile(r"\b([A-Za-z_]\w*)=(\S+)")
RE_DEV = re.compile(r"device state: temp level (-?\d+), GPU load (-?\d+) \(vertex (-?\d+), fragment (-?\d+), texture (-?\d+), "
                    r"bandwidth (-?\d+)\), GPU (-?\d+) kHz, thermal margin (-?\d+) s, trend (-?\d+)")
RE_STAMP = re.compile(r"^\s*(\d\d-\d\d \d\d:\d\d:\d\d(?:\.\d+)?)")


def mhz(v):
    """A frequency in MHz whatever unit SP_daemon printed it in (CPU: kHz, GPU: Hz)."""
    v = num(v)
    if v != v:
        return None
    return v / 1e6 if v > 1e8 else v / 1e3 if v > 1e5 else v


def sp_blocks(text):
    """SP_daemon's output ("order:N key=value" lines; every sample begins with timestamp=) as one dict per sample."""
    out, cur = [], None
    for k, v in RE_KV.findall(text):
        if k == "timestamp" or cur is None:
            cur = {}
            out.append(cur)
        cur[k] = v
    return [s for s in out if "gpuLoad" in s or "gpuFrequency" in s or "cpu0Frequency" in s or "cpu0Usage" in s]


def add_sp(kv, t):
    cores = []
    for i in range(64):
        f, u = kv.get(f"cpu{i}Frequency"), kv.get(f"cpu{i}Usage")
        if f is None and u is None:
            continue
        load = num(u)
        cores.append({"i": i, "mhz": fin(mhz(f), 0), "load": fin(load, 1)})
    by = {c["i"]: c for c in cores}

    def cluster(ids, key, how):
        v = [by[i][key] for i in ids if i in by and by[i][key] is not None]
        return fin(how(v), 1) if v else None
    row = [fin(t, 2), fin(num(kv.get("gpuLoad")), 1), fin(mhz(kv.get("gpuFrequency")), 0)]
    row += [cluster(ids, "mhz", max) for ids in CLUSTERS] + [cluster(ids, "load", lambda v: sum(v) / len(v)) for ids in CLUSTERS]
    with S.lock:
        k = bisect.bisect(S.sp_t, t)
        S.sp_t.insert(k, t); S.sp.insert(k, row)
        if cores:
            S.cores = cores
            for c in cores:
                h = S.core_hist.setdefault(c["i"], [])
                h.append([c["mhz"], c["load"]])
                del h[:-90]
        if len(S.sp_t) > 8400:
            del S.sp_t[:1200]; del S.sp[:1200]


def celsius(v):
    """A temperature as SmartPerf prints it (degrees, tenths or thousandths), in degrees."""
    v = num(v)
    if not v == v or v <= 0:
        return None
    return v / 1000.0 if v > 1000 else v / 10.0 if v > 150 else v


def sys_extra(kv):
    """What the status row shows besides the charts: temperatures, the battery's current and voltage, memory."""
    cur, volt = num(kv.get("currentNow")), num(kv.get("voltageNow"))        # mA (negative: the battery discharges), uV
    watt = abs(cur * volt) / 1e9 if cur == cur and volt == volt and volt > 0 else None
    mb = lambda key: num(kv.get(key)) / 1024.0                               # kB
    return {"soc_c": fin(celsius(kv.get("soc_thermal")), 1), "shell_c": fin(celsius(kv.get("shell_frame")), 1),
            "board_c": fin(celsius(kv.get("system_h")), 1), "battery_c": fin(celsius(kv.get("Battery")), 1),
            "ma": fin(cur, 0), "volt": fin(volt / 1e6, 3), "watt": fin(watt, 2) if watt is not None and watt < 100 else None,
            "mem_avail_mb": fin(mb("memAvailable"), 0), "mem_total_mb": fin(mb("memTotal"), 0)}


def add_sys(kv, t, coarse=False):
    """One sample for the simple page: [time, GPU MHz, GPU load, every core's MHz, every core's usage]. `coarse`: a
    session's first sample, whose CPU usage was measured over a few milliseconds (0 / 50 / 100) and is left out."""
    ids = [i for i in range(64) if f"cpu{i}Frequency" in kv or f"cpu{i}Usage" in kv]
    with S.lock:
        if ids and ids != S.sys_cores:              # the first sample, or other cores than before: the rows start again
            S.sys_t, S.sys, S.sys_cores = [], [], ids
            S.sys_epoch = f"{os.getpid()}-{time.time():.3f}"
        ids = S.sys_cores
        row = [fin(t, 2), fin(mhz(kv.get("gpuFrequency")), 0), fin(num(kv.get("gpuLoad")), 1)]
        row += [fin(mhz(kv.get(f"cpu{i}Frequency")), 0) for i in ids]
        row += [None if coarse else fin(num(kv.get(f"cpu{i}Usage")), 1) for i in ids]
        k = bisect.bisect(S.sys_t, t)
        S.sys_t.insert(k, t); S.sys.insert(k, row)
        extra = sys_extra(kv)
        if any(v is not None for v in extra.values()):
            S.sys_extra = dict(extra, t=fin(t, 2))
        drop = bisect.bisect_left(S.sys_t, S.sys_t[-1] - SYS_KEEP_S)
        if drop > 600:
            del S.sys_t[:drop]; del S.sys[:drop]


def add_dev(m, t):
    g = [int(x) for x in m.groups()]
    lv = lambda v: v if v >= 0 else None            # -1: the system gave no value
    row = [fin(t, 2), lv(g[0]), lv(g[1]), lv(g[2]), lv(g[3]), lv(g[4]), lv(g[5]), fin(g[6] / 1000.0, 0) if g[6] > 0 else None, g[7], g[8]]
    with S.lock:
        k = bisect.bisect(S.dev_t, t)
        S.dev_t.insert(k, t); S.dev.insert(k, row)
        if len(S.dev_t) > 1500:
            del S.dev_t[:300]; del S.dev[:300]


def stamp_seconds(s):
    """A hilog time stamp (MM-DD HH:MM:SS.mmm) as seconds; only differences are used."""
    try:
        d = datetime.datetime.strptime("2000-" + s, "%Y-%m-%d %H:%M:%S.%f" if "." in s else "%Y-%m-%d %H:%M:%S")
        return (d - datetime.datetime(2000, 1, 1)).total_seconds()
    except ValueError:
        return None


# ---- live: hdc pollers -----------------------------------------------------------------------------------------------
class Hdc:
    def __init__(self, exe, server, target):
        self.base = [exe] + (["-s", server] if server else []) + (["-t", target] if target else [])

    def run(self, *args, timeout=20):
        r = subprocess.run(self.base + list(args), stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout, creationflags=NOWIN)
        return (r.stdout + r.stderr).decode("utf-8", "replace")


def brief(e):
    if isinstance(e, subprocess.TimeoutExpired):
        return "hdc 超时（手机没连上？）"
    if isinstance(e, FileNotFoundError):
        return "找不到 hdc（--hdc 或 TOTK_HDC）"
    return " ".join(str(e).split())[:200] or type(e).__name__


class Recorder:
    """--record: the system samples as hdc printed them, each under '#s <time>' / '#d <time>', for --replay-sys."""

    def __init__(self, path):
        self.path, self.mu = path, threading.Lock()

    def write(self, mark, t, text):
        if not self.path:
            return
        try:
            with self.mu, open(self.path, "a", encoding="utf-8") as f:
                f.write(f"#{mark} {t:.3f}\n{text.strip()}\n")
        except OSError:
            pass


def log_loop(hdc, bundle, interval, parser):
    offset, head, grew = 0, None, time.time()
    part = LOG_LOCAL + ".part"
    os.makedirs(RUNS, exist_ok=True)
    while True:
        t0 = time.time()
        try:
            if os.path.exists(part):
                os.remove(part)
            out = hdc.run("file", "recv", "-b", bundle, LOG_REMOTE, part, timeout=90)
            if not os.path.exists(part):
                raise RuntimeError(out or "hdc file recv 没有取回文件")
            os.replace(part, LOG_LOCAL)
            size = os.path.getsize(LOG_LOCAL)
            with open(LOG_LOCAL, "rb") as f:
                h = f.read(512)
                if size < offset or (head is not None and h[:len(head)] != head):   # the game started again
                    parser.restart()
                    offset = 0
                head = h
                f.seek(offset)
                data = f.read()
            cut = data.rfind(b"\n") + 1
            if cut:
                parser.feed(data[:cut].decode("utf-8", "replace").split("\n"), t0)
                offset += cut
                grew = t0
            quiet = t0 - grew
            S.say("log", True, f"{size / 1048576:.1f} MB，本次新增 {data[:cut].count(10)} 行" +
                  (f"；{quiet:.0f} 秒没有新内容（游戏没在跑？）" if quiet > 3 * interval else ""))
        except Exception as e:      # the phone may be away: keep the last data, say so, try again
            S.say("log", False, brief(e))
        time.sleep(max(1.0, interval - (time.time() - t0)))


RE_SP_KEEP = re.compile(r"^(?:cpu\d+(?:Frequency|Usage)|gpu(?:Frequency|Load)|soc_thermal|shell_frame|system_h|Battery|currentNow|voltageNow|memAvailable|memTotal)$")
RE_CORE_USAGE = re.compile(r"^cpu\d+Usage$")


def start_sampler(hdc, bundle, interval_ms, rec):
    """The continuous SmartPerf session (sp_session.py) in its own thread; every sample goes to both pages' rows."""
    sys.path.insert(0, HERE)            # python -I leaves the script's folder out of the path
    from sp_session import Sampler

    def sample(kv, t, first):
        add_sys(kv, t, coarse=first)
        add_sp({k: v for k, v in kv.items() if not RE_CORE_USAGE.match(k)} if first else kv, t)
        if rec.path and not first:      # as sys_records reads it back: a sample begins with timestamp=
            rec.write("s", t, f"timestamp={kv.get('timestamp', '')} " + " ".join(f"{k}={v}" for k, v in kv.items() if RE_SP_KEEP.match(k)))

        with S.lock:                    # the analysis page's status line: SmartPerf answered just now
            S.status["sp"]["t_ok"] = S.status["sp"]["t_try"] = time.time()

    def state(ok, msg):
        S.say("sp", ok, msg)

    S.sampler = Sampler(hdc.base, bundle, interval_ms, on_sample=sample, on_state=state, log=lambda m: print(m, flush=True))
    S.sampler.start()


def hilog_loop(hdc, rec):
    seen = ""
    while True:
        t0 = time.time()
        try:
            out = hdc.run("shell", "date '+%m-%d %H:%M:%S'; hilog -x | grep 'device state'", timeout=25)
            lines = out.splitlines()
            there = stamp_seconds(lines[0].strip()) if lines and re.match(r"^\d\d-\d\d \d\d:\d\d:\d\d\s*$", lines[0]) else None
            found = []
            for l in lines:
                m = RE_DEV.search(l)
                if m:
                    s = RE_STAMP.match(l)
                    found.append((s.group(1) if s else "", m, l))
            if there is None and not found:
                raise RuntimeError(out or "hilog 没有输出")
            if there is None:                       # no clock line: take the newest line as "now"
                stamps = [x for x in (stamp_seconds(f[0]) for f in found if f[0]) if x is not None]
                there = max(stamps) if stamps else None
            fresh = 0
            for stamp, m, l in found:
                if stamp and stamp <= seen:         # hilog -x prints its whole buffer every time
                    continue
                seen = max(seen, stamp)
                at = stamp_seconds(stamp) if stamp else None
                age = max(there - at, 0.0) if there is not None and at is not None else 0.0
                if age < 1800:
                    add_dev(m, t0 - age)
                    rec.write("d", t0 - age, l)
                    fresh += 1
            S.say("hilog", True, f"新增 {fresh} 行" if found else "hilog 里没有 device state 行（游戏没在跑？）")
        except Exception as e:
            S.say("hilog", False, brief(e))
        time.sleep(max(1.0, 5.0 - (time.time() - t0)))


def hdc_settings(a):
    """The same order as ohos_env.ps1: the environment, native-harmonyos/local.ohos.ps1 (through ohos_env.py), defaults."""
    try:
        sys.path.insert(0, NATIVE)
        import ohos_env  # noqa: F401  (fills os.environ from the local files)
    except Exception:
        pass
    env = os.environ.get
    deveco = env("TOTK_DEVECO") or r"C:\Program Files\Huawei\DevEco Studio"
    exe = a.hdc or env("TOTK_HDC") or shutil.which("hdc") or os.path.join(deveco, "sdk", "default", "openharmony", "toolchains", "hdc.exe")
    server = a.server if a.server is not None else env("TOTK_HDC_SERVER") or DEFAULT_SERVER
    target = a.target if a.target is not None else env("TOTK_HDC_TARGET") or DEFAULT_TARGET
    return exe, server, target, a.bundle or env("TOTK_OHOS_BUNDLE") or DEFAULT_BUNDLE


# ---- replay: a saved log through the same parser, no hdc -------------------------------------------------------------
def replay_plan(lines):
    """[(seconds after the start, first line, end line)]: every window's statistics and timeline rows arrive together when
    the window ends (its length is 300 / fps), the lines between two windows spread over the next window's time."""
    marks, prev = [], 0
    for i, l in enumerate(lines):
        if not l.startswith("[nvn-vk] frame "):
            continue
        m = RE_FPS.match(l)
        if not m:
            continue
        fps = num(m.group(2))
        first = i
        for k in range(i - 1, max(prev, i - 150) - 1, -1):          # the statistics printed just before the fps line
            if lines[k].startswith(("[nvn-vk] slot submission/frame", "[nvn-vk] waits/frame by kind")):
                first = k
        marks.append((first, i, int(m.group(3)) / fps if fps > 0 else 0.0))
        prev = i + 1
    if not marks:
        return [(0.0, 0, len(lines))], 1.0
    plan, at = [(0.0, 0, marks[0][0])], 0.0
    for k, (first, i, dur) in enumerate(marks):
        at += dur
        end = marks[k + 1][0] if k + 1 < len(marks) else len(lines)
        burst = min(end, i + 80)
        for q in range(end - 1, i, -1):
            if lines[q].startswith("[frame-tl "):
                burst = q + 1
                break
        plan.append((at, first, burst))
        following = marks[k + 1][2] if k + 1 < len(marks) else 5.0
        for p in range(5):
            a, b = burst + (end - burst) * p // 5, burst + (end - burst) * (p + 1) // 5
            if b > a:
                plan.append((at + following * (p + 1) / 6.0, a, b))
    return plan, at + 5.0


def replay_loop(lines, plan, parser):
    for at, a, b in plan:
        while CLOCK.elapsed() < at:
            time.sleep(0.02)
        parser.feed(lines[a:b], CLOCK.v0 + at)
    with S.lock:
        S.replay["done"] = True
    S.say("log", True, f"回放完：{len(lines)} 行")


def sys_records(text):
    """A capture of the system samples for --replay-sys: SP_daemon output and hilog 'device state' lines as hdc printed
    them. With the '#s <time>' / '#d <time>' lines --record writes, those times are used; without them SP_daemon samples
    are one second apart and hilog lines keep the distances of their own time stamps."""
    recs, mark, block, block_t = [], None, [], None

    def flush():
        nonlocal block, block_t
        for k, kv in enumerate(sp_blocks("\n".join(block))):
            recs.append([block_t + k if block_t is not None else None, "s", kv])
        block, block_t = [], None
    first_stamp, last_dev = None, None
    for l in text.splitlines():
        m = re.match(r"^#([sd]) ([\d.]+)\s*$", l)
        if m:
            flush()
            if m.group(1) == "s":
                block_t, mark = float(m.group(2)), None
            else:
                mark = float(m.group(2))
            continue
        if l.startswith("#"):
            continue
        d = RE_DEV.search(l)
        if d:
            t = mark
            if t is None:
                s = RE_STAMP.match(l)
                at = stamp_seconds(s.group(1)) if s else None
                if at is not None:
                    first_stamp = at if first_stamp is None else first_stamp
                    t = ("stamp", at - first_stamp)
                else:
                    last_dev = 0.0 if last_dev is None else last_dev + 5.0
                    t = ("stamp", last_dev)
            recs.append([t, "d", d])
            mark = None
        elif "=" in l:
            block.append(l)
    flush()
    marked = [r[0] for r in recs if isinstance(r[0], float)]
    base, k = (min(marked) if marked else 0.0), 0
    for r in recs:
        if isinstance(r[0], float):
            r[0] -= base
        elif isinstance(r[0], tuple):
            r[0] = r[0][1]
        else:
            r[0], k = float(k), k + 1
    return sorted(recs, key=lambda r: r[0])


def replay_sys_loop(recs):
    i = 0
    while i < len(recs):
        e = CLOCK.elapsed()
        while i < len(recs) and recs[i][0] <= e:
            at, kind, what = recs[i]
            (add_sp if kind == "s" else add_dev)(what, CLOCK.v0 + at)
            if kind == "s":
                add_sys(what, CLOCK.v0 + at)
            i += 1
        if e >= CLOCK.limit:
            break
        time.sleep(0.02)
    S.say("sp", True, f"回放完：{sum(1 for r in recs if r[1] == 's')} 个采样")
    S.say("hilog", True, f"回放完：{sum(1 for r in recs if r[1] == 'd')} 行")


# ---- /api ------------------------------------------------------------------------------------------------------------
WINDOW_KEYS = ("frame", "n", "fps", "frame_ms", "gpu_ms", "busy", "idle_ms", "rec_ms", "wait_ms", "rest_ms", "subs", "w_cpu", "w_wall",
               "w_off", "draws", "us_draw", "ring_ms", "ring_blk", "arena_ms", "arena_blk", "fence_ms", "fence_blk", "small",
               "small_lim", "small_mid", "chunks", "ring", "arenas", "slots_all", "slots_game", "slots_handed", "per_submit",
               "join_ms", "qsubmit_ms", "longest_ms", "over33", "over50", "passes")


def window_system(w):
    """What the system reported while the window ran: SP_daemon's GPU frequency and load, the device-state levels."""
    lo, hi = w["t"] - (w["dur"] or 0.0), w["t"]
    out = {}
    rows = S.sp[bisect.bisect_right(S.sp_t, lo):bisect.bisect_right(S.sp_t, hi)]
    f = [r[2] for r in rows if r[2] is not None]
    g = [r[1] for r in rows if r[1] is not None]
    if f:
        out.update(gpu_mhz=fin(sum(f) / len(f), 0), gpu_mhz_min=min(f), gpu_mhz_max=max(f))
    if g:
        out.update(gpu_load=fin(sum(g) / len(g), 1), n=len(g))
    rows = S.dev[bisect.bisect_right(S.dev_t, lo):bisect.bisect_right(S.dev_t, hi)]
    for key, col, how in (("temp", 1, max), ("lvl", 2, None), ("frag", 4, None), ("bw", 6, None), ("dev_mhz", 7, None)):
        v = [r[col] for r in rows if r[col] is not None]
        if v:
            out[key] = how(v) if how else fin(sum(v) / len(v), 1)
    return out or None


def snapshot(span):
    now = CLOCK.now()
    with S.lock:
        lo = now - span - 120
        wins = []
        for key in S.order[-260:]:
            w = S.windows.get(key)
            if w is None or w.get("t") is None or w["t"] < lo:
                continue
            o = {k: fin(w.get(k)) for k in WINDOW_KEYS}
            o.update(t=fin(w["t"], 2), dur=fin(w["dur"], 2), run=w["run"], busy=fin(w.get("busy"), 4), tl=w.get("tl"), sys=window_system(w))
            wins.append(o)
        k = bisect.bisect_left(S.sp_t, now - span - 5)
        d = bisect.bisect_left(S.dev_t, now - span - 10)
        status = {}
        for key, s in S.status.items():
            status[key] = {"ok": s["ok"], "msg": s["msg"], "age": fin(time.time() - s["t_ok"], 1) if s["t_ok"] else None}
        pins = sorted((p for p in S.pins.values() if now - p["t"] < 60), key=lambda p: (-p["role"], p["tid"]))
        out = {
            "now": fin(now, 2), "mode": S.mode, "replay": S.replay, "status": status, "windows": wins,
            "tl": {"frames": S.tl_rows[-12:], "series": S.tl_series[-300:], "profile": S.tl_profile} if S.tl_rows else None,
            "sp": {"series": S.sp[k:], "cores": S.cores, "hist": {str(i): h[-60:] for i, h in S.core_hist.items()},
                   "t": fin(S.sp_t[-1], 2) if S.sp_t else None},
            "dev": S.dev[d:],
            "cpu": {"pins": [dict(p, t=fin(p["t"], 2)) for p in pins], "spin": [dict(s, t=fin(s["t"], 2)) for s in S.spin_hist[-6:]],
                    "keeper": S.keeper, "topology": S.topology},
        }
        return json.dumps(out, ensure_ascii=False, allow_nan=False).encode("utf-8")


def sys_snapshot(since, span):
    """The simple page's data: the samples after `since` (at most the last `span` seconds), the latest status-row values
    and the sampler's state. The page keeps what it has and asks only for what is new."""
    now = CLOCK.now()
    with S.lock:
        k = bisect.bisect_right(S.sys_t, max(since, now - span))
        rows, last = S.sys[k:], (S.sys_t[-1] if S.sys_t else None)
        recent = S.sys_t[bisect.bisect_left(S.sys_t, last - 20.0):] if last is not None else []
        rate = (len(recent) - 1) / (recent[-1] - recent[0]) if len(recent) >= 3 and recent[-1] > recent[0] else None
        if S.mode == "replay":
            st = {"state": "replay", "msg": f"回放 {S.replay['file']}" + (f"，系统采样 {S.replay['sys']}" if S.replay["sys"] else "：回放里没有系统采样（--replay-sys）")}
        elif S.sampler is None:
            st = {"state": "off", "msg": S.status["sp"]["msg"] or "没有 SmartPerf 会话"}
        else:
            st = S.sampler.status()
            st["since"] = fin(st.get("since"), 2)
        st.update(rate=fin(rate, 2) if rate is not None and (S.mode == "replay" or now - last < 20) else None,
                  last=fin(last, 2), age=fin(now - last, 2) if last is not None else None, count=len(S.sys_t))
        out = {"now": fin(now, 2), "epoch": S.sys_epoch, "mode": S.mode, "cores": S.sys_cores,
               "clusters": [{"name": n, "ids": [i for i in ids if i in S.sys_cores]} for n, ids in zip(CLUSTER_NAMES, CLUSTERS)],
               "gpu_max": GPU_MAX_MHZ, "rows": rows, "extra": S.sys_extra, "status": st}
        return json.dumps(out, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")


PAGES = {"/": "monitor.html", "/index.html": "monitor.html", "/monitor": "monitor.html",
         "/analysis": "index.html", "/analysis.html": "index.html"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        try:
            q = parse_qs(u.query)
            if u.path == "/api":
                span = float(q.get("span", ["600"])[0])
                self.send(200, snapshot(min(max(span, 60.0), 3600.0)), "application/json; charset=utf-8")
            elif u.path == "/api/sys":
                since, span = float(q.get("since", ["0"])[0]), float(q.get("span", ["900"])[0])
                self.send(200, sys_snapshot(since if since == since else 0.0, min(max(span, 10.0), SYS_KEEP_S)), "application/json; charset=utf-8")
            elif u.path in PAGES:
                with open(os.path.join(HERE, PAGES[u.path]), "rb") as f:
                    self.send(200, f.read(), "text/html; charset=utf-8")
            else:
                self.send(404, b"not found", "text/plain")
        except (BrokenPipeError, ConnectionError):
            pass
        except Exception as e:      # a page request must never end the server
            try:
                self.send(500, f"{type(e).__name__}: {e}".encode("utf-8"), "text/plain; charset=utf-8")
            except OSError:
                pass

    def do_POST(self):
        """/api/quit with the header X-Perfdash: quit (a page of another site cannot send that header): the clean end."""
        try:
            if urlparse(self.path).path == "/api/quit" and self.headers.get("X-Perfdash") == "quit":
                self.send(200, b"stopping\n", "text/plain")
                threading.Thread(target=self.server.shutdown, daemon=True).start()
            else:
                self.send(404, b"not found", "text/plain")
        except OSError:
            pass


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = os.name != "nt"   # on Windows that option lets a second server take a port in use: fail instead


def main():
    ap = argparse.ArgumentParser(description="Live performance page for the OoT HarmonyOS port (sampling only).")
    ap.add_argument("--port", type=int, default=8766, help="the page's port (default 8766; 8765 is the TOTK page)")
    ap.add_argument("--replay", metavar="LOG", help="feed a saved runtime log through the parser instead of the phone (no hdc)")
    ap.add_argument("--speed", type=float, default=60.0, help="replay: how many times faster than the game ran (default 60; 1 = as it ran)")
    ap.add_argument("--replay-sys", metavar="FILE", help="replay: SP_daemon output / hilog 'device state' lines to play alongside (a --record file)")
    ap.add_argument("--hdc", help="hdc executable (default: TOTK_HDC, hdc on PATH, the one inside DevEco Studio)")
    ap.add_argument("--server", help="hdc server for -s (default: TOTK_HDC_SERVER, else hdc's own; '' for hdc's own)")
    ap.add_argument("--target", help=f"device key for -t (default: TOTK_HDC_TARGET, else {DEFAULT_TARGET}; '' for the only device)")
    ap.add_argument("--bundle", help=f"the game's bundle name (default: TOTK_OHOS_BUNDLE, else {DEFAULT_BUNDLE})")
    ap.add_argument("--log-interval", type=float, default=5.0, help="seconds between log fetches (5 or more)")
    ap.add_argument("--sp-interval", type=int, default=500, help="the SmartPerf session's -INTERVAL in ms (default 500; this phone's SP_daemon delivers one sample a second whatever is asked)")
    ap.add_argument("--sp-count", type=int, default=1, help=argparse.SUPPRESS)     # of the one-shot SP_daemon calls this page made before; no longer used
    ap.add_argument("--no-sp", action="store_true", help="no SmartPerf session (another tool is using SP_daemon)")
    ap.add_argument("--no-log", action="store_true", help="do not fetch the runtime log (a second page instance beside the main one: both would write the same local copy)")
    ap.add_argument("--no-hilog", action="store_true", help="do not read hilog")
    ap.add_argument("--record", action="store_true", help="live: also save the system samples to artifacts\\runs\\perfdash-sys-<time>.txt")
    a = ap.parse_args()
    parser = LogParser()
    if a.replay:
        with open(a.replay, "rb") as f:
            lines = f.read().decode("utf-8", "replace").split("\n")
        plan, length = replay_plan(lines)
        recs = []
        if a.replay_sys:
            with open(a.replay_sys, "rb") as f:
                recs = sys_records(f.read().decode("utf-8", "replace"))
        S.mode = "replay"
        S.replay = {"file": os.path.basename(a.replay), "speed": a.speed, "seconds": round(length, 1), "done": False,
                    "sys": os.path.basename(a.replay_sys) if a.replay_sys else None}
        S.say("log", True, f"回放 {len(lines)} 行")
        for key in ("sp", "hilog"):
            S.say(key, None, "回放里没有系统采样（--replay-sys）" if not recs else "回放中")
        CLOCK.start_replay(a.speed, length)
        threading.Thread(target=replay_loop, args=(lines, plan, parser), daemon=True).start()
        if recs:
            threading.Thread(target=replay_sys_loop, args=(recs,), daemon=True).start()
        print(f"replay of {a.replay}: {len(plan)} steps, {length:.0f} s of game time at x{a.speed:g}", flush=True)
    else:
        exe, server, target, bundle = hdc_settings(a)
        hdc = Hdc(exe, server, target)
        rec = Recorder(os.path.join(RUNS, time.strftime("perfdash-sys-%Y%m%d-%H%M%S.txt")) if a.record else None)
        if a.no_log:
            S.say("log", None, "已关闭（--no-log）")
        else:
            threading.Thread(target=log_loop, args=(hdc, bundle, max(5.0, a.log_interval), parser), daemon=True).start()
        if a.no_hilog:
            S.say("hilog", None, "已关闭（--no-hilog）")
        else:
            threading.Thread(target=hilog_loop, args=(hdc, rec), daemon=True).start()
        print(f"live: {' '.join(hdc.base)}, bundle {bundle}" + ("" if a.no_log else f"; the log copy is {LOG_LOCAL}") +
              (f"; recording to {rec.path}" if rec.path else ""), flush=True)
    try:
        srv = Server(("127.0.0.1", a.port), Handler)
    except OSError as e:
        sys.exit(f"port {a.port}: {e} (another perfdash is running? use --port)")
    if not a.replay:                    # after the port is ours: a second instance must not start a session and then leave
        if a.no_sp:
            S.say("sp", None, "已关闭（--no-sp）")
        else:
            start_sampler(hdc, bundle, max(100, min(a.sp_interval, 10000)), rec)

    def interrupt(*_):                  # SIGTERM / Ctrl+Break: the same clean end as Ctrl+C
        raise KeyboardInterrupt
    for name in ("SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), interrupt)
            except (OSError, ValueError):
                pass
    print(f"perfdash on http://127.0.0.1:{a.port}/  (the analysis page: /analysis)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if S.sampler is not None:       # session stop, forward removed, daemon gone
            print("stopping the SmartPerf session ...", flush=True)
            S.sampler.stop()


if __name__ == "__main__":
    main()
