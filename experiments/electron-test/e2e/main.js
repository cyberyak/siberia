const { app, BrowserWindow } = require('electron'), http = require('http');
const url = process.env.PAGE, fake = !!process.env.FAKE_HOST;
const get = (p) => new Promise((res) => http.get('http://localhost:8766' + p, (r) => { let b = ''; r.on('data', (d) => b += d); r.on('end', () => res(b)); }).on('error', () => res('ERR')));
app.whenReady().then(async () => {
  const w = new BrowserWindow({ width: 1280, height: 800, show: true, webPreferences: { preload: __dirname + '/preload.js', sandbox: false, contextIsolation: false, backgroundThrottling: false } });
  const errs = []; w.webContents.on('console-message', (e) => { if (e.level === 'error') errs.push(e.message.slice(0, 140)); });
  await w.loadURL(url); await new Promise((r) => setTimeout(r, 3000));
  const js = (c) => w.webContents.executeJavaScript(c, true);
  await js(`document.getElementById('pgmSend').click()`); await new Promise((r) => setTimeout(r, 3000));
  let r = { label: await js(`document.getElementById('pgmSend').textContent`), state: await js(`document.getElementById('pgmState').textContent`), errs: [...new Set(errs)].slice(0, 4) };
  if (fake) { r.host = await js(`({ sends: hostLog.sends, sizes: [...hostLog.sizes] })`); await js(`document.getElementById('pgmSend').click()`); await new Promise((q) => setTimeout(q, 300)); r.stops = await js(`hostLog.stops`); }
  else { r.sources = await get('/sources'); await js(`document.getElementById('pgmSend').click()`); await new Promise((q) => setTimeout(q, 800)); r.afterStop = await get('/sources'); await new Promise((q) => setTimeout(q, 6000)); r.after6s = await get('/sources'); const names = (x) => { try { return JSON.parse(x).map((v) => v.name); } catch (e) { return x; } }; r = { label: r.label, errs: r.errs, during: names(r.sources), afterStop: names(r.afterStop), after6s: names(r.after6s) }; }
  console.log('RESULT ' + JSON.stringify(r)); app.exit(0);
});
