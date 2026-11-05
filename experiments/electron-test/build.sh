#!/bin/sh
set -e
cd "$(dirname "$0")"; mkdir -p build
NODE_INC=".tools/node-v26.13.0-darwin-arm64/include/node"
W="-Wno-deprecated-declarations -Wno-unused-function -O2"
clang $W -fobjc-arc -framework Foundation -framework IOSurface native/rx.m -o build/rx
clang $W -fobjc-arc -framework Foundation -framework IOSurface native/tx.m -o build/tx
clang++ $W -std=c++17 -fobjc-arc -bundle -undefined dynamic_lookup -I"$NODE_INC" -framework Foundation -framework Metal -framework IOSurface native/addon.mm -o build/addon.node
echo built
