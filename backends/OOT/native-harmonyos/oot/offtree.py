import json, sys, subprocess, re
from pathlib import Path
d=json.load(open(sys.argv[1],encoding='utf-8',errors='replace'))
sym=d['SymbolMap']; files=d['symbolsFileList']; tn=d['threadNameMap']
tid=int(sys.argv[2]); thr=float(sys.argv[3]); maxd=int(sys.argv[4])
SYM=r"C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\llvm-symbolizer.exe"
OBJ=str(Path(__file__).resolve().parents[2]/"artifacts"/"harmonyos-native"/"lib-vk"/"libtotk_ohos.so")   # this backend's build
p=d['recordSampleInfo'][0]['processes'][0]
t=[x for x in p['threads'] if x['tid']==tid][0]
tot=t['eventCount']
nodes=[]
def collect(n,lvl):
    v=n.get('subEvents',0)
    if v<thr/100*tot or lvl>maxd: return
    nodes.append((lvl,v,n.get('selfEvents',0),n['symbol']))
    for c in sorted(n.get('callStack',[]),key=lambda k:-k.get('subEvents',0)): collect(c,lvl+1)
for c in sorted(t['CallOrder'].get('callStack',[]),key=lambda k:-k.get('subEvents',0)): collect(c,0)
addrs=set()
def raw(i):
    s=sym.get(str(i)); f=files[s['file']].split('/')[-1]; return f,s['symbol']
for lvl,v,se,s in nodes:
    f,n=raw(s)
    m=re.search(r'libtotk_ohos\.so\+(0x[0-9a-f]+)$',n)
    if m: addrs.add(m.group(1))
res={}
if addrs:
    al=sorted(addrs)
    out=subprocess.run([SYM,'--obj='+OBJ,'--functions=short']+al,capture_output=True,text=True).stdout.strip().split('\n\n')
    for a,o in zip(al,out):
        ls=o.split('\n'); fn=ls[0]; loc=ls[1].replace(chr(92),'/').split('/')[-1] if len(ls)>1 else ''
        res[a]=fn+' '+loc
print('thread',tid,tn.get(str(tid)),'wall ms',round(tot/1e6))
prev=None
for lvl,v,se,s in nodes:
    f,n=raw(s)
    m=re.search(r'libtotk_ohos\.so\+(0x[0-9a-f]+)$',n)
    nm=res[m.group(1)] if m else (f.replace('.so','')+'!'+n.split('/')[-1][-40:])
    if nm=='libtotk_guest!.plt' and prev=='libtotk_guest!.plt' and se<0.01*tot: continue
    prev=nm
    print('%s%5.1f%% (self %4.1f%%) %s'%(' '*min(lvl,30),100*v/tot,100*se/tot,nm))
