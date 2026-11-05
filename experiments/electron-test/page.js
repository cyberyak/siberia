// Test page. Modes: send/plain/readback draw coded 4K frames; recv draws frames from another app.
const q = new URLSearchParams(location.search), mode = q.get('mode'), W = +q.get('w'), H = +q.get('h');
const say = (o) => window.siberia.ipc.send('stats', o);
const VS = `@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{var p=array(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));return vec4f(p[i],0,1);}`;
const DRAW = VS + `struct U{frame:u32,ms:u32,w:f32,h:f32,ox:f32,oy:f32};@group(0)@binding(0) var<uniform> u:U;
@fragment fn fs(@builtin(position) q:vec4f)->@location(0) vec4f{
 let p=vec2f(q.x-u.ox,q.y-u.oy);
 if(p.y<32.0||p.y>=u.h-32.0){let cell=floor(u.w/40.0);let i=u32(floor(p.x/cell));var b=0u;
  if(i<16u){b=(u.frame>>i)&1u;}else if(i<40u){b=(u.ms>>(i-16u))&1u;}return vec4f(vec3f(f32(b)),1);}
 let t=f32(u.frame)/60.0;let x=p/vec2f(u.w,u.h);var v=0.0;for(var k=1.0;k<5.0;k+=1.0){v+=sin(x.x*k*9.0+t*k)*cos(x.y*k*7.0-t);}
 return vec4f(.5+.5*sin(v+t),.5+.5*sin(v*1.3),.5+.5*cos(v),1);}`;
