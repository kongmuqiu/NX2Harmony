"""Read the [img] lines of a run's log (TOTK_OHOS_IMG_DUMP=1: every live image of at least 1 MiB, each time live image
memory grows by 256 MiB and each 300 frames) and say what the images are.

    python native-harmonyos/oot/img_dump.py artifacts/runs/oot-a81.log [round]

For each dump: the totals by class, the images by NVN format and size (count, MiB, how many are idle, how old), and the
images whose guest storage overlaps another live image's (on the console those are one piece of memory)."""
import collections
import re
import sys

LINE = re.compile(r'\[img\] r(\d+) (\S+) fmt=(\S+) (\d+)x(\d+)x(\d+) lv=(\d+) la=(\d+) mib=([\d.]+) refs=(\d+) wb=(\d+) gpuw=(\d+) mem=(\S+) size=(\S+)(?: ser=(\d+) age=(\d+) pf=(\S+) tf=(\S+))?(?: idle=(\d+))?')
TOTAL = re.compile(r'\[img\] r(\d+) at submission (\d+): (.*)')


def main():
    path = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else None
    rounds = collections.defaultdict(list); totals = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            r = int(m.group(1))
            rounds[r].append(dict(cls=m.group(2), fmt=int(m.group(3), 16), w=int(m.group(4)), h=int(m.group(5)), d=int(m.group(6)), lv=int(m.group(7)), la=int(m.group(8)),
                                  mib=float(m.group(9)), refs=int(m.group(10)), wb=int(m.group(11)), gpuw=int(m.group(12)), mem=int(m.group(13), 16), size=int(m.group(14), 16),
                                  age=int(m.group(16)) if m.group(16) else None, pf=int(m.group(17), 16) if m.group(17) else None, idle=int(m.group(19)) if m.group(19) else None))
            continue
        m = TOTAL.search(line)
        if m:
            totals[int(m.group(1))] = (int(m.group(2)), m.group(3))
    for r in sorted(rounds):
        sub, text = totals.get(r, (0, '?'))
        listed = sum(i['mib'] for i in rounds[r])
        print(f'round {r} at submission {sub}: {text}; listed (1 MiB and up) {len(rounds[r])} images, {listed:.0f} MiB')
    if not rounds:
        return
    r = want if want is not None else max(rounds)
    imgs = rounds[r]
    print(f'\n== round {r}: by class and pool flags')
    by = collections.defaultdict(lambda: [0, 0.0])
    for i in imgs:
        k = (i['cls'], i['pf']); by[k][0] += 1; by[k][1] += i['mib']
    for k, v in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f'  {k[0]:8} pool flags {k[1] if k[1] is None else hex(k[1])}: {v[0]} images, {v[1]:.0f} MiB')
    print(f'\n== round {r}: by format and size (count, MiB, idle ones, last looked up min..max submissions ago)')
    by = collections.defaultdict(list)
    for i in imgs:
        by[(i['fmt'], i['w'], i['h'], i['d'], i['lv'], i['la'])].append(i)
    rows = sorted(by.items(), key=lambda kv: -sum(x['mib'] for x in kv[1]))
    for k, v in rows[:45]:
        ages = [x['age'] for x in v if x['age'] is not None]
        idle = sum(1 for x in v if x['cls'] == 'idle')
        gpuw = sum(1 for x in v if x['gpuw'])
        print(f'  fmt {k[0]:#x} {k[1]}x{k[2]}x{k[3]} lv {k[4]} la {k[5]}: {len(v)} images, {sum(x["mib"] for x in v):.0f} MiB; idle {idle}, GPU-written {gpuw}; '
              f'age {min(ages) if ages else "-"}..{max(ages) if ages else "-"}; classes {sorted(set(x["cls"] for x in v))}')
    # by format only
    print(f'\n== round {r}: by format')
    byf = collections.defaultdict(lambda: [0, 0.0, 0, 0.0])
    for i in imgs:
        f = byf[i['fmt']]; f[0] += 1; f[1] += i['mib']
        if i['age'] is not None and i['age'] > 300 or i['cls'] == 'idle':
            f[2] += 1; f[3] += i['mib']
    for f, v in sorted(byf.items(), key=lambda kv: -kv[1][1]):
        print(f'  fmt {f:#x}: {v[0]} images {v[1]:.0f} MiB; not looked up for 300 submissions or idle: {v[2]} images {v[3]:.0f} MiB')
    # storage overlap: sweep by storage address
    print(f'\n== round {r}: images whose guest storage overlaps another live image (1 MiB and up)')
    s = sorted(imgs, key=lambda i: i['mem'])
    groups = []; cur = []; end = 0
    for i in s:
        if cur and i['mem'] < end:
            cur.append(i); end = max(end, i['mem'] + i['size'])
        else:
            if len(cur) > 1:
                groups.append((cur, end))
            cur = [i]; end = i['mem'] + i['size']
    if len(cur) > 1:
        groups.append((cur, end))
    over = sum(sum(x['mib'] for x in g) for g, _ in groups)
    span = sum((e - g[0]['mem']) / 1048576 for g, e in groups)
    print(f'  {len(groups)} groups, {sum(len(g) for g, _ in groups)} images, {over:.0f} MiB of images over {span:.0f} MiB of guest storage')
    for g, e in sorted(groups, key=lambda ge: -sum(x['mib'] for x in ge[0]))[:25]:
        desc = collections.Counter((x['fmt'], x['w'], x['h'], x['cls']) for x in g)
        text = ', '.join(f'{n}x {k[1]}x{k[2]} {k[0]:#x} {k[3]}' for k, n in desc.most_common(6))
        print(f'  storage {g[0]["mem"]:#x}+{(e - g[0]["mem"]) / 1048576:.1f}M pool flags {g[0]["pf"] if g[0]["pf"] is None else hex(g[0]["pf"])}: {len(g)} images {sum(x["mib"] for x in g):.0f} MiB: {text}')


main()
