#!/usr/bin/env python3
"""One continuous SmartPerf (SP_daemon) editor session over hdc, kept alive: perfdash.py's system sampler (standard library).

The chain is the one the TOTK page's sp_session.py uses (SmartPerf's editor server):
hdc starts `SP_daemon -editorServer:<random token>` on the phone, a free local port is forwarded to the
device's tcp:8284, `init::<token> -SESSIONID 1 -INTERVAL <ms> -c -g -t -p -r`, `start::<token>`, `keepalive::<token>`
every 60 s, and at the end `stop::<token>` (answered stop::True), the forward removed and the hdc shell ended, which ends
the daemon. Monitoring only: -lockfreq is never sent, nothing on the phone is changed.

What was measured on this phone (2026-11-13): the session delivers one sample a second whatever -INTERVAL asks for
(100, 500 and 2000 gave 1.00 s); one sample is one { "name": value, ... } object with about 140 names; the first sample
after start has its CPU usage measured over a few milliseconds (0 / 50 / 100), the later ones over the whole second;
the TCP server needs a moment after the process appears, so the first connection can be reset and is tried again;
SmartPerf takes one client: a second SP_daemon of any kind ends the first ("Signal 15"), so none is started while
another is on the device.

Three classes:
  Session   one session: start(), tick() (keepalive), close(); every sample goes to on_sample(dict of strings, time, first)
  Guard     what cleans up if this process is killed: a watchdog child that ends the session when our end of its stdin
            pipe closes, and a state file (.sp_session-<pid>.json next to this file, no token in it) from which the next
            start removes the forward a killed process tree left behind
  Sampler   the thread that keeps a session alive: starts it, watches it, starts again every few seconds when it ended,
            the phone went away or another SP_daemon is in the way; status() is what the page shows
"""
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
NEWGROUP = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
DEVICE_PORT = 8284
METRICS = "-c -g -t -p -r"        # CPU, GPU, temperatures, battery current and voltage, memory
KEEPALIVE_S, STALL_S, RETRY_S, GAME_CHECK_S = 60.0, 8.0, 5.0, 10.0
FIELD = re.compile(r'"([^"]+)":\s*("[^"]*"|[^,}]*)')
ACK = re.compile(r"(?:init|start|stop)::(?:True|False|INIT_FAILED|START_FAILED|STOP_FAILED)|TOKEN_CHECK_FAILED")
SECRET = re.compile(r"((?:editor|device)Server:)\S+")
STATE_FILE = re.compile(r"^\.sp_session-(\d+)\.json$")


class Offline(Exception):
    """hdc cannot reach the phone."""


class Busy(Exception):
    """Another SP_daemon is on the device."""


class Failed(Exception):
    """The session did not come up, or ended."""


def brief(text, limit=160):
    return " ".join(SECRET.sub(r"\1…", str(text)).split())[:limit]


class Hdc:
    def __init__(self, base):
        self.base = list(base)      # [hdc, "-s", server, "-t", target]

    def run(self, *args, timeout=12):
        r = subprocess.run(self.base + list(args), stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout, creationflags=NOWIN)
        return (r.stdout + r.stderr).decode("utf-8", "replace")


def probe(hdc, bundle=""):
    """One ps on the phone: (the SP_daemon lines, is the game's process there). Offline when hdc cannot run it."""
    bundle = re.sub(r"[^A-Za-z0-9._-]", "", bundle or "")
    what = "-e SP_daemon" + (f" -e {bundle}" if bundle else "")
    try:
        out = hdc.run("shell", f"ps -ef | grep {what} | grep -v grep; echo __done__", timeout=10)
    except subprocess.TimeoutExpired:
        raise Offline("hdc 超时")
    except OSError as e:
        raise Offline(f"找不到 hdc（{e}）")
    if "__done__" not in out:
        raise Offline(brief(out) or "hdc 没有回应")
    lines = [l.strip() for l in out.splitlines() if l.strip() and "__done__" not in l]
    return [l for l in lines if "SP_daemon" in l], (any(bundle in l and "SP_daemon" not in l for l in lines) if bundle else None)


