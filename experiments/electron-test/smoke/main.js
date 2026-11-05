// Boots the real VRS page, unmodified, in an offscreen shared-texture window.
const { app, BrowserWindow } = require('electron'); const fs = require('fs');
const page = process.env.HOME + '/Projects/vrs/lzx-rack.html';
let paints = 0, noTex = 0, info = null; const logs = [];
app.whenReady().then(async () => {
  const w = new BrowserWindow({ width: 1920, height: 1080, show: false, useContentSize: true, webPreferences: { backgroundThrottling: false, offscreen: { useSharedTexture: true } } });
  w.webContents.setFrameRate(60);
  w.webContents.on('console-message', (e) => { if (e.level === 'error' || e.level === 'warning') logs.push(e.message.slice(0, 160)); });
  w.webContents.on('paint', (ev) => { const t = ev.texture; if (!t) { noTex++; return; } paints++; if (!info) info = { size: t.textureInfo.codedSize, format: t.textureInfo.pixelFormat }; t.release(); });
  await w.loadFile(page);
  await new Promise((r) => setTimeout(r, 9000));
  const base = paints, r = await w.webContents.executeJavaScript(`({ gpu: !!navigator.gpu, stats: document.getElementById('stats') && document.getElementById('stats').textContent, title: document.title, canvases: document.querySelectorAll('canvas').length, big: [...document.querySelectorAll('canvas')].map((c) => c.width + 'x' + c.height).slice(0, 4) })`);
  await new Promise((r) => setTimeout(r, 4000));
  const res = { ...r, paintFps: (paints - base) / 4, noTex, info, logs: [...new Set(logs)].slice(0, 6) };
  fs.writeFileSync(__dirname + '/../results/smoke.json', JSON.stringify(res, null, 1)); console.log(JSON.stringify(res)); app.exit(0);
});
