// usage: electron . --mode=send|plain|readback|recv|multi --w= --h= --secs= --count=
const { app, BrowserWindow, ipcMain, sharedTexture } = require('electron');
const path = require('path'), fs = require('fs'), { execSync, spawn } = require('child_process');
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.split('=')[1] : d; };
const mode = arg('mode', 'send'), W = +arg('w', 3840), H = +arg('h', 2160), SECS = +arg('secs', 12), COUNT = +arg('count', 1), FPS = +arg('fps', 60);
const out = { mode, W, H, count: COUNT, page: [], errors: [], info: null, cpu: [], gpu: [] };
const addon = require('./build/addon.node');
const offscreen = mode === 'send' || mode === 'multi' || mode === 'tiles', COLS = +arg('cols', 2), ROWS = +arg('rows', 2);
process.on('uncaughtException', (e) => console.log('UNCAUGHT', e));
const gpuUtil = () => { try { const s = execSync('ioreg -r -d 1 -c IOAccelerator', { encoding: 'utf8' }); const m = s.match(/"Device Utilization %"=(\d+)/); return m ? +m[1] : null; } catch (e) { return null; } };
const procTime = () => { const pids = app.getAppMetrics().map((m) => [m.pid, m.type]); try { execSync('pgrep -x rx tx bridge4k syphon-bridge', { encoding: 'utf8' }); } catch (e) {}
  ['rx', 'tx', 'bridge4k', 'syphon-bridge'].forEach((n) => { try { execSync(`pgrep -x ${n}`, { encoding: 'utf8' }).trim().split('\n').forEach((p) => pids.push([+p, n])); } catch (e) {} });
  const map = new Map(pids), r = {}; execSync(`ps -o pid=,time= -p ${pids.map((p) => p[0]).join(',')}`, { encoding: 'utf8' }).trim().split('\n').forEach((l) => { const [p, t] = l.trim().split(/\s+/), [m, sec] = t.split(':'); r[p] = { type: map.get(+p), s: +m * 60 + +sec }; });
  return { t: Date.now(), r }; };