(async () => {
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) return say({ fatal: 'no adapter' });
  const device = await adapter.requestDevice();
  device.onuncapturederror = (e) => say({ error: e.error.message });
  const c = document.getElementById('c'); c.width = W; c.height = H;
  c.style.width = mode === 'send' || mode === 'multi' || mode === 'tiles' ? W + 'px' : '100vw'; c.style.height = mode === 'send' || mode === 'multi' || mode === 'tiles' ? H + 'px' : '100vh';
  const ctx = c.getContext('webgpu'), format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: 'opaque', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const extra = [];
  window.openOutputs = () => { for (let i = 1; i < +q.get('count'); i++) { const w = window.open('about:blank', 'o' + i, 'popup'); if (!w) { say({ error: 'popup ' + i + ' blocked' }); continue; }
    w.document.body.style.cssText = 'margin:0;background:#000;overflow:hidden'; const cv = w.document.createElement('canvas'); cv.width = W; cv.height = H; cv.style.cssText = `display:block;width:${W}px;height:${H}px`; w.document.body.appendChild(cv);
    const x = cv.getContext('webgpu'); x.configure({ device, format, alphaMode: 'opaque' }); extra.push(x); const loop = () => { cv.style.opacity = cv.style.opacity === '0.999' ? '1' : '0.999'; w.requestAnimationFrame(loop); }; w.requestAnimationFrame(loop); } say({ opened: extra.length }); };
  const pass = (enc, view, pipe, bg, n = 3) => { const r = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] }); r.setPipeline(pipe); if (bg) r.setBindGroup(0, bg); r.draw(n); r.end(); };
  if (mode === 'recv') return recv(device, ctx, format, pass);
  const mod = device.createShaderModule({ code: DRAW });
  const pipe = device.createRenderPipeline({ layout: 'auto', vertex: { module: mod, entryPoint: 'vs' }, fragment: { module: mod, entryPoint: 'fs', targets: [{ format }] } });
  const tiles = mode === 'tiles' ? +q.get('cols') * +q.get('rows') : 1, cols = mode === 'tiles' ? +q.get('cols') : 1, rows = mode === 'tiles' ? +q.get('rows') : 1, tw = Math.floor(W / cols), th = Math.floor(H / rows);
  const ubs = [], bgs = []; for (let i = 0; i < tiles; i++) { const b = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }); ubs.push(b); bgs.push(device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: b } }] })); }
  const ub = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: ub } }] });
  const row = (W * 4 + 255) & ~255, buf = (mode === 'readback' || mode === 'post') ? device.createBuffer({ size: row * H, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }) : null;
  let posts = 0, bad = 0, frame = 0, busy = false, reads = 0, skipped = 0, copyMs = 0, frames = 0, t0 = performance.now();
  const data = buf && new Uint8Array(W * H * 4);
  const tick = () => {
    const ms = Date.now() % 16777216;
    const tex = ctx.getCurrentTexture(), enc = device.createCommandEncoder(), rp = enc.beginRenderPass({ colorAttachments: [{ view: tex.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] }); rp.setPipeline(pipe);
    for (let i = 0; i < tiles; i++) { const u = new ArrayBuffer(32), ox = (i % cols) * tw, oy = Math.floor(i / cols) * th; new Uint32Array(u, 0, 2).set([frame & 0xffff, ms]); new Float32Array(u, 8, 4).set([tw, th, ox, oy]); device.queue.writeBuffer(ubs[i], 0, u);
      rp.setViewport(ox, oy, tw, th, 0, 1); rp.setScissorRect(ox, oy, tw, th); rp.setBindGroup(0, bgs[i]); rp.draw(3); }
    rp.end();
    extra.forEach((x) => pass(enc, x.getCurrentTexture().createView(), pipe, bg));
    let read = false; if (buf) { if (busy) skipped++; else { busy = read = true; enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: row }, [W, H]); } }
    device.queue.submit([enc.finish()]);
    if (read) buf.mapAsync(GPUMapMode.READ).then(() => { const t = performance.now(), s = new Uint8Array(buf.getMappedRange()); for (let y = 0; y < H; y++) data.set(s.subarray(y * row, y * row + W * 4), y * W * 4); buf.unmap(); copyMs += performance.now() - t; reads++;
      if (mode === 'post') fetch(`http://localhost:8766/publish?name=spike&w=${W}&h=${H}`, { method: 'POST', headers: { 'X-Video-Rack': '1', 'Content-Type': 'application/octet-stream' }, body: data }).then((r) => { if (r.status === 204) posts++; else bad++; busy = false; }).catch((e) => { bad++; busy = false; }); else busy = false; });
    frame++; frames++;
    if (performance.now() - t0 > 2000) { const el = (performance.now() - t0) / 1000; say({ pageFps: frames / el, reads: reads / el, skipped: skipped / el, cpuCopyMs: reads ? copyMs / reads : 0, postsPerSec: posts / el, bad }); posts = 0; frames = reads = skipped = 0; copyMs = 0; t0 = performance.now(); }
    requestAnimationFrame(tick);
  };
  tick();
})().catch((e) => say({ fatal: String(e && e.stack || e) }));
function recv(device, ctx, format, pass) {
  const code = VS + `@group(0)@binding(0) var s:sampler;@group(0)@binding(1) var t:texture_external;
  @fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f{return textureSampleBaseClampToEdge(t,s,p.xy/vec2f(${W}.0,${H}.0));}
  @fragment fn bars(@builtin(position) p:vec4f)->@location(0) vec4f{let cell=floor(${W}.0/40.0);let x=(floor(p.x)*cell+cell/2.0)/${W}.0;let y=select(16.0/${H}.0,1.0-16.0/${H}.0,p.y>1.0);return textureSampleBaseClampToEdge(t,s,vec2f(x,y));}`;
  const mod = device.createShaderModule({ code });
  const mk = (entryPoint, f) => device.createRenderPipeline({ layout: 'auto', vertex: { module: mod, entryPoint: 'vs' }, fragment: { module: mod, entryPoint, targets: [{ format: f }] } });
  const pipe = mk('fs', format), bars = mk('bars', 'rgba8unorm'), sampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
  const small = device.createTexture({ size: [40, 2], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const buf = device.createBuffer({ size: 512, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  let busy = false, shown = 0, checked = 0, torn = 0, skippedF = 0, last = -1, lat = [], t0 = performance.now(), recvMs = 0, closed = 0;
  const decode = (d, off) => { let lo = 0, hi = 0; for (let i = 0; i < 40; i++) if (d[off + i * 4 + 1] > 128) { if (i < 16) lo |= 1 << i; else hi |= 2 ** (i - 16); } return [lo, hi]; };
  window.siberia.sharedTexture.setSharedTextureReceiver(async ({ importedSharedTexture: imp }) => {
    const t = performance.now();
    const vf = imp.getVideoFrame();
    const ext = device.importExternalTexture({ source: vf });
    const bind = (p) => device.createBindGroup({ layout: p.getBindGroupLayout(0), entries: [{ binding: 0, resource: sampler }, { binding: 1, resource: ext }] });
    const enc = device.createCommandEncoder();
    pass(enc, ctx.getCurrentTexture().createView(), pipe, bind(pipe));
    let read = false; if (!busy) { busy = read = true; pass(enc, small.createView(), bars, bind(bars)); enc.copyTextureToBuffer({ texture: small }, { buffer: buf, bytesPerRow: 256 }, [40, 2]); }
    device.queue.submit([enc.finish()]);
    recvMs += performance.now() - t; shown++;
    vf.close(); imp.release();
    if (read) buf.mapAsync(GPUMapMode.READ).then(() => { const d = new Uint8Array(buf.getMappedRange()).slice(); buf.unmap(); busy = false;
      const a = decode(d, 0), b = decode(d, 256); checked++; if (a[0] !== b[0] || a[1] !== b[1]) torn++;
      if (last >= 0) { const g = (a[0] - last) & 0xffff; if (g === 0) skippedF += 0; } last = a[0];
      const l = (Date.now() % 16777216 - a[1]) & 0xffffff; if (l < 2000) lat.push(l); });
    if (performance.now() - t0 > 2000) { const el = (performance.now() - t0) / 1000; lat.sort((x, y) => x - y); say({ recvFps: shown / el, checked, torn, latP50: lat[lat.length >> 1], latP99: lat[Math.floor(lat.length * .99)], jsMs: shown ? recvMs / shown : 0, lastFrame: last }); shown = checked = torn = 0; recvMs = 0; lat = []; t0 = performance.now(); }
  });
  say({ ready: 1 });
}
