#!/bin/zsh
# usage: run.sh <name> <send|tiles> [--w= --h= --cols= --rows= --secs=]. Set SECS for the check length.
cd "${0:A:h}"; name=$1; mode=$2; shift 2; rx=()
E=node_modules/electron/dist/Electron.app/Contents/MacOS/Electron; P=../../build/pattern; n=1
[ $mode = tiles ] && n=${TILES:-4}; s="t$$"
for ((i=0;i<n;i++)); do st=$s; [ $n -gt 1 ] && st=$s-$i; $P sub "$st" ${SECS:-7} results/$name.rx$i.json 2>/dev/null & rx+=($!); done
$E . --mode=$mode --stream=$s --name=$name "$@" 2>&1 | grep -E '^\{|UNCAUGHT' | tail -3
sleep 1; for p in $rx; do kill $p 2>/dev/null; done
python3 - "$name" $n <<'P'
import json,sys
n=int(sys.argv[2]); a=[]
for i in range(n):
    try: a.append(json.load(open(f'results/{sys.argv[1]}.rx{i}.json')))
    except Exception as e: print('rx',i,'no result')
if a: print('rx', {'streams':len(a),'frames':sum(x['frames'] for x in a),'fpsEach':[x['fps'] for x in a],'torn':sum(x['torn'] for x in a),'skipped':sum(x['skipped'] for x in a),'latP50':[x['latencyP50ms'] for x in a],'latP99':max(x['latencyP99ms'] for x in a)})
P