def describe(ps_line):
    """An SP_daemon line of ps for the status line: its pid and arguments, any token hidden."""
    f = ps_line.split()
    pid = f[1] if len(f) > 1 and f[1].isdigit() else "?"
    k = ps_line.find("SP_daemon")
    return f"PID {pid}：{brief(ps_line[k:], 60)}"


# ---- what must not stay behind: the forward, the hdc shell (the daemon ends with it) ---------------------------------
def pid_alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, int(pid))      # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return bool(ok) and code.value == 259           # STILL_ACTIVE
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def is_hdc(pid):
    try:
        if os.name == "nt":
            r = subprocess.run(["tasklist", "/FI", f"PID eq {int(pid)}", "/FO", "CSV", "/NH"], stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=15, creationflags=NOWIN)
        else:
            r = subprocess.run(["ps", "-p", str(int(pid)), "-o", "comm="], stdin=subprocess.DEVNULL, capture_output=True, timeout=15)
        return b"hdc" in r.stdout.lower()
    except (OSError, ValueError, subprocess.SubprocessError):
        return False


def end_hdc_client(pid):
    """End the `hdc shell SP_daemon ...` process we started (the daemon on the phone ends with it)."""
    if pid and is_hdc(pid):
        try:
            os.kill(int(pid), signal.SIGTERM)           # Windows: TerminateProcess
        except OSError:
            pass


def remove_forward(hdc, port):
    """True when the forward tcp:<port> -> tcp:8284 is no longer in hdc's list."""
    rule = f"tcp:{port} tcp:{DEVICE_PORT}"
    for _ in range(2):
        try:
            hdc.run("fport", "rm", f"tcp:{port}", f"tcp:{DEVICE_PORT}", timeout=10)
            if rule not in hdc.run("fport", "ls", timeout=10):
                return True
        except (OSError, subprocess.SubprocessError):
            return False
    return False


def clean_up(info, stop=True):
    """What a killed process left: ask the session to stop (it may no longer answer), remove the forward, end the hdc shell."""
    hdc, port, token = Hdc(info.get("hdc") or []), info.get("port"), info.get("token")
    if stop and port and token:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=3)
            s.settimeout(0.5)
            s.sendall(f"stop::{token}".encode())
            got, t = b"", time.time()
            while b"stop::" not in got and time.time() - t < 4:
                try:
                    d = s.recv(65536)
                except socket.timeout:
                    continue
                if not d:
                    break
                got = (got + d)[-256:]
            s.close()
        except OSError:
            pass
    if port and hdc.base:
        remove_forward(hdc, port)
    end_hdc_client(info.get("pid"))
    try:
        os.remove(info.get("state") or "")
    except OSError:
        pass


def sweep(state_dir):
    """State files of processes that no longer run: remove their forward and hdc shell. Returns what was done, for the log."""
    done = []
    try:
        names = os.listdir(state_dir)
    except OSError:
        return done
    for name in names:
        m = STATE_FILE.match(name)
        if not m or (int(m.group(1)) != os.getpid() and pid_alive(int(m.group(1)))):
            continue
        path = os.path.join(state_dir, name)
        try:
            with open(path, encoding="utf-8") as f:
                info = json.load(f)
        except (OSError, ValueError):
            info = {}
        info["state"] = path
        clean_up(info, stop=False)
        done.append(f"removed what process {m.group(1)} left (forward tcp:{info.get('port')})")
    return done


