# Siberia

Siberia is a protocol for sharing live video between apps on one Mac, built for 4K with as little compute as possible.

Apps share GPU memory instead of copying frames. A frame costs about the same to share at any resolution, because its pixels never move. The protocol also carries what Syphon-style sharing leaves to each app: timing, colour information, and what the receiver actually needs, so a sender never renders work nobody will use.

**Status:** early. The transport has been proven in a test, but there is no Siberia library yet. The protocol is a draft and will change.

## Goals

- **4K first:** 4K at 60 fps should be routine, not a special case.
- **Low compute:** no per-frame messages, polling, allocation or CPU copies.
- **Low lag:** receivers always take the newest frame, and lag can't build up in a queue.
- **Works in web tech:** Electron apps can both send and receive, not just native apps.

Sharing between machines is not part of this first version. The design keeps that door open: over a cable or network, a small process could write incoming frames into a local Siberia stream.

## What has been proven

Measured on an M2 MacBook Air with Electron 45 and a synthetic test shader. CPU figures use 100% for one core.

| Test | Frame rate | CPU | Median lag | Torn / skipped frames |
|---|---|---|---|---|
| Send 4K, 1-minute run | 60 fps | 39% | 13 ms | 0 / 0 of 3,302 |
| Receive 4K into WebGPU | 60 fps | 37% | 7–8 ms | 0 torn in frames checked |
| 4 × 1080p from one canvas | 60 fps each | 38% | 28 ms | 0 / 0 of 1,697 |
| 2 × 4K from one canvas | 60 fps each | 36% | 27 ms | 0 / 4 of 837 |

For comparison, sending 4K frames over localhost HTTP reached 14 fps at 107% CPU.

The test used a separate Mach message for each frame, and sending from Electron needed one GPU copy per frame. It didn't test the control block in the draft, float formats, or non-Electron receivers such as TouchDesigner. Full figures and how to run them are in [experiments/electron-test/RESULTS.md](experiments/electron-test/RESULTS.md).

## Repository

- [`docs/protocol.md`](docs/protocol.md): the draft protocol.
- [`experiments/electron-test/`](experiments/electron-test/): the test behind the numbers above. It installs its own Node and Electron, so `.tools` and `node_modules` are not in git.

## First user

VRS, a modular video synthesizer, is the first app planned to use Siberia, through an Electron app.
