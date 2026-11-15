# Replays a trace (trace.csv, -stats with the genlock): the real arrivals against the core's real
# pickups, with a sequential decode model d = A + B x bytes estimated from the observed repeats.
# Usage: python3 genlock_replay.py trace.csv
import sys
# Replay: real arrivals (w, size, v, real target) vs the real pickup grid, moved by (new target - real
# target) as the genlock would move the core's phase; sequential decode d = B * bytes.
import bisect
F=[];E=[]
for line in open(sys.argv[1]):
    p=line.strip().split(',')
    if p[0]=='F':
        w,by,v,lk,q,D=float(p[1]),int(p[4]),float(p[14]),int(p[12]),float(p[9]),float(p[10])
        F.append((w,by,v,lk,q,D))
    elif p[0]=='E': E.append((float(p[1]),float(p[8])))
P=16666.7
evt=[s-P+eo for s,eo in E][1:]
F=[f for f in F if f[2]>0 and f[3]==1]        # locked part only
def run(B, Dt, rel_fn):
    # the core's pickups, moved so that each picture's pickup is the first after v + Dt + q + 0.5
    # instead of v + D + q + 0.5 (real): shift = Dt - D (real D was 15 ms)
    writes=[]; picks=[]
    for w,by,v,lk,q,D in F:
        shift=Dt-D
        tr=v+D+q+500                             # real schedule point
        i=bisect.bisect_left(evt,tr)
        if i<1 or i>=len(evt): continue
        ek=evt[i]+shift; ep=evt[i-1]+shift
        wt=w
        if rel_fn is not None:
            r=ep-rel_fn(by)
            if w<r: wt=r
        writes.append((wt,by,w,ek))
    writes.sort()
    tdec=0; fin=[]
    for wt,by,w,ek in writes:
        st=max(wt,tdec); tdec=st+B*by; fin.append((tdec,w,ek))
    miss=sum(1 for f,w,ek in fin if f>ek)
    lat=sorted(ek-w for f,w,ek in fin); 
    return miss,len(fin),lat[len(lat)//2]/1000
for B in (0.62,0.72):
    print("decoding %.2f us/byte: 12 KB %.1f ms, 22 KB %.1f ms"%(B,B*12000/1000,B*22000/1000))
    for Dt in (15000,16000,17000,18000,19000,20000):
        for name,rf in (("release 1 ms after the previous pickup",lambda by:-1000),("release before, by size",lambda by:0.35*by-1000)):
            m,n,lm=run(B,Dt,rf)
            print("   expected decoding %4.1f ms, %-40s: frames lost %4d of %d (%.2f%%), median latency %.1f ms"%(Dt/1000,name,m,n,100*m/n,lm))

# calibration: observed repeats during the locked part
Eall=[]
for line in open(sys.argv[1]):
    p=line.strip().split(',')
    if p[0]=='E': Eall.append((float(p[1]),int(p[5])))
t_a=F[0][0]; t_b=F[-1][0]
obs=0
for (s0,r0),(s1,r1) in zip(Eall,Eall[1:]):
    if t_a<s1<t_b: obs+=(r1-r0)&0xffff
print("\nrepeats observed in the locked part: %d of %d pictures"%(obs,len(F)))
def run2(A,B,Dt,rel_fn):
    writes=[]
    for w,by,v,lk,q,D in F:
        shift=Dt-D; tr=v+D+q+500
        i=bisect.bisect_left(evt,tr)
        if i<1 or i>=len(evt): continue
        ek=evt[i]+shift; ep=evt[i-1]+shift
        wt=w
        if rel_fn is not None:
            r=ep-rel_fn(by)
            if w<r: wt=r
        writes.append((wt,by,w,ek))
    writes.sort(); tdec=0; miss=0; lat=[]
    for wt,by,w,ek in writes:
        st=max(wt,tdec); tdec=st+max(2000,A+B*by)
        if tdec>ek: miss+=1
        lat.append(ek-w)
    lat.sort()
    return miss,lat[len(lat)//2]/1000
best=[]
for A in range(-8000,6001,1000):
    for B in [x/100 for x in range(30,121,5)]:
        m,_=run2(A,B,15000,lambda by:-1000)
        best.append((abs(m-obs),A,B,m))
best.sort()
print("models that reproduce the observed repeats:")
for d,A,B,m in best[:6]:
    print("  d = %+5.1f ms + %.2f us/byte (12 KB %.1f, 22 KB %.1f, 26 KB %.1f ms): %d"%(A/1000,B,(A+B*12000)/1000,(A+B*22000)/1000,(A+B*26000)/1000,m))
print()
for d,A,B,m in best[:3]:
    print("with d = %+.1f ms + %.2f us/byte:"%(A/1000,B))
    for Dt in (15000,17000,19000):
        for name,rf in (("release 1 ms after the previous pickup",lambda by:-1000),("release before, 0.35 us/byte - 1 ms",lambda by:0.35*by-1000),("release before, 0.5 us/byte - 1 ms",lambda by:0.5*by-1000),("no hold",None)):
            m2,lm=run2(A,B,Dt,rf)
            print("   expected decoding %4.1f ms, %-38s: lost %4d, median latency %.1f ms"%(Dt/1000,name,m2,lm))
