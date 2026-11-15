# Per ogni quadro ripetuto dopo l'aggancio: l'immagine destinata a quella presa, quanto tempo ha avuto
# per decodificarsi e la sua dimensione. Uso: python3 genlock_trace_causes.py traccia.csv
# (legge genlock_trace_analysis.py nella stessa cartella)
import sys
exec(open(__import__('os').path.join(__import__('os').path.dirname(__file__),'genlock_trace_analysis.py')).read().split("print(\"quadri\"")[0])
import bisect, collections
print("quadri",len(evs),"ripetute",sum(e['dst'] for e in evs),"scartate",sum(e['dsk'] for e in evs))
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
    if not d: cat='nessuna immagine scritta tra 8 ms e 2 quadri prima (arrivo in ritardo)'; sl=None
    else:
        f=d[-1]; sl=(e['ev']-f['w']-f['held'])/1000
        if sl>2*16.7: cat='la più recente era già vecchia (>33 ms): buco negli arrivi'
        elif f['by']>=19000: cat='grande (>=19 KB) con %d-%d ms per decodificarsi'%(int(sl//2)*2,int(sl//2)*2+2)
        else: cat='normale con %d-%d ms per decodificarsi'%(int(sl//2)*2,int(sl//2)*2+2)
    c[cat]+=1; ex[cat].append("t%.1f [%s]"%(e['t']," ".join(desc)))
for k,v in sorted(c.items(),key=lambda x:-x[1]): print("%3d  %s\n       es.: %s"%(v,k," | ".join(ex[k][:2])))
