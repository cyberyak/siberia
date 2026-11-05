# Siberia protocol, draft 0

This draft covers one Mac and matches `src/siberia.c`. Nothing here is stable yet. Parts not marked as tested are design only.

## Terms

- **Stream:** a named source of video.
- **Publisher:** the app that produces a stream.
- **Subscriber:** an app that reads a stream.
- **Ring:** the fixed set of frame buffers a stream owns.
- **Control block:** a small shared-memory page holding the stream's state.

## How it works

Setup can be slow, and each frame must be almost free.

### Setup

1. The publisher allocates its ring of IOSurfaces and a control block in Mach shared memory.
2. It registers a Mach service and writes one small file naming it, in the user's temporary folder. The file is created with `link()`, so two publishers can't claim one name, and a file left by a crashed process can be taken over.
3. A subscriber reads that file, looks up the service, and sends its version, process ID and a token port.
4. The publisher replies once with the control block, every IOSurface and the subscriber's reader number.
5. The subscriber creates one GPU texture per surface and keeps them.

Both sides watch for the other's death with Mach dead-name notifications. Closing and crashing take the same path: a subscriber's token dies, and the publisher clears its claim and frees its place.

Registering a Mach service this way uses a deprecated call and doesn't work from sandboxed apps, so discovery is likely to change.

### Each frame

1. The publisher takes a buffer that is neither the newest frame nor claimed, and renders into it.
2. When the GPU finishes, it writes the frame's details and publishes the slot with an atomic store.
3. On its next display refresh, the subscriber reads `latest`, claims that slot, and rechecks `latest`.
4. It samples the texture directly, then releases the claim.

No message is sent per frame. Publishing only after the GPU finishes means a subscriber has nothing to wait for. A low-latency mode could publish sooner and use `MTLSharedEvent` to order the GPU work.

## Why a claim is safe

The publisher never writes to the slot `latest` names, so it can only reuse a slot after `latest` has moved on. All the operations below are sequentially consistent.

- The subscriber stores its claim, then reads `latest`.
- The publisher stores `latest`, then reads every claim.

If the recheck still sees the slot, the claim came before `latest` moved, so the publisher's later scan must see the claim and leave that slot alone. If `latest` has moved, the subscriber drops the claim and tries again. Sequence numbers can't repeat, which rules out seeing an old value by coincidence.

The stress test covers this: removing the recheck produces thousands of torn frames, and keeping it produces none.

## Control block

One 4 KB page. The exact layout is in `src/siberia.c`.

| Field | Written by | Purpose |
|---|---|---|
| `magic`, `version`, `format`, `slot_count` | Publisher, once | What the stream is |
| `closed` | Publisher | Set on a clean shutdown |
| `latest` | Publisher | `sequence << 8 \| slot`, or 0 before the first frame |
| `slots[]` | Publisher | Sequence and time for each buffer |
| `readers[]` | One subscriber each | Claim, wanted size and rate, and next refresh |

Each reader entry has its own cache line, so subscribers don't slow each other down.

## Rules

- **The publisher never waits.** A slow subscriber can't stall it.
- **Newest frame wins.** Subscribers skip to `latest` and never queue.
- **Enough slots:** a stream has `max_subscribers + 2` buffers, which guarantees a free one: one for the newest frame, one claim per subscriber, and one to write.
- **One claim per subscriber.** If its GPU work for the last frame hasn't finished, `acquire` returns `SIBERIA_BUSY`, so it should release from a GPU completion handler.
- **Crashes are cleaned up.** Tested with `kill -9` on both sides.
- **Subscribers say what they need.** Publishers can read the most demanding size and rate, and may skip rendering when nobody is subscribed.

## Timing

Every time uses `CLOCK_UPTIME_RAW`, which all processes share and which matches Mach host time, the basis for display link timestamps.

Each subscriber can report its next refresh, and the publisher can start rendering just early enough to finish before it.

**Experimental.** In the 4K benchmark, an 8 ms budget gave about 8 ms of lag in three of four runs, against 9–21 ms without pacing. One run fell to 50 fps, and 5–6 ms budgets often missed refreshes. The benchmark runs on ordinary threads with `nanosleep`, so a real display link with higher thread priority may do better.

## Formats

| Format | Bytes per pixel | 4K buffer | Status |
|---|---|---|---|
| `BGRA` 8-bit | 4 | 33 MB | Tested |
| 10-bit RGB | 4 | 33 MB | Planned |
| `RGhA` 16-bit float | 8 | 66 MB | Planned |

## Electron

Tested with Electron 45.2.3, where both APIs are experimental. This test used its own per-frame messages and hasn't moved to the core library yet.

- **Sending:** an offscreen window with `useSharedTexture` delivers each frame as an IOSurface through its `paint` event. That surface goes back to Chromium, so frames are GPU-copied into the ring.
- **Receiving:** `sharedTexture` imports an IOSurface and produces a `VideoFrame`, which `importExternalTexture` brings into WebGPU.
- **Several outputs:** one offscreen canvas holds them as tiles, which are split into streams in a single GPU pass. Offscreen popups never fired `paint`, so they are not an option.

## Open questions

- How should sandboxed apps discover streams? The current file and Mach service approach won't work for them.
- How should a stream change size or format while running? Today a publisher must close and recreate it.
- Should subscribers get a second claim, so GPU work can overlap across refreshes? That would need more buffers.
- Which pixel formats beyond `BGRA` need testing?
- Should Syphon compatibility be a separate adapter?
