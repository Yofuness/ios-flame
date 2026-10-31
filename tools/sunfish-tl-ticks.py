#!/usr/bin/env python3
# Swap-to-swap in ticks and per-frame CPU/GPU split from SWEET TL records (sweettrace=1), run from port/:
#   tools/sunfish-tl-ticks.py BENCH_TAG [SKIP_S] [DUR_S]   (clips to the full-motion part of a `=== bench TAG` window)
import sys,bisect,importlib.util
spec=importlib.util.spec_from_file_location('t','tools/sunfish-timeline.py');t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
tag=sys.argv[1]; skip=float(sys.argv[2]) if len(sys.argv)>2 else 5; dur=float(sys.argv[3]) if len(sys.argv)>3 else 10
recs,clocks=t.load('../logs/usb-live.log',f'bench {tag} start',f'bench {tag} end')
H=19.2e6; ms=lambda x:x*1000/H
ck=sorted(clocks)
def cp2m(cp):
    i=min(range(len(ck)),key=lambda j:abs(ck[j][1]-cp)); m,c=ck[i]; return m+cp-c
ev={k:sorted(r for r in recs if r[0]==k) for k in range(1,8)}
sw=[r[1] for r in ev[1]]; tk=[r[1] for r in ev[2]]
a=sw[0]+skip*H; b=a+dur*H
swin=[x for x in sw if a<=x<=b]
def P(xs,p): xs=sorted(xs); return xs[min(len(xs)-1,int(len(xs)*p))] if xs else float('nan')
def line(n,xs): print(f'  {n:34s} n={len(xs)} p50 {P(xs,.5):.2f} p95 {P(xs,.95):.2f}')
print(tag,'swaps in window',len(swin),'rate',len(swin)/dur)
# ticks between swaps
h={}
iv=[]
for x,y in zip(swin,swin[1:]):
    n=bisect.bisect_right(tk,y)-bisect.bisect_right(tk,x); k=min(n,4); h[k]=h.get(k,0)+1; iv.append(ms(y-x))
N=len(iv); print('  ticks between swaps:',{k:f'{v} ({100*v/N:.0f}%)' for k,v in sorted(h.items())},'(0 ticks:',h.get(0,0),')')
line('swap-to-swap ms',iv)
spans={(r[2],r[3]):(cp2m(r[1]),cp2m(r[4]),r[3]) for r in ev[7]}
subs=[r for r in ev[6] if r[3]!=0]; sub_t=[r[1] for r in subs]
pb=[r[1] for r in ev[4]]
cpu=[];ca=[];gdur=[];lat=[];g2p=[];fit=[];over=[];ontick=[];ptick=[];p2t=[]
for x,y in zip(sw,sw[1:]):
    if not(a<=y<=b): continue
    i=bisect.bisect_right(sub_t,x); j=bisect.bisect_right(sub_t,y)
    ss=[s for s in subs[i:j] if (s[2],s[3]) in spans]
    if not ss: continue
    t1=ss[0][1]; k=bisect.bisect_right(tk,x)
    if k>=len(tk) or tk[k]>t1: continue
    c=ms(t1-tk[k]); gend=max(spans[(s[2],s[3])][1] for s in ss)
    gd=sum(ms(spans[(s[2],s[3])][1]-spans[(s[2],s[3])][0]) for s in ss)
    cpu.append(c); ca.append(ms(y-t1)); gdur.append(gd); lat.append(ms(gend-ss[0][1]))
    T=c+gd; fit.append(T)
    if T>16.7: over.append(T-16.7)
    q=bisect.bisect_left(pb,max(gend,y))
    if q<len(pb):
        g2p.append(ms(pb[q]-gend))
        nt=bisect.bisect_right(tk,gend)  # first tick after GPU end
        if nt<len(tk): ontick.append(1 if ms(pb[q]-tk[nt])<2 else 0)
line('tick->first submit (CPU)',cpu); line('first submit->swap call',ca); line('GPU time (sum of frame spans)',gdur)
line('first submit->last GPU end (latency)',lat); line('GPU end->present call',g2p); line('CPU + GPU time',fit)
n=len(fit); print(f'  frames {n}; CPU+GPU<=16.7: {100*(n-len(over))/n:.0f}%; over by p50 {P(over,.5):.2f} ms (n={len(over)})')
print(f'  present call within 2 ms of first tick after GPU end: {100*sum(ontick)/len(ontick):.0f}% (n={len(ontick)})')
ph=[x for x in pb if a<=x<=b]; tt=[x for x in tk if a<=x<=b]
print(f'  ticks {len(tt)} presents {len(ph)} swaps {len(swin)}; present per tick {len(ph)/len(tt):.2f}')
