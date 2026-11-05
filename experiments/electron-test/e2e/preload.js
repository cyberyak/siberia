// Stands in for a native app: replaces the web host before the page starts.
if (process.env.FAKE_HOST) { const log = (window.hostLog = { sends: 0, sizes: new Set(), stops: [] });
  window.vrsHost = { canCapture: true, canShare: true, text: { noCapture: '', noSources: '', down: 'down', noSend: 'x', noMapSend: 'x' },
    sources: async () => [{ id: 'n1', app: 'NativeApp', name: 'stream' }], frame: async (id, w, h) => new Uint8ClampedArray(w * h * 4).fill(120).buffer,
    send: async (name, w, h, data) => { log.sends++; log.sizes.add(`${name} ${w}x${h} ${data.length}`); return ''; }, stop: (n) => log.stops.push(n) }; }
