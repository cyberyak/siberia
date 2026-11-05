#!/bin/zsh
# usage: tools/bench.sh <mode> [budget_ms] [w] [h] [secs]
cd "${0:A:h}/.."
m=$1; b=${2:-8}; w=${3:-3840}; h=${4:-2160}; t=${5:-10}; n="bench$$$RANDOM"
build/bench sub "$n" $t $m > build/s.out 2>build/s.err & S=$!
build/bench pub "$n" $w $h $t $m $b > build/p.out 2>build/p.err
wait $S; cat build/p.out build/s.out; cat build/p.err build/s.err | head -3
