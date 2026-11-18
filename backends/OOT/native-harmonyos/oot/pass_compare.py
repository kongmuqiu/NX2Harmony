"""Compare runs' per-pass GPU profiles (TOTK_VK_PASS_PROFILE=N) kernel by kernel, scaled to one GPU clock.

usage: python native-harmonyos/oot/pass_compare.py <log> <log> ... [--last 30] [--top 14] [--ref compute]
Each log's last --last profiled frames are averaged. Per kernel: dispatches, ms a frame, us a workgroup. The reference is
work the change under test does not touch ('compute': the game's own compute shaders; 'main': the 854x480 passes); every
other log's times are scaled by (reference time of the first log / its own) so a different GPU clock or heat does not show
as a change. Printed: totals by kind, then the top kernels of the first log in each log, raw and scaled."""
import argparse
import re
import sys
from collections import defaultdict

FRAME = re.compile(r'pass profile frame (\d+): (.*)$')
DISP = re.compile(r'dispatches of ([0-9a-f]{16}) \(([^,]+), .*?\): (\d+), ([\d.]+) ms, about (\d+) workgroups \(([\d.]+) us each\)')
KIND = re.compile(r'dispatches by kind: mesh kernels (\d+), ([\d.]+) ms; task kernels (\d+), ([\d.]+) ms; compute (\d+), ([\d.]+) ms')
SPAN = re.compile(r'GPU span ([\d.]+) ms; (\d+) passes ([\d.]+) ms, (\d+) dispatches ([\d.]+) ms')
MAIN = re.compile(r'^854x480 color 1 \(fmt 122\) depth fmt 126: (\d+) passes, (\d+) draws, ([\d.]+) ms')


def frames(path, last):
    by = defaultdict(list)
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = FRAME.search(line)
            if m:
                by[int(m.group(1))].append(m.group(2))
    keys = sorted(k for k in by if k > 0)
    return [(k, by[k]) for k in keys[-last:]]


def summarise(path, last):
    fr = frames(path, last)
    if not fr:
        sys.exit(f'{path}: no profiled frames (TOTK_VK_PASS_PROFILE)')
    k_ms, k_n, k_wg, k_us, k_kind = defaultdict(float), defaultdict(float), defaultdict(float), defaultdict(list), {}
    tot = defaultdict(float)
    for _, lines in fr:
        for l in lines:
            m = DISP.search(l)
            if m:
                key = m.group(1)
                k_kind[key] = m.group(2)
                k_n[key] += int(m.group(3)); k_ms[key] += float(m.group(4)); k_wg[key] += int(m.group(5))
                k_us[key].append(float(m.group(6)))
                continue
            m = KIND.search(l)
            if m:
                tot['mesh'] += float(m.group(2)); tot['task'] += float(m.group(4)); tot['compute'] += float(m.group(6))
                continue
            m = SPAN.search(l)
            if m:
                tot['span'] += float(m.group(1)); tot['passes'] += float(m.group(3)); tot['dispatches'] += float(m.group(5))
                continue
            m = MAIN.match(l)
            if m:
                tot['main'] += float(m.group(3)); tot['main_draws'] += int(m.group(2))
    n = len(fr)
    kern = {k: {'kind': k_kind[k], 'n': k_n[k] / n, 'ms': k_ms[k] / n,
                'us': (k_ms[k] * 1000 / k_wg[k]) if k_wg[k] else 0.0} for k in k_ms}
    return {'path': path, 'frames': n, 'first': fr[0][0], 'last': fr[-1][0], 'tot': {k: v / n for k, v in tot.items()}, 'kern': kern}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+'); ap.add_argument('--last', type=int, default=30); ap.add_argument('--top', type=int, default=14)
    ap.add_argument('--ref', default='compute', choices=('compute', 'main'))
    a = ap.parse_args()
    runs = [summarise(p, a.last) for p in a.logs]
    ref0 = runs[0]['tot'].get(a.ref, 0.0)
    for r in runs:
        r['scale'] = (ref0 / r['tot'][a.ref]) if r['tot'].get(a.ref) else 1.0
    name = lambda p: re.sub(r'.*[\\/]', '', p).replace('.log', '')
    print(f'reference: {a.ref} (scaled to the first log)')
    for r in runs:
        t = r['tot']
        print(f'{name(r["path"]):>18}: frames {r["first"]}..{r["last"]} ({r["frames"]}), scale {r["scale"]:.3f}; span {t.get("span", 0):.2f} ms, '
              f'main 854x480 {t.get("main", 0):.2f} ms ({t.get("main_draws", 0):.0f} draws), mesh {t.get("mesh", 0):.2f}, task {t.get("task", 0):.2f}, '
              f'compute {t.get("compute", 0):.2f}; scaled mesh {t.get("mesh", 0) * r["scale"]:.2f}, task {t.get("task", 0) * r["scale"]:.2f}')
    base = runs[0]['kern']
    top = sorted((k for k in base if base[k]['kind'] != 'compute'), key=lambda k: -base[k]['ms'])[:a.top]
    print(f'\n{"kernel":<18}{"kind":<13}' + ''.join(f'{name(r["path"])[-16:]:>34}' for r in runs))
    print(' ' * 31 + ''.join(f'{"disp  ms/frame  us/wg  scaled us":>34}' for _ in runs))
    for k in top:
        row = f'{k:<18}{base[k]["kind"]:<13}'
        for r in runs:
            x = r['kern'].get(k)
            row += f'{x["n"]:>8.1f}{x["ms"]:>8.2f}{x["us"]:>8.2f}{x["us"] * r["scale"]:>10.2f}' if x else f'{"-":>34}'
        print(row)


if __name__ == '__main__':
    main()
