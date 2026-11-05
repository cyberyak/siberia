#!/bin/sh
set -e
cd "$(dirname "$0")"; mkdir -p build
make -C ../.. build/libsiberia.a build/pattern >/dev/null
clang++ -Wno-deprecated-declarations -O2 -std=c++17 -fobjc-arc -bundle -undefined dynamic_lookup -I.tools/node-v26.13.0-darwin-arm64/include/node -I../../include -framework Foundation -framework Metal -framework IOSurface -framework CoreFoundation ../../bindings/electron/siberia_node.mm ../../build/libsiberia.a -o build/addon.node
echo built
