import json, sys, collections
d=json.load(open(sys.argv[1],encoding='utf-8',errors='replace'))
sym=d['SymbolMap']; files=d['symbolsFileList']; tn=d['threadNameMap']
def name(i):
    s=sym.get(str(i));
    if not s: return '?'
    f=files[s['file']].split('/')[-1]
    return f+'!'+s['symbol'].split('/')[-1][:60]
p=d['recordSampleInfo'][0]['processes'][0]
want=sys.argv[2].split(',')
depth=int(sys.argv[3]) if len(sys.argv)>3 else 3
for t in p['threads']:
    nm=tn.get(str(t['tid']),'?')
    if nm not in want: continue
    tot=t['eventCount']
    print('=====',nm,t['tid'],'total ms',round(tot/1e6),'samples',t['sampleCount'])
    co=t['CalledOrder']   # bottom-up: children = callers
    kids=co.get('callStack',[])
    kids=sorted(kids,key=lambda k:-(k.get('selfEvents',0)+k.get('subEvents',0)))
    def walk(n,lvl,parent):
        v=n.get('subEvents',0) if lvl else (n.get('subEvents',0))
        if v < 0.012*tot: return
        print('  '*lvl+'%5.1f%% %s'%(100*v/tot,name(n['symbol'])))
        if lvl<depth:
            for c in sorted(n.get('callStack',[]),key=lambda k:-k.get('subEvents',0))[:5]: walk(c,lvl+1,v)
    for k in kids[:22]: walk(k,0,tot)
