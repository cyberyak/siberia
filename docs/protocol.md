# Siberia protocol, draft 0

This draft covers one Mac. Nothing here is stable yet. Parts marked **Tested** were exercised in `experiments/electron-test`, and everything else is design only.

## Terms

- **Stream:** a named source of video.
- **Publisher:** the app that produces a stream.
- **Subscriber:** an app that reads a stream.
- **Ring:** the fixed set of frame buffers a stream owns.
- **Control block:** a small shared-memory page holding the stream's state.

## How it works

Setup can be slow, and each frame must be almost free.

### Setup

1. The publisher allocates its ring of IOSurfaces, plus a control block.
2. A subscriber finds the stream by name and connects over a Mach or XPC channel.
3. The publisher hands over the IOSurfaces and control block once. **Tested** for surfaces, using Mach ports.
4. The subscriber creates one GPU texture per surface and keeps them.

Sandboxed apps may need a different discovery path, which is still open.

### Each frame

1. The publisher renders into a buffer that no subscriber has claimed.
2. When the GPU finishes, it writes the new frame's details and publishes the slot with an atomic store.
3. On its next display refresh, the subscriber reads `latest`, claims that slot, and rechecks `latest`.
4. It samples the texture directly, then releases the claim.

No message is sent per frame, so no process wakes up that wasn't already awake. Publishing only after the GPU finishes means a subscriber has nothing to wait for. A low-latency mode could publish sooner and use `MTLSharedEvent` to order the GPU work.

## Control block

```c
#define SIBERIA_MAGIC 0x52424953u   // "SIBR"
#define SIBERIA_MAX_SLOTS 8
#define SIBERIA_MAX_READERS 8

typedef struct {
  uint64_t sequence;        // frame number, increasing
  uint64_t time_ns;         // when rendering started, on a shared clock
} siberia_slot;

typedef struct {
  _Atomic uint32_t pid;       // 0 means this entry is free
  _Atomic int32_t  claimed;   // slot in use, or -1
  uint32_t wanted_width;      // 0 means full size
  uint32_t wanted_height;
  uint32_t wanted_fps_milli;  // 0 means every frame
  uint64_t next_refresh_ns;   // when this reader's display next refreshes
} siberia_reader;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t width, height;
  uint32_t pixel_format;      // IOSurface format code
  uint32_t color_space;
  uint32_t slot_count;
  _Atomic uint64_t latest;    // sequence << 8 | slot
  siberia_slot   slots[SIBERIA_MAX_SLOTS];
  siberia_reader readers[SIBERIA_MAX_READERS];
} siberia_control;
```

## Rules

- **The publisher never waits.** A slow subscriber can't stall it. If every other slot is claimed, it drops the frame.
- **Newest frame wins.** Subscribers skip to `latest` and never queue.
- **Enough slots:** a ring needs at least the number of subscribers plus two.
- **Crashes are cleaned up.** A publisher must detect a subscriber's process exit and clear its claim.
- **Subscribers say what they need.** Publishers should honour requested size and frame rate when they can, and may skip rendering when nobody is subscribed.

## Timing

Each subscriber writes its next refresh time into the control block. The publisher can then start rendering just early enough to finish before it. This could save up to a whole frame of lag, and it hasn't been tested.

## Formats

| Format | Bytes per pixel | 4K buffer | Status |
|---|---|---|---|
| `BGRA` 8-bit | 4 | 33 MB | **Tested** |
| 10-bit RGB | 4 | 33 MB | Planned |
| `RGhA` 16-bit float | 8 | 66 MB | Planned |

## Electron

**Tested** with Electron 45.2.3, where both APIs are experimental:

- **Sending:** an offscreen window with `useSharedTexture` delivers each frame as an IOSurface through its `paint` event. That surface goes back to Chromium, so frames are GPU-copied into the ring.
- **Receiving:** `sharedTexture` imports an IOSurface and produces a `VideoFrame`, which `importExternalTexture` brings into WebGPU.
- **Several outputs:** one offscreen canvas holds them as tiles, which are split into streams in a single GPU pass. Offscreen popups never fired `paint`, so they are not an option.

## Open questions

- How should subscribers discover streams, especially from sandboxed apps?
- Which shared clock should `time_ns` use across processes?
- Should the claim sequence change to close the race between a subscriber's claim and a publisher reusing that slot?
- How should a stream change size or format while running?
- Should Syphon compatibility be a separate adapter?