const avg = (a) => a.length ? a.reduce((x, y) => x + y, 0) / a.length : null;
let done = false; const wins = [], counts = {};
function finish() {
  done = true;
  const a = out.start, b = procTime(), el = (b.t - a.t) / 1000, perType = {}; let total = 0;
  Object.entries(b.r).forEach(([p, v]) => { const was = a.r[p]; if (!was) return; const pct = (v.s - was.s) / el * 100; perType[v.type] = +((perType[v.type] || 0) + pct).toFixed(1); total += pct; });
  out.gpu.splice(0, 2);
  const res = { mode, W, H, count: COUNT, cpuByProcess: perType, cpuTotal: +total.toFixed(1), gpuUtilAvg: avg(out.gpu), paint: out.paint, info: out.info, blit: addon.stats(), noTexture: out.noTexture || 0, counts, streams: wins.length, page: out.page.slice(-3), errors: out.errors.slice(0, 5) };
  fs.writeFileSync(path.join(__dirname, 'results', `${arg('name', mode)}.json`), JSON.stringify(res, null, 1));
  console.log(JSON.stringify(res)); app.exit(0);
}
ipcMain.on('stats', (e, o) => { if (o.fatal || o.error) { out.errors.push(o.fatal || o.error); console.log('PAGE', o); } else out.page.push(o); });
app.whenReady().then(async () => {
  if (offscreen && !addon.connectOut()) console.log('no external receiver');
  const paint = { n: 0, forwardMs: 0, t0: Date.now() }; out.paint = paint;
  const hook = (w, i) => { w.webContents.setFrameRate(FPS);
    w.webContents.on('paint', (ev) => { const t = ev.texture; if (!t) { out.noTexture = (out.noTexture || 0) + 1; return; }
      const s = t.textureInfo.handle.ioSurface;
      if (!out.info) out.info = { surface: addon.inspect(s), pixelFormat: t.textureInfo.pixelFormat, codedSize: t.textureInfo.codedSize, colorSpace: t.textureInfo.colorSpace };
      counts[i] = (counts[i] || 0) + 1; paint.n++; paint.forwardMs += mode === 'tiles' ? addon.forwardTiles(s, COLS, ROWS) : addon.forward(i, s); t.release(); }); };
  for (let i = 0; i < (mode === 'multi' ? 1 : COUNT); i++) {
    const w = new BrowserWindow({ width: offscreen ? W : 640, height: offscreen ? H : 360, show: !offscreen, useContentSize: true, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: false, sandbox: false, backgroundThrottling: false, webSecurity: mode !== 'post', ...(offscreen ? { offscreen: { useSharedTexture: true } } : {}) } });
    wins.push(w);
    w.webContents.on('console-message', (ev) => console.log('CONSOLE', ev.message));
    w.webContents.on('render-process-gone', (e, d) => out.errors.push('renderer gone ' + d.reason));
    if (offscreen) hook(w, i);
    if (mode === 'multi') { let next = 1;
      w.webContents.setWindowOpenHandler(() => ({ action: 'allow', overrideBrowserWindowOptions: { show: true, width: W, height: H, useContentSize: true, webPreferences: { offscreen: { useSharedTexture: true }, backgroundThrottling: false, sandbox: false, contextIsolation: false } } }));
      w.webContents.on('did-create-window', (child) => { wins.push(child); console.log('CHILD', 'offscreen', child.webContents.isOffscreen(), 'painting', child.webContents.isPainting && child.webContents.isPainting(), 'fps', child.webContents.getFrameRate(), child.getSize()); const idx = next; hook(child, next++); try { child.webContents.startPainting(); } catch (e) { console.log('sp', e.message); } setTimeout(() => console.log('CHILDLATER', idx, child.webContents.isPainting(), child.webContents.getFrameRate(), counts), 3000); });
      w.webContents.once('did-finish-load', () => setTimeout(() => w.webContents.executeJavaScript('openOutputs()', true), 1000)); }
    w.loadFile('page.html', { query: { mode, w: W, h: H, count: COUNT, cols: COLS, rows: ROWS } });
  }
  if (mode === 'recv') {
    const wc = wins[0].webContents; let imported = 0, importMs = 0, sendFail = 0, inflight = 0, dropped = 0, ready = false; out.paint = { get n() { return imported; } };
    ipcMain.on('stats', (e, o) => { if (o.ready) ready = true; });
    addon.listen((buf, sid, w, h, seq) => {
      if (!ready || done || wc.isDestroyed()) return; if (inflight > 1) { dropped++; return; }
      try {
      const t = performance.now(); inflight++;
      const imp = sharedTexture.importSharedTexture({ textureInfo: { pixelFormat: 'bgra', codedSize: { width: w, height: h }, visibleRect: { x: 0, y: 0, width: w, height: h }, handle: { ioSurface: buf } } });
      importMs += performance.now() - t; imported++;
      sharedTexture.sendSharedTexture({ frame: wc.mainFrame, importedSharedTexture: imp }).catch((e) => { sendFail++; out.errors.push(String(e)); }).finally(() => { inflight--; imp.release(); });
      } catch (e) { inflight--; out.errors.push('listen: ' + e.message); }
    });
    out.recvStats = () => ({ imported, importMs: imported ? importMs / imported : 0, sendFail, dropped });
  }
  setTimeout(() => { out.start = procTime(); }, 3000);
  const iv = setInterval(() => { const g = gpuUtil(); if (g != null) out.gpu.push(g); }, 1000);
  setTimeout(() => { clearInterval(iv); if (out.recvStats) out.info = out.recvStats(); paint.fps = paint.n / SECS; paint.forwardMs = paint.n ? paint.forwardMs / paint.n : 0; finish(); }, SECS * 1000);
});
