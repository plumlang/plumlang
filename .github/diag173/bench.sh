set -e
make -j4 >/dev/null; cp plum plum-o2
make -j4 PGO_LTO=0 release-pgo >/dev/null 2>&1; cp plum plum-pgo
make -j4 release-pgo >/dev/null 2>&1; cp plum plum-ltopgo
for round in 1 2 3 4; do
  for v in o2 pgo ltopgo; do python3 bench/run.py ./plum-$v --runs 5 --plum-only --json res-$v-$round.json >/dev/null; done
done
python3 - <<'P'
import json
names=None; best={}
for v in ["o2","pgo","ltopgo"]:
    for r in range(1,5):
        d=json.load(open(f"res-{v}-{r}.json"))["seconds"]
        for k,x in d.items():
            best.setdefault(v,{}); best[v][k]=min(best[v].get(k,9e9), x["plum"])
ks=sorted(best["o2"])
print("| build | "+" | ".join(ks)+" |")
for v in best: print(f"| {v} | "+" | ".join(f"{best[v][k]:.3f}" for k in ks)+" |")
print("| ltopgo vs pgo | "+" | ".join(f"{(best['ltopgo'][k]/best['pgo'][k]-1)*100:+.0f}%" for k in ks)+" |")
print("| ltopgo vs o2 | "+" | ".join(f"{(best['ltopgo'][k]/best['o2'][k]-1)*100:+.0f}%" for k in ks)+" |")
P