class Guard:
    def __init__(self, hdc_base, state_dir=HERE):
        self.base, self.path, self.proc, self.mu = list(hdc_base), os.path.join(state_dir, f".sp_session-{os.getpid()}.json"), None, threading.Lock()

    def start(self):
        try:
            self.proc = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "--watchdog"], stdin=subprocess.PIPE,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NOWIN | NEWGROUP)
        except OSError:
            self.proc = None

    def _tell(self, message):
        with self.mu:
            if self.proc is not None and self.proc.poll() is None:
                try:
                    self.proc.stdin.write((json.dumps(message) + "\n").encode())
                    self.proc.stdin.flush()
                except (OSError, ValueError):
                    pass

    def arm(self, token, port, pid):
        info = {"hdc": self.base, "port": port, "pid": pid, "state": self.path}
        try:
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(info, f)                      # no token on disk
        except OSError:
            pass
        self._tell({"arm": dict(info, token=token)})

    def disarm(self):
        try:
            os.remove(self.path)
        except OSError:
            pass
        self._tell({"disarm": True})

    def stop(self):
        self.disarm()
        with self.mu:
            if self.proc is not None:
                try:
                    self.proc.stdin.close()
                    self.proc.wait(5)
                except (OSError, ValueError, subprocess.SubprocessError):
                    pass
                self.proc = None


def watchdog():
    """`sp_session.py --watchdog`: remembers the live session it is told about; when stdin ends while one is armed (the
    page's process was killed) it ends that session."""
    armed = None
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        try:
            m = json.loads(line)
        except ValueError:
            continue
        armed = m["arm"] if "arm" in m else None if "disarm" in m else armed
    if armed:
        clean_up(armed)


# ---- one session -----------------------------------------------------------------------------------------------------
class Session:
    def __init__(self, hdc, interval_ms=500, metrics=METRICS, on_sample=None, guard=None, cancel=None):
        self.hdc, self.interval, self.metrics = hdc, int(interval_ms), metrics
        self.on_sample, self.guard, self.cancel = on_sample or (lambda kv, t, first: None), guard, cancel or threading.Event()
        self.token = secrets.token_hex(16)
        self.daemon = self.sock = self.port = None
        self.mu, self.acks, self.alive, self.gen = threading.Lock(), [], False, 0
        self.started = self.last_alive = self.last_sample = 0.0
        self.samples, self.last_stamp, self.problem = 0, None, ""

    def _check(self):
        if self.cancel.is_set():
            raise Failed("停止中")

    def _reader(self, sock, gen):
        buf = ""
        while self.alive and gen == self.gen:
            try:
                d = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            buf += d.decode("utf-8", "replace")
            while True:             # control answers and whole sample objects; a TCP read can end anywhere
                a, i = ACK.search(buf), buf.find("{")
                if a and (i < 0 or a.start() < i):
                    with self.mu:
                        self.acks.append(a.group(0))
                    buf = buf[a.end():]
                    continue
                j = buf.find("}", i) if i >= 0 else -1
                if j < 0:
                    if i < 0 and len(buf) > 64:
                        buf = buf[-64:]             # nothing that begins a sample: keep only what may be half an answer
                    elif len(buf) > 1 << 20:
                        buf = ""
                    break
                obj, buf = buf[i:j + 1], buf[j + 1:]
                kv = {k: v.strip().strip('"') for k, v in FIELD.findall(obj)}
                stamp = kv.get("timestamp")
                if not kv or (stamp is not None and stamp == self.last_stamp):
                    continue
                self.last_stamp, self.last_sample = stamp, time.time()
                self.samples += 1
                try:
                    self.on_sample(kv, self.last_sample, self.samples == 1)
                except Exception as e:      # a problem in the page's code must not end the session
                    self.problem = f"sample handler: {type(e).__name__}: {e}"
        if gen == self.gen:
            self.alive = False

    def _command(self, cmd, want="", timeout=8.0):
        with self.mu:
            self.acks.clear()
        self.sock.sendall(cmd.encode())
        t = time.time()
        while want and time.time() - t < timeout:
            with self.mu:
                if self.acks:
                    return self.acks[0]
            if not self.alive:
                return "closed"
            time.sleep(0.02)
        return "timeout" if want else ""

    def _drop(self):
        self.alive = False
        self.gen += 1
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def start(self):
        self.daemon = subprocess.Popen(self.hdc.base + ["shell", f"SP_daemon -editorServer:{self.token}"], stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NOWIN)
        if self.guard:
            self.guard.arm(self.token, None, self.daemon.pid)
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        out = self.hdc.run("fport", f"tcp:{port}", f"tcp:{DEVICE_PORT}")
        if "OK" not in out:
            raise Failed(f"fport：{brief(out)}")
        self.port = port
        if self.guard:
            self.guard.arm(self.token, port, self.daemon.pid)
        deadline, answer = time.time() + 12, ""
        while True:                 # the daemon's TCP server needs a moment: a reset connection is tried again
            self._check()
            if self.daemon.poll() is not None:
                raise Failed("SP_daemon 没起来（hdc shell 已退出）")
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.sock.settimeout(1)
                self.alive = True
                threading.Thread(target=self._reader, args=(self.sock, self.gen), daemon=True).start()
                answer = self._command(f"init::{self.token} -SESSIONID 1 -INTERVAL {self.interval} {self.metrics}", "init::True")
            except OSError as e:
                answer = type(e).__name__
            if answer == "init::True":
                break
            self._drop()
            if answer.startswith("init::") or answer == "TOKEN_CHECK_FAILED" or time.time() > deadline:
                raise Failed(f"init：{answer}")
            time.sleep(0.4)
        answer = self._command(f"start::{self.token}", "start::True", 6)
        if answer != "start::True":
            raise Failed(f"start：{answer}")
        self.started = self.last_alive = self.last_sample = time.time()

    def tick(self):
        """Call about twice a second: the keepalive every 60 s (it has no answer). False once the session has ended."""
        if self.alive and time.time() - self.last_alive >= KEEPALIVE_S:
            self.last_alive = time.time()
            try:
                self._command(f"keepalive::{self.token}")
            except OSError:
                self.alive = False
        return self.alive

    def close(self):
        """Session stop, forward removed, hdc shell (and with it the daemon) ended. Returns a line for the log."""
        note = []
        if self.sock is not None and self.alive:
            try:
                note.append("stop " + self._command(f"stop::{self.token}", "stop::True", 6).replace("stop::", ""))
            except OSError as e:
                note.append(f"stop {type(e).__name__}")
        self._drop()
        if self.port:
            note.append("forward removed" if remove_forward(self.hdc, self.port) else "forward NOT removed")
            self.port = None
        if self.daemon is not None:
            self.daemon.terminate()
            try:
                self.daemon.wait(5)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
            self.daemon = None
            note.append("hdc shell ended")
        if self.guard:
            self.guard.disarm()
        return ", ".join(note)


