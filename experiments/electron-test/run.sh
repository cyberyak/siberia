#!/bin/sh
# usage: run.sh <name> <mode> [extra electron args]; starts the external app, then Electron.
cd "$(dirname "$0")"; name=$1; mode=$2; shift 2
E=node_modules/electron/dist/Electron.app/Contents/MacOS/Electron
case $mode in
 send|multi|tiles) build/rx ${SECS:-12} results/$name.rx.json >results/rx.out 2>&1 & RX=$!; sleep 0.5 ;;
esac
$E . --mode=$mode --name=$name "$@" 2>&1 | grep -vE 'ERROR:|WARNING:|^$' | tail -12
[ -n "$RX" ] && { sleep 1; kill $RX 2>/dev/null; wait $RX 2>/dev/null; }
