# Electron test, 2026-11-05

M2 MacBook Air, macOS 15.3.1, Electron 45.2.3. CPU: 100% = one core. Raw reports are in `results/`.

| Test | Rate | CPU | GPU | Median lag | Torn / skipped |
|---|---|---|---|---|---|
| Send 4K, 60 s soak | 60 fps | 39% | 75% | 13 ms | 0 / 0 of 3302 |
| Current HTTP path, 4K (bridge copy with cap raised) | 14 fps | 107% | 64% | not measured | not measured |
| Send 1080p | 60 fps | 32% | 58% | 6 ms | 0 / 0 of 422 |
| Current HTTP path, 1080p | 34 fps | 82% | 40% | not measured | not measured |
| Receive 4K into WebGPU | 60 fps | 37% | 52% | 7-8 ms | 0 torn in ~110 checked per 2 s |
| 4 x 1080p, tiles | 60 fps each | 38% | 71% | 28 ms | 0 / 0 of 1697 |
| 4 x 1080p, separate windows | 60 fps each | 65% | 74% | 9 ms | 0 / 0 of 1678 |
| 2 x 4K, tiles | 60 fps each | 36% | 85% | 27 ms | 0 / 4 of 837 |

Render only at 4K: CPU 32%, GPU 64%. Render plus readback to JS: CPU 43%, 2.4 ms per frame.

Offscreen popups sharing a renderer never fire `paint`, so they are not a route to several outputs.
All performance runs use a synthetic shader. The real VRS page boots offscreen with WebGPU, but its frames were not sent.

Run from this folder: `npm install`, then `./build.sh`, then `SECS=7 ./run.sh <name> send --w=3840 --h=2160 --secs=10`, or `./runrecv.sh <name> 3840 2160 60 12`.