# ---- the keeper ------------------------------------------------------------------------------------------------------
class Sampler:
    def __init__(self, hdc_base, bundle="", interval_ms=500, metrics=METRICS, on_sample=None, on_state=None, log=None, state_dir=HERE):
        self.hdc, self.bundle, self.interval, self.metrics = Hdc(hdc_base), bundle, int(interval_ms), metrics
        self.on_sample, self.on_state, self.log = on_sample or (lambda kv, t, first: None), on_state or (lambda ok, msg: None), log or (lambda m: None)
        self.guard, self.state_dir = Guard(hdc_base, state_dir), state_dir
        self.cancel, self.thread, self.mu = threading.Event(), None, threading.Lock()
        self.st = {"state": "starting", "msg": "正在建立 SmartPerf 会话…", "since": time.time(), "game": None, "sessions": 0, "samples": 0}
        self.recent = deque(maxlen=40)      # arrival times of the latest samples: the achieved rate

    def _set(self, state, msg):
        with self.mu:
            if self.st["state"] != state:
                self.st["since"] = time.time()
            self.st["state"], self.st["msg"] = state, msg
        self.on_state(state == "sampling", msg)

    def _sample(self, kv, t, first):
        with self.mu:
            self.st["samples"] += 1
            self.recent.append(t)
        self.on_sample(kv, t, first)

    def status(self):
        """state: starting / sampling / busy (another SP_daemon) / offline (no phone) / error / stopped."""
        with self.mu:
            st, r = dict(self.st), list(self.recent)
        now = time.time()
        r = [t for t in r if now - t < 25]
        st["rate"] = round((len(r) - 1) / (r[-1] - r[0]), 2) if len(r) >= 3 and r[-1] > r[0] else None
        st["interval_ms"] = self.interval
        return st

    def start(self):
        self.thread = threading.Thread(target=self._run, name="smartperf", daemon=True)
        self.thread.start()

    def stop(self, timeout=30):
        """Clean end: the running session is closed by its own thread; then the phone is asked once more."""
        self.cancel.set()
        if self.thread is not None:
            self.thread.join(timeout)
            if self.thread.is_alive():      # an hdc call that does not return: the watchdog stays armed and ends the session
                self.log("SmartPerf: the sampler did not end in time; the watchdog ends the session when this process exits")
                return
        self.guard.stop()
        try:
            for _ in range(6):
                left, _game = probe(self.hdc)
                if not left:
                    break
                time.sleep(0.5)
            self.log("SmartPerf: no SP_daemon left on the phone" if not left else
                     "SmartPerf: an SP_daemon is on the phone (not necessarily ours): " + "; ".join(describe(l) for l in left))
        except Offline as e:
            self.log(f"SmartPerf: the phone cannot be asked ({e})")

    def _run(self):
        for line in sweep(self.state_dir):
            self.log("SmartPerf: " + line)
        self.guard.start()
        short = 0                   # sessions in a row that ended within seconds: something keeps ending them
        while not self.cancel.is_set():
            sess, wait, began = None, RETRY_S, time.time()
            try:
                self._set("starting", "正在建立 SmartPerf 会话…")
                others, game = probe(self.hdc, self.bundle)
                with self.mu:
                    self.st["game"] = game
                if others:
                    raise Busy("；".join(describe(l) for l in others))
                sess = Session(self.hdc, self.interval, self.metrics, self._sample, self.guard, self.cancel)
                sess.start()
                with self.mu:
                    self.st["sessions"] += 1
                self.log(f"SmartPerf: session started (monitoring only, -INTERVAL {self.interval} {self.metrics})")
                self._set("sampling", f"SmartPerf 连续会话（仅监控，不锁频）；请求间隔 {self.interval} ms")
                checked = time.time()
                while not self.cancel.wait(0.5):
                    if not sess.tick():
                        raise Failed("会话断了（SP_daemon 结束了，或手机断开）")
                    if time.time() - sess.last_sample > STALL_S:
                        raise Failed(f"{STALL_S:.0f} 秒没有新采样")
                    if time.time() - checked >= GAME_CHECK_S:
                        checked = time.time()
                        try:
                            _others, game = probe(self.hdc, self.bundle)
                            with self.mu:
                                self.st["game"] = game
                        except Offline:
                            pass    # the session itself tells when the phone is gone
            except Busy as e:
                self._set("busy", f"手机上已有别的 SP_daemon 在跑（{e}）；SmartPerf 同时只能有一个，{wait:.0f} 秒后重试")
            except Offline as e:
                with self.mu:
                    self.st["game"] = None
                self._set("offline", f"手机没连上（{e}）；{wait:.0f} 秒后重试")
            except Exception as e:      # Failed, a socket or hdc error: say it and try again
                if self.cancel.is_set():
                    break
                lived = time.time() - (sess.started or began) if sess is not None and sess.started else 0.0
                short = short + 1 if lived < 20 else 0
                wait = min(RETRY_S * (2 ** max(short - 1, 0)), 40.0)
                why = str(e) if isinstance(e, Failed) else f"{type(e).__name__}: {brief(e)}"
                self._set("error", f"SmartPerf 会话中断：{why}" + ("（接连被打断，多半有别的 SP_daemon 在用）" if short >= 2 else "") + f"；{wait:.0f} 秒后重试")
                self.log(f"SmartPerf: session ended: {why}")
            finally:
                if sess is not None:
                    note = sess.close()
                    self.log(f"SmartPerf: session closed ({note}); {sess.samples} samples" + (f"; {sess.problem}" if sess.problem else ""))
            if self.cancel.wait(wait):
                break
        self._set("stopped", "已停止")


if __name__ == "__main__":
    if sys.argv[1:] == ["--watchdog"]:
        watchdog()
    else:
        sys.exit("this is perfdash.py's sampler module; run perfdash.py")
