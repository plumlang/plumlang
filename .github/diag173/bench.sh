set -e
python3 --version
b() { python3 bench/run.py "$1" --runs 5 --plum-only --json "res-$2.json" >/dev/null; echo "$2"; python3 -c "import json,sys; d=json.load(open('res-$2.json')); print(d)" | head -c 3000; echo; }
make -j4 >/dev/null; cp plum plum-o2
make -j4 release-lto >/dev/null; cp plum plum-lto
make -j4 PGO_LTO=0 release-pgo >/dev/null 2>&1; cp plum plum-pgo
make -j4 release-pgo >/dev/null 2>&1; cp plum plum-ltopgo
for round in 1 2; do
  for v in o2 lto pgo ltopgo; do b ./plum-$v $v-$round; done
done
