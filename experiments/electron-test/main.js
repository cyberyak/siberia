// usage: electron . --mode=send|tiles|recv|plain|readback|post --stream= --w= --h= --secs= --cols= --rows=
const { app, BrowserWindow, ipcMain, sharedTexture } = require('electron');
const path = require('path'), fs = require('fs'), { execSync, spawn } = require('child_process');
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.split('=')[1] : d; };
const mode = arg('mode', 'send'), W = +arg('w', 3840), H = +arg('h', 2160), SECS = +arg('secs', 12), COUNT = +arg('count', 1), FPS = +arg('fps', 60);
const out = { mode, W, H, count: COUNT, page: [], errors: [], info: null, cpu: [], gpu: [] };
const addon = require('./build/addon.node');
process.on('uncaughtException', (e) => console.log('UNCAUGHT', e));
const offscreen = mode === 'send' || mode === 'tiles', COLS = mode === 'tiles' ? +arg('cols', 2) : 1, ROWS = mode === 'tiles' ? +arg('rows', 2) : 1, STREAM = arg('stream', 'electron-test');
const gpuUtil = () => { try { const s = execSync('ioreg -r -d 1 -c IOAccelerator', { encoding: 'utf8' }); const m = s.match(/"Device Utilization %"=(\d+)/); return m ? +m[1] : null; } catch (e) { return null; } };
const procTime = () => { const pids = app.getAppMetrics().map((m) => [m.pid, m.type]); try {  } catch (e) {}
  ['pattern', 'bridge4k', 'syphon-bridge'].forEach((n) => { try { execSync(`pgrep -x ${n}`, { encoding: 'utf8' }).trim().split('\n').forEach((p) => pids.push([+p, n])); } catch (e) {} });
  const map = new Map(pids), r = {}; execSync(`ps -o pid=,time= -p ${pids.map((p) => p[0]).join(',')}`, { encoding: 'utf8' }).trim().split('\n').forEach((l) => { const [p, t] = l.trim().split(/\s+/), [m, sec] = t.split(':'); r[p] = { type: map.get(+p), s: +m * 60 + +sec }; });
  return { t: Date.now(), r }; };
const avg = (a) => a.length ? a.reduce((x, y) => x + y, 0) / a.length : null;
let done = false; const wins = [];
function finish() {
  done = true;
  const a = out.start, b = procTime(), el = (b.t - a.t) / 1000, perType = {}; let total = 0;
  Object.entries(b.r).forEach(([p, v]) => { const was = a.r[p]; if (!was) return; const pct = (v.s - was.s) / el * 100; perType[v.type] = +((perType[v.type] || 0) + pct).toFixed(1); total += pct; });
  out.gpu.splice(0, 2);
  const res = { mode, W, H, count: COUNT, cpuByProcess: perType, cpuTotal: +total.toFixed(1), gpuUtilAvg: avg(out.gpu), paint: out.paint, info: out.info, binding: addon.stats(), noTexture: out.noTexture || 0, streams: wins.length, page: out.page.slice(-3), errors: out.errors.slice(0, 5) };
  fs.writeFileSync(path.join(__dirname, 'results', `${arg('name', mode)}.json`), JSON.stringify(res, null, 1));
  console.log(JSON.stringify(res)); out.closeAll && out.closeAll(); app.exit(0);
}
ipcMain.on('stats', (e, o) => { if (o.fatal || o.error) { out.errors.push(o.fatal || o.error); console.log('PAGE', o); } else out.page.push(o); });
app.whenReady().then(async () => {
  const paint = { n: 0, sent: 0 }; out.paint = paint;
  const streams = []; if (offscreen) for (let i = 0; i < COLS * ROWS; i++) streams.push(addon.publish(COLS * ROWS > 1 ? `${STREAM}-${i}` : STREAM, Math.floor(W / COLS), Math.floor(H / ROWS), 2));
  out.closeAll = () => streams.forEach((h) => addon.closePublisher(h));
  const held = new Map(); let token = 0;
  addon.onSent((t) => { const tex = held.get(t); held.delete(t); if (tex) tex.release(); });
  const w = new BrowserWindow({ width: offscreen ? W : 640, height: offscreen ? H : 360, show: !offscreen, useContentSize: true, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: false, sandbox: false, backgroundThrottling: false, webSecurity: mode !== 'post', ...(offscreen ? { offscreen: { useSharedTexture: true } } : {}) } });
  wins.push(w);
  w.webContents.on('console-message', (ev) => { if (ev.level === 'error') out.errors.push(ev.message.slice(0, 160)); });
  w.webContents.on('render-process-gone', (e, d) => out.errors.push('renderer gone ' + d.reason));
  if (offscreen) { w.webContents.setFrameRate(FPS);
    w.webContents.on('paint', (ev) => { const t = ev.texture; if (!t) { out.noTexture = (out.noTexture || 0) + 1; return; }
      const start = performance.now(); paint.n++; const id = ++token; held.set(id, t);
      if (addon.send(streams, t.textureInfo.handle.ioSurface, COLS, ROWS, id)) paint.sent++; else { held.delete(id); t.release(); }
      paint.callMs = (paint.callMs || 0) + performance.now() - start; }); }
  w.loadFile('page.html', { query: { mode, w: W, h: H, cols: COLS, rows: ROWS } });
  if (mode === 'recv') {
    const wc = w.webContents; let imported = 0, sendFail = 0, closed = 0, ready = false, sub = null, inUse = false, heldMax = 0, t0 = 0, busyTicks = 0;
    ipcMain.on('stats', (e, o) => { if (o.ready) ready = true; });
    ipcMain.on('want', () => {
      if (!ready || done || wc.isDestroyed()) return;
      if (sub === null) { sub = addon.subscribe(STREAM); if (sub === null) return; }
      if (inUse) { busyTicks++; return; }
      const f = addon.acquire(sub); if (!f) return; if (f === 'closed') { closed++; return; }
      inUse = true; const started = performance.now();
      try {
        const imp = sharedTexture.importSharedTexture({ textureInfo: { pixelFormat: 'bgra', codedSize: { width: f.width, height: f.height }, visibleRect: { x: 0, y: 0, width: f.width, height: f.height }, handle: { ioSurface: f.surface } }, allReferencesReleased: () => { addon.release(sub); inUse = false; heldMax = Math.max(heldMax, performance.now() - started); } });
        imported++;
        sharedTexture.sendSharedTexture({ frame: wc.mainFrame, importedSharedTexture: imp }).catch((e) => { sendFail++; out.errors.push(String(e)); }).finally(() => imp.release());
      } catch (e) { inUse = false; addon.release(sub); out.errors.push('import: ' + e.message); }
    });
    out.recvStats = () => ({ imported, sendFail, closed, busyTicks, heldMaxMs: +heldMax.toFixed(1) });
  }
  setTimeout(() => { out.start = procTime(); }, 3000);
  const iv = setInterval(() => { const g = gpuUtil(); if (g != null) out.gpu.push(g); }, 1000);
  setTimeout(() => { clearInterval(iv); if (out.recvStats) out.info = out.recvStats(); paint.fps = paint.n / SECS; paint.callMs = paint.n ? (paint.callMs || 0) / paint.n : 0; finish(); }, SECS * 1000);
});
