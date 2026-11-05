const { app, BrowserWindow } = require('electron'), http = require('http');
const url = process.env.PAGE, fake = !!process.env.FAKE_HOST;
const get = (p) => new Promise((res) => http.get('http://localhost:8766' + p, (r) => { let b = ''; r.on('data', (d) => b += d); r.on('end', () => res(b)); }).on('error', () => res('ERR')));
app.whenReady().then(async () => {
  const w = new BrowserWindow({ width: 1280, height: 800, show: true, webPreferences: { preload: __dirname + '/preload.js', sandbox: false, contextIsolation: false, backgroundThrottling: false } });
  const errs = []; w.webContents.on('console-message', (e) => { if (e.level === 'error') errs.push(e.message.slice(0, 140)); });
  await w.loadURL(url); await new Promise((r) => setTimeout(r, 3000));
  const js = (c) => w.webContents.executeJavaScript(c, true);
  const wait = (ms) => new Promise((q) => setTimeout(q, ms)), click = () => js(`document.getElementById('pgmSend').click()`);
  let linger = 0, runs = 16, gone = 0; for (let i = 0; i < runs; i++) { await click(); await wait(600); await click(); await wait(800); const n = await get('/sources'); if (n.includes('Video Rack On air')) { linger++; await wait(3500); if (!(await get('/sources')).includes('Video Rack On air')) gone++; } }
  console.log('RESULT ' + JSON.stringify({ runs, linger, clearedLater: gone, errs: [...new Set(errs)].slice(0, 3) })); app.exit(0);
});
