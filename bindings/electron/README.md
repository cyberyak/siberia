# Siberia for Electron

A Node-API addon for Electron's main process. Both Electron APIs it relies on are experimental.

## Build

Compile `siberia_node.mm` against `libsiberia.a` as a bundle. `experiments/electron-test/build.sh` shows the command.

## Sending

Render into an offscreen window, then pass each `paint` surface to `send`. Keep the texture until `onSent` fires.

```js
const siberia = require('./addon.node');
const streams = [siberia.publish('My output', 3840, 2160, 2)];
const held = new Map();
let token = 0;
siberia.onSent((id) => { held.get(id).release(); held.delete(id); });

win.webContents.on('paint', (event) => {
  const texture = event.texture, id = ++token;
  held.set(id, texture);
  if (!siberia.send(streams, texture.textureInfo.handle.ioSurface, 1, 1, id)) {
    held.delete(id);
    texture.release();
  }
});
```

`send` returns false when nothing is using the surface, because nobody is subscribed or the last copy is still running.

For several outputs, pass one stream per tile, with the columns and rows of the canvas. Streams are read left to right, then top to bottom.

## Receiving

Call `acquire` once per display frame. It returns a frame, `null` when there's nothing new, or `'closed'` when the publisher has gone.

```js
const sub = siberia.subscribe('My output'); // null until the stream exists
const frame = siberia.acquire(sub);
if (frame && frame !== 'closed') {
  const imported = sharedTexture.importSharedTexture({
    textureInfo: {
      pixelFormat: 'bgra',
      codedSize: { width: frame.width, height: frame.height },
      handle: { ioSurface: frame.surface },
    },
    allReferencesReleased: () => siberia.release(sub),
  });
  sharedTexture.sendSharedTexture({ frame: win.webContents.mainFrame, importedSharedTexture: imported })
    .finally(() => imported.release());
}
```

In the renderer, turn it into a WebGPU texture, and release it when done:

```js
sharedTexture.setSharedTextureReceiver(async ({ importedSharedTexture }) => {
  const video = importedSharedTexture.getVideoFrame();
  const texture = device.importExternalTexture({ source: video });
  // draw with texture, then:
  video.close();
  importedSharedTexture.release();
});
```

A subscriber holds one frame at a time, so don't call `acquire` again until `allReferencesReleased` has run.

## Other calls

- `list()`: streams on this Mac, as `{ name, pid }`.
- `reportRefresh(sub, timeNs)`: tells the publisher when this display refreshes next, using `now()`'s clock.
- `closePublisher(handle)`, `closeSubscriber(handle)`: call these before quitting.
- `stats()`: send counts, timings and dropped frames.
