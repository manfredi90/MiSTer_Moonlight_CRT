# For each frame repeated after the lock: the picture meant for that pickup, how long it had to
# decode and its size. Usage: python3 genlock_trace_causes.py trace.csv
# (reads genlock_trace_analysis.py in the same folder)
import sys
exec(open(__import__('os').path.join(__import__('os').path.dirname(__file__),'genlock_trace_analysis.py')).read().split("print(\"frames\"")[0])
import bisect, collections
print("frames",len(evs),"repeated",sum(e['dst'] for e in evs),"dropped",sum(e['dsk'] for e in evs))
c=collections.Counter(); ex=collections.defaultdict(list)
slk=collections.Counter()
for k,e in enumerate(evs):
    if not e['dst'] or e['t']<45: continue
    # pictures written in (e - 2P, e]
    i=bisect.bisect_right(wts,e['ev'])
    cand=[fbyw[j] for j in range(max(0,i-4),i)]
    desc=["%.1f/%dB"%((e['ev']-f['w']-f['held'])/1000,f['by']) for f in cand]
    # destined: latest with slack >= 8 ms
    d=[f for f in cand if e['ev']-f['w']-f['held']>=8000]
    if not d: cat='no picture written between 8 ms and 2 frames before (late arrival)'; sl=None
    else:
        f=d[-1]; sl=(e['ev']-f['w']-f['held'])/1000
        if sl>2*16.7: cat='the latest was already old (>33 ms): a gap in the arrivals'
        elif f['by']>=19000: cat='large (>=19 KB) with %d-%d ms to decode'%(int(sl//2)*2,int(sl//2)*2+2)
        else: cat='normal with %d-%d ms to decode'%(int(sl//2)*2,int(sl//2)*2+2)
    c[cat]+=1; ex[cat].append("t%.1f [%s]"%(e['t']," ".join(desc)))
for k,v in sorted(c.items(),key=lambda x:-x[1]): print("%3d  %s\n       e.g.: %s"%(v,k," | ".join(ex[k][:2])))
