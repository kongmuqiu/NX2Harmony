"""The app's native crash records (files/native-faults.json, written by entry's CrashDiagnostics.ets), one line each:
time, pid, signal, thread and the first frames. usage: python faults.py <native-faults.json> [<frames>]
Pull the file first: hdc file recv -b <bundle> /data/storage/el2/base/haps/entry/files/native-faults.json <local>"""
import datetime, json, sys

d = json.load(open(sys.argv[1], encoding='utf-8', errors='replace'))
n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
recs = d if isinstance(d, list) else d.get('records', [])
for r in recs:
    if not isinstance(r, dict):
        continue
    try:
        p = json.loads(r.get('paramsJson') or '{}')
    except ValueError:
        p = {}
    ex = p.get('exception', {}) if isinstance(p, dict) else {}
    fr = ex.get('frames', []) if isinstance(ex, dict) else []
    t = datetime.datetime.fromtimestamp(r.get('timestamp', 0) / 1000)
    names = ' < '.join(f"{f.get('file', '?').split('/')[-1].replace('libtotk_', '').replace('.so', '')}+{int(f.get('pc', '0'), 16):#x}" for f in fr[:n])
    print(t.strftime('%m-%d %H:%M:%S'), 'pid', r.get('pid'), 'signal', p.get('signal') if isinstance(p, dict) else None,
          'thread', ex.get('thread_name') if isinstance(ex, dict) else None, '|', names)
