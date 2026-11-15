# Analyses a trace (trace.csv): for each repeated frame, the picture before and after.
# Usage: python3 genlock_trace_analysis.py trace.csv
import sys, bisect
F=[];E=[]
for line in open(sys.argv[1]):
    if line.startswith('F,'):
        p=line.strip().split(',')[1:]
        w,recv,rtp,by,idr,phv,err,tg,q,D,trim,lk,held,v=p
        F.append(dict(w=float(w),recv=int(recv),rtp=int(rtp),by=int(by),idr=int(idr),phv=float(phv),err=float(err),tg=float(tg),q=float(q),D=float(D),trim=int(trim),lk=int(lk),held=int(held),v=float(v)))
    elif line.startswith('E,'):
        p=line.strip().split(',')[1:]
        s,seq,pk,sk,st,Pm,tref,eoff=p[:8]
        E.append(dict(s=float(s),seq=int(seq),pk=int(pk),sk=int(sk),st=int(st),Pm=float(Pm),tref=float(tref),eoff=float(eoff)))
t0=E[0]['s']
P=16666.7
evs=[]
for i in range(1,len(E)):
    a,b=E[i-1],E[i]
    ev=b['s']-P+b['eoff']
    evs.append(dict(ev=ev,t=(b['s']-t0)/1e6,dpk=b['pk']-a['pk'],dsk=(b['sk']-a['sk'])&0xffffffff,dst=(b['st']-a['st'])&0xffff,lk=None))
wts=sorted(f['w']+f['held'] for f in F)
fbyw=sorted(F,key=lambda f:f['w']+f['held'])
print("frames",len(evs),"picks",sum(e['dpk'] for e in evs),"repeated",sum(e['dst'] for e in evs),"dropped",sum(e['dsk'] for e in evs))
# For repeats after 40 s: the latest picture written before the eval and its slack; also next one
rep=[e for e in evs if e['dst'] and e['t']>40]
print("repeats after 40 s:",len(rep))
for e in rep[:40]:
    i=bisect.bisect_right(wts,e['ev'])
    prev=fbyw[i-1]; nxt=fbyw[i] if i<len(fbyw) else None
    def desc(f):
        wt=f['w']+f['held']
        return "written %+6.1f ms (arrival %+6.1f, held %4.1f), %6d bytes, x=%5.1f ms, idr %d"%((wt-e['ev'])/1000,(f['w']-e['ev'])/1000,f['held']/1000,f['by'],(f['w']-f['v'])/1000,f['idr'])
    print("t %6.2f  before: %s | after: %s"%(e['t'],desc(prev),desc(nxt) if nxt else '-'))
