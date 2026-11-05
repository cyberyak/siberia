#!/bin/sh
cd "$(dirname "$0")"; name=$1; w=$2; h=$3; fps=$4; secs=$5; shift 5
E=node_modules/electron/dist/Electron.app/Contents/MacOS/Electron
$E . --mode=recv --name=$name --w=$w --h=$h --secs=$secs "$@" > results/$name.log 2>&1 &
EP=$!; sleep 2.5
build/tx $w $h $fps $((secs-4)) > results/$name.tx 2>&1
wait $EP; grep -vE 'ERROR:|WARNING:|Security|^$|electronjs|consult|risks|once the app|will not show|Insecure|This renderer' results/$name.log | tail -5; cat results/$name.tx
