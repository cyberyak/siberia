#!/bin/zsh
# usage: runrecv.sh <name> <w> <h> <fps> <secs>
cd "${0:A:h}"; name=$1; w=$2; h=$3; fps=$4; secs=$5; s="r$$"
E=node_modules/electron/dist/Electron.app/Contents/MacOS/Electron
../../build/pattern pub $s $w $h $fps $secs > results/$name.pub 2>&1 & P=$!
$E . --mode=recv --stream=$s --name=$name --w=$w --h=$h --secs=$secs 2>&1 | grep -E '^\{|UNCAUGHT' | tail -2
kill $P 2>/dev/null; cat results/$name.pub
