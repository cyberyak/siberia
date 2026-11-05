// Tests the binding under plain Node, so it needs no window or GPU work.
// usage: node bindings/electron/test.js <addon.node>
const assert = require('assert');
const path = require('path');
const siberia = require(path.resolve(process.argv[2]));
const tag = `binding ${process.pid}`;
let passed = 0, failed = 0;
const test = (name, fn) => { try { fn(); passed++; } catch (e) { failed++; console.log(`FAIL ${name}: ${e.message}`); } };
const pixels = (w, h, r, g, b) => { const p = new Uint8Array(w * h * 4); for (let i = 0; i < p.length; i += 4) p.set([r, g, b, 255], i); return p; };
const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

test('a stream appears in the list and leaves it', () => {
  const name = `${tag} list`, pub = siberia.publish(name, 16, 16, 1, 'RGBA');
  assert(siberia.list().some((s) => s.name === name && s.pid === process.pid));
  siberia.closePublisher(pub);
  assert(!siberia.list().some((s) => s.name === name));
});

test('a name can only be used once', () => {
  const name = `${tag} once`, pub = siberia.publish(name, 16, 16, 1, 'RGBA');
  assert.throws(() => siberia.publish(name, 16, 16, 1, 'RGBA'), /in use/);
  siberia.closePublisher(pub);
  siberia.closePublisher(siberia.publish(name, 16, 16, 1, 'RGBA'));
});

test('subscribing to nothing returns null', () => assert.strictEqual(siberia.subscribe(`${tag} nobody`), null));

test('a stream counts its subscribers and refuses too many', () => {
  const name = `${tag} count`, pub = siberia.publish(name, 16, 16, 1, 'RGBA');
  assert.strictEqual(siberia.subscribers(pub), 0);
  const sub = siberia.subscribe(name);
  assert.strictEqual(siberia.subscribers(pub), 1);
  assert.throws(() => siberia.subscribe(name), /no room/);
  siberia.closeSubscriber(sub);
  for (let i = 0; i < 100 && siberia.subscribers(pub); i++) sleep(5);
  assert.strictEqual(siberia.subscribers(pub), 0);
  siberia.closePublisher(pub);
});

test('RGBA frames arrive unchanged', () => {
  const name = `${tag} rgba`, pub = siberia.publish(name, 8, 4, 1, 'RGBA'), sub = siberia.subscribe(name), out = new Uint8Array(8 * 4 * 4);
  assert.strictEqual(siberia.read(sub, 8, 4, out), null, 'nothing yet');
  assert.strictEqual(siberia.write(pub, pixels(8, 4, 10, 20, 30)), true);
  assert.strictEqual(siberia.read(sub, 8, 4, out), true);
  assert.deepStrictEqual([...out.slice(0, 4)], [10, 20, 30, 255]);
  assert.strictEqual(siberia.read(sub, 8, 4, out), null, 'the same frame is not new');
  siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('BGRA frames are converted to RGBA', () => {
  const name = `${tag} bgra`, pub = siberia.publish(name, 8, 4, 1, 'BGRA'), sub = siberia.subscribe(name), out = new Uint8Array(8 * 4 * 4);
  siberia.write(pub, pixels(8, 4, 10, 20, 30));
  siberia.read(sub, 8, 4, out);
  assert.deepStrictEqual([...out.slice(0, 4)], [30, 20, 10, 255]);
  siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('read scales to the size asked for, including odd row sizes', () => {
  const name = `${tag} scale`, w = 33, h = 17, pub = siberia.publish(name, w, h, 1, 'RGBA'), sub = siberia.subscribe(name), left = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) left.set(x < w / 2 ? [200, 0, 0, 255] : [0, 0, 200, 255], (y * w + x) * 4);
  siberia.write(pub, left);
  const out = new Uint8Array(4 * 2 * 4);
  assert.strictEqual(siberia.read(sub, 4, 2, out), true);
  assert.deepStrictEqual([...out.slice(0, 4)], [200, 0, 0, 255], 'left stays left');
  assert.deepStrictEqual([...out.slice(12, 16)], [0, 0, 200, 255], 'right stays right');
  siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('a buffer that is too small is refused', () => {
  const name = `${tag} small`, pub = siberia.publish(name, 8, 8, 1, 'RGBA'), sub = siberia.subscribe(name);
  assert.strictEqual(siberia.write(pub, new Uint8Array(4)), false);
  siberia.write(pub, pixels(8, 8, 1, 2, 3));
  assert.strictEqual(siberia.read(sub, 8, 8, new Uint8Array(4)), null);
  siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('acquire holds a frame until release, and sample copies from it', () => {
  const name = `${tag} hold`, pub = siberia.publish(name, 16, 8, 1, 'RGBA'), sub = siberia.subscribe(name);
  assert.strictEqual(siberia.acquire(sub), null, 'nothing yet');
  siberia.write(pub, pixels(16, 8, 40, 50, 60));
  const frame = siberia.acquire(sub);
  assert.strictEqual(frame.width, 16); assert.strictEqual(frame.height, 8); assert.strictEqual(frame.pixelFormat, 'rgba'); assert.strictEqual(frame.sequence, 1);
  const small = new Uint8Array(4 * 2 * 4);
  assert.strictEqual(siberia.sample(sub, frame.surface, 4, 2, small), true);
  assert.deepStrictEqual([...small.slice(0, 4)], [40, 50, 60, 255]);
  siberia.write(pub, pixels(16, 8, 1, 1, 1));
  assert.strictEqual(siberia.acquire(sub), null, 'one frame at a time');
  siberia.release(sub);
  assert.strictEqual(siberia.acquire(sub).sequence, 2);
  siberia.release(sub); siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('BGRA frames report their format', () => {
  const name = `${tag} format`, pub = siberia.publish(name, 8, 8, 1), sub = siberia.subscribe(name);
  siberia.write(pub, pixels(8, 8, 1, 2, 3));
  assert.strictEqual(siberia.acquire(sub).pixelFormat, 'bgra');
  siberia.release(sub); siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('a subscriber hears when the stream closes', () => {
  const name = `${tag} closed`, pub = siberia.publish(name, 8, 8, 1, 'RGBA'), sub = siberia.subscribe(name);
  siberia.closePublisher(pub);
  assert.strictEqual(siberia.acquire(sub), 'closed');
  assert.strictEqual(siberia.read(sub, 8, 8, new Uint8Array(256)), 'closed');
  siberia.closeSubscriber(sub);
});

test('a stream can be reopened under the same name', () => {
  const name = `${tag} again`;
  let pub = siberia.publish(name, 8, 8, 1, 'RGBA');
  siberia.closePublisher(pub);
  pub = siberia.publish(name, 8, 8, 1, 'RGBA');
  const sub = siberia.subscribe(name), out = new Uint8Array(256);
  siberia.write(pub, pixels(8, 8, 7, 8, 9));
  assert.strictEqual(siberia.read(sub, 8, 8, out), true);
  siberia.closeSubscriber(sub); siberia.closePublisher(pub);
});

test('the shared clock runs forward', () => { const a = siberia.now(); sleep(2); assert(siberia.now() > a); });
test('stats report what was written', () => assert(siberia.stats().sends > 0));

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
