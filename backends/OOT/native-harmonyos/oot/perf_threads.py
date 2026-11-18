"""Threads of a `hiperf report --json` (event hw-cpu-cycles): time on CPU (samples), cycles, and the frequency the thread
really ran at (cycles / time on CPU). usage: python perf_threads.py <report.json> <sampling Hz> [<rows>]"""
import json, sys

d = json.load(open(sys.argv[1], encoding='utf-8', errors='replace'))
hz = float(sys.argv[2]); rows = int(sys.argv[3]) if len(sys.argv) > 3 else 8
tn = d['threadNameMap']
for p in d['recordSampleInfo'][0]['processes']:
    r = sorted(((t['eventCount'], t['sampleCount'], t['tid'], tn.get(str(t['tid']), '?')) for t in p['threads']), reverse=True)
    for ev, sc, tid, name in r[:rows]:
        print(f'  {name:16s} tid {tid}: on CPU {sc / hz:6.2f} s, {ev / 1e6:8.0f} M cycles, ran at {ev / (sc / hz) / 1e6 if sc else 0:6.0f} MHz')
