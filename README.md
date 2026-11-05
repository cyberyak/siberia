# Siberia

Siberia is a protocol for sharing live video between apps on one Mac, built for 4K with as little compute as possible.

Apps share GPU memory instead of copying frames. A frame costs about the same to share at any resolution, because its pixels never move. The protocol also carries what Syphon-style sharing leaves to each app: timing, colour information, and what the receiver actually needs, so a sender never renders work nobody will use.

**Status:** early. The core library works and is tested on one Mac, VRS, a modular video synthesizer, uses it in its desktop app, and ThinFilm, a liquid film simulator, publishes to it. The protocol is a draft and will change.

## Goals

- **4K first:** 4K at 60 fps should be routine, not a special case.
- **Low compute:** no per-frame messages, polling, allocation or CPU copies.
- **Low lag:** receivers always take the newest frame, and lag can't build up in a queue.
- **Works in web tech:** Electron apps can both send and receive, not just native apps.

Sharing between machines is not part of this first version. The design keeps that door open: over a cable or network, a small process could write incoming frames into a local Siberia stream.

## Results

Measured on an M2 MacBook Air. CPU figures use 100% for one core.

### Core library, native Metal

Real 4K rendering at 60 fps, between two processes.

| Measure | Result |
|---|---|
| CPU | about 2% per process |
| Each Siberia call | under 1 µs |
| System calls from Siberia | 1 in 100,000 frames |
| CPU with no subscribers, skipping rendering | 0.08% |
| Torn frames, stress test across 3 processes | 0 in about 235,000 |

The stress test checks every pixel of every frame, and it finds 7,287 torn frames when the claim recheck is removed, so a pass means something.

Two findings were less flattering:

- **Removing per-frame messages barely changed CPU:** 2.0% against 2.1%, because Metal's own work dominates. The saving is about scheduling and simplicity more than compute.
- **Timing feedback helps but isn't reliable yet:** with an 8 ms budget, lag held at about 8 ms in three of four runs, against 9–21 ms without it. One run dropped to 50 fps, and tighter budgets often missed refreshes. It stays experimental.

### Electron

Tested on Electron 45 with a synthetic shader, sending and receiving through [`bindings/electron`](bindings/electron/README.md). Nearly all of the CPU use in Electron is Electron itself, not sharing.

The figures below come from the first test, on a quiet GPU, before the core library existed.

| Test | Frame rate | CPU | Median lag | Torn / skipped frames |
|---|---|---|---|---|
| Send 4K, 1-minute run | 60 fps | 39% | 13 ms | 0 / 0 of 3,302 |
| Receive 4K into WebGPU | 60 fps | 37% | 7–8 ms | 0 torn in frames checked |
| 4 × 1080p from one canvas | 60 fps each | 38% | 28 ms | 0 / 0 of 1,697 |
| 2 × 4K from one canvas | 60 fps each | 36% | 27 ms | 0 / 4 of 837 |

Running on the core library has only been measured with another app holding the GPU at 100%. No frames tore, and it held 58 fps for 4K and 41 fps for 2 × 4K, dropping frames rather than stalling. Clean numbers are still to come.

For comparison, sending 4K frames over localhost HTTP reached 14 fps at 107% CPU. Full figures are in [experiments/electron-test/RESULTS.md](experiments/electron-test/RESULTS.md).

### VRS

The first real app. VRS reads each frame back from WebGPU and copies it straight into a stream, measured with another app holding the GPU at about 95%.

| Output | Siberia | Localhost HTTP to Syphon |
|---|---|---|
| 1080p | 60 fps at 74% CPU | 57 fps at 128% CPU |
| 4K | 60 fps at 123% CPU, none skipped | Scaled to 1080p |

CPU figures cover the whole app, which uses about 40% just rendering. The copy path isn't the cheapest possible, so there's room to improve.

Sending to VRS costs real work, so copies that only publish can stop drawing themselves. Three VRS copies in Send Only and a fourth loading all three streams reached a steady 60 fps at 720p, with gaps of about 20–40 ms. With the full interface on all of them, the same setup ran at 38 fps. Frames arrive in VRS as GPU textures, with a small CPU sample for its control engine. At 720p that saved almost no CPU, and it hasn't been measured at 4K.

### ThinFilm

ThinFilm publishes a stream named `ThinFilm`. The rate depends on how fast it can simulate, not on Siberia: its own measurements range from 9 fps at a resolution of 3840 to 74 fps at 1600. VRS can show 60 fps while a source sends far fewer, so `pattern watch` is the way to check a stream's real rate.

## Build and test

```bash
make
```

```bash
make test
```

`make test` runs the tests as real processes, including `kill -9` crashes, then repeats the stress test with the race widened, then confirms the deliberately broken build gets caught. To run the 4K benchmark:

```bash
tools/bench.sh shared
```

To test the Electron binding under plain Node:

```bash
make test-binding
```

## Repository

- [`include/siberia.h`](include/siberia.h): the public API.
- [`src/siberia.c`](src/siberia.c): the core library.
- [`bindings/electron/`](bindings/electron/README.md): the Electron binding.
- [`test/`](test/): the multi-process tests.
- [`tools/`](tools/): the Metal benchmark, and a pattern tool that checks streams from outside.
- [`docs/protocol.md`](docs/protocol.md): the draft protocol.
- [`experiments/electron-test/`](experiments/electron-test/): the Electron test. It needs its own Node in `.tools` and is set up by hand.
