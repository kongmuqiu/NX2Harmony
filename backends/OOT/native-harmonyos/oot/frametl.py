"""The frame timeline rows of a runtime log ([frame-tl late], TOTK_FRAME_TIMELINE=1) as a readable picture of who waits
for whom. Times are milliseconds after the previous present of the game.

usage: python frametl.py <log> [<first frame> [<frames>]]     (default: the last 6 rows of the log)
  per frame: the game's interval, the main thread's wake of the walking thread, each walk [begin, end, cpu], each slot
  (seal -> QueueSubmit -> GPU done, GPU time), and a 100-column chart of the interval:
    W  the walking thread is replaying          g  the GPU has submitted work it has not finished
    .  neither
  then the averages over the rows printed.
"""
import re, sys

ROW = re.compile(r'\[frame-tl late\] frame=(\d+) target=([\d.]+) game_iv=([\d.nan]+).*?wake_gap=([\d.nan]+) dispatch=([\d.]+) walk=([\d.]+) offcpu=([\d.]+) join=([\d.]+) '
                 r'submit_latency=([\d.]+) queue_cpu=([\d.]+) gpu=([\d.]+) gpu_tail=([\d.]+) gpu_backlog_or_observe=([\d.]+) gpu_complete=(\d) acquire=([\d.]+).*?'
                 r'relative_prev_game_ms\([^)]*\)=\[([^\]]*)\] walks=(.*?) slots=(.*?) draws=(\d+) passes=(\d+)')


def num(x):
    try:
        return float(x)
    except ValueError:
        return float('nan')


def rows(path):
    out = []
    for l in open(path, encoding='utf-8', errors='replace'):
        m = ROW.search(l)
        if not m:
            continue
        g = m.groups()
        rel = [num(x) for x in g[15].split(',')]
        walks = [tuple(num(x.replace('cpu=', '')) for x in w.split(',')) for w in re.findall(r'\[([^\]]*)\]', g[16])]
        slots = []
        for s in re.findall(r'\[([^\]]*)\]', g[17]):
            t, rest = s.split(':', 1)
            v = [num(x.replace('gpu=', '')) for x in rest.split(',')]
            slots.append((int(t), *v))   # ticket, seal, queue begin, queue end, gpu done, gpu ms
        out.append(dict(frame=int(g[0]), iv=num(g[2]), wake_gap=num(g[3]), dispatch=num(g[4]), walk=num(g[5]), offcpu=num(g[6]), join=num(g[7]),
                        submit_latency=num(g[8]), queue_cpu=num(g[9]), gpu=num(g[10]), gpu_tail=num(g[11]), acquire=num(g[14]), rel=rel, walks=walks,
                        slots=slots, draws=int(g[18]), passes=int(g[19])))
    return out


def chart(r, cols=100):
    iv = r['iv']
    if not iv == iv or iv <= 0:
        return ''
    # the interval is [0, iv] after the previous present; this frame's own events may lie before 0 (its walks began in the
    # previous interval) and after iv: the chart shows [lo, hi] = the frame's first walk .. its last GPU completion
    ev = [w[0] for w in r['walks']] + [s[4] for s in r['slots'] if s[4] == s[4]] + [w[1] for w in r['walks']]
    lo, hi = min(ev + [0.0]), max(ev + [iv])
    step = (hi - lo) / cols
    line = []
    for c in range(cols):
        a, b = lo + c * step, lo + (c + 1) * step
        walking = any(w[0] < b and w[1] > a for w in r['walks'])
        # the GPU has work of this frame: from a slot's QueueSubmit to its completion
        gpu = any(s[3] == s[3] and s[4] == s[4] and s[3] < b and s[4] > a for s in r['slots'])
        line.append('B' if walking and gpu else 'W' if walking else 'g' if gpu else '.')
    marks = [' '] * cols
    for t, ch in ((0.0, '|'), (iv, '|')):
        k = int((t - lo) / step) if step else 0
        if 0 <= k < cols:
            marks[k] = ch
    return f'    {lo:8.1f} ms {"".join(line)} {hi:.1f} ms\n                {"".join(marks)}   (| the previous present and this one; B = walking and GPU busy)'


def main():
    rs = rows(sys.argv[1])
    if not rs:
        print('no [frame-tl late] rows'); return
    if len(sys.argv) > 2:
        first, n = int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 6
        sel = [r for r in rs if first <= r['frame'] < first + n]
    else:
        sel = rs[-6:]
    for r in sel:
        wake, ready, wb, we, qb, qe, gpu_done, acq_in, acq_out = r['rel'][:9]
        print(f'frame {r["frame"]}: interval {r["iv"]:.1f} ms; {r["draws"]} draws, {r["passes"]} passes; main woke the walker at {wake:+.1f} (ready {ready:+.1f}); '
              f'walks {wb:+.1f} .. {we:+.1f} (wall {r["walk"]:.1f}, off-CPU {r["offcpu"]:.1f}, waits {r["join"]:.1f}); first / last QueueSubmit {qb:+.1f} / {qe:+.1f}; '
              f'last GPU done {gpu_done:+.1f}; GPU {r["gpu"]:.1f} ms in {len(r["slots"])} slots; acquire {acq_in:+.1f} .. {acq_out:+.1f}')
        print('    walks: ' + ' '.join(f'[{a:+.1f},{b:+.1f} cpu {c:.1f}]' for a, b, c in r['walks']))
        print('    slots (seal > submit > done, GPU ms): ' + ' '.join(f'[{s[1]:+.0f}>{s[3]:+.0f}>{s[4]:+.0f} {s[5]:.1f}]' for s in r['slots']))
        print(chart(r))
    n = len(sel)
    avg = lambda k: sum(r[k] for r in sel if r[k] == r[k]) / max(1, sum(1 for r in sel if r[k] == r[k]))
    print(f'averages over {n} frames: interval {avg("iv"):.1f} ms, walk wall {avg("walk"):.1f} (off-CPU {avg("offcpu"):.1f}, waits {avg("join"):.1f}), GPU {avg("gpu"):.1f}, '
          f'GPU tail after the last submit {avg("gpu_tail"):.1f}, seal-to-submit {avg("submit_latency"):.1f}, acquire {avg("acquire"):.1f}')
    rel = lambda i: sum(r['rel'][i] for r in sel if r['rel'][i] == r['rel'][i]) / max(1, sum(1 for r in sel if r['rel'][i] == r['rel'][i]))
    print(f'  after the previous present (ms): main wakes the walker {rel(0):+.1f}, first walk {rel(2):+.1f}, last walk ends {rel(3):+.1f}, first QueueSubmit {rel(4):+.1f}, '
          f'last QueueSubmit {rel(5):+.1f}, last GPU done {rel(6):+.1f}, acquire {rel(7):+.1f} .. {rel(8):+.1f}')


main()
