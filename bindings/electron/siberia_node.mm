// Node-API binding for Electron's main process. See bindings/electron/README.md.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <node_api.h>
#include <unistd.h>
#include <string.h>
#include <atomic>
#include <vector>
#include "siberia.h"

struct Publisher { siberia_publisher *p; NSMutableArray<id<MTLTexture>> *textures; std::atomic<bool> busy{false}; };
struct Subscriber { siberia_subscriber *s; };
static std::vector<Publisher *> publishers;
static std::vector<Subscriber *> subscribers;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static napi_threadsafe_function onSent;
static std::atomic<uint64_t> sends{0}, sendNs{0}, sendMaxNs{0}, gpuNs{0}, busyDrops{0}, slotDrops{0};

#define ARGS(n) size_t argc = n; napi_value argv[n]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL)
static napi_value number(napi_env env, double v) { napi_value r; napi_create_double(env, v, &r); return r; }
static napi_value boolean(napi_env env, bool v) { napi_value r; napi_get_boolean(env, v, &r); return r; }
static napi_value null_(napi_env env) { napi_value r; napi_get_null(env, &r); return r; }
static int32_t toInt(napi_env env, napi_value v) { int32_t i = 0; napi_get_value_int32(env, v, &i); return i; }
static double toNum(napi_env env, napi_value v) { double d = 0; napi_get_value_double(env, v, &d); return d; }
static void toStr(napi_env env, napi_value v, char *out, size_t size) { size_t n; napi_get_value_string_utf8(env, v, out, size, &n); }
static void field(napi_env env, napi_value o, const char *k, napi_value v) { napi_set_named_property(env, o, k, v); }
static napi_value fail(napi_env env, int status) { napi_throw_error(env, NULL, siberia_status_text(status)); return NULL; }
static IOSurfaceRef surfaceArg(napi_env env, napi_value v) { void *d; size_t l = 0; napi_get_buffer_info(env, v, &d, &l); return l >= sizeof(IOSurfaceRef) ? *(IOSurfaceRef *)d : NULL; }

static void ensureMetal() { if (!device) { device = MTLCreateSystemDefaultDevice(); queue = [device newCommandQueue]; } }
static id<MTLTexture> wrap(IOSurfaceRef s) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:IOSurfaceGetWidth(s) height:IOSurfaceGetHeight(s) mipmapped:NO];
  d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModeShared;
  return [device newTextureWithDescriptor:d iosurface:s plane:0];
}

// publish(name, width, height, maxSubscribers, format) -> handle. format is 'BGRA' or 'RGBA'.
static napi_value publish(napi_env env, napi_callback_info info) {
  ARGS(5); char name[SIBERIA_NAME_MAX * 4], format[8] = "BGRA"; toStr(env, argv[0], name, sizeof name);
  napi_valuetype type = napi_undefined; if (argc > 4) napi_typeof(env, argv[4], &type);
  if (type == napi_string) toStr(env, argv[4], format, sizeof format);
  siberia_format f = { (uint32_t)toInt(env, argv[1]), (uint32_t)toInt(env, argv[2]), strcmp(format, "RGBA") ? (uint32_t)'BGRA' : (uint32_t)'RGBA', 4, 0 };
  auto *p = new Publisher(); int r = siberia_publisher_create(name, &f, toInt(env, argv[3]), &p->p);
  if (r) { delete p; return fail(env, r); }
  publishers.push_back(p);
  return number(env, (double)publishers.size() - 1);
}

// onSent(fn): fn(token) runs on the main thread when a send's GPU copy has finished.
static void callSent(napi_env env, napi_value fn, void *, void *data) {
  if (env && fn) { napi_value args[1] = { number(env, (double)(uintptr_t)data) }, global; napi_get_undefined(env, &global); napi_call_function(env, global, fn, 1, args, NULL); }
}
static napi_value setOnSent(napi_env env, napi_callback_info info) {
  ARGS(1); napi_value name; napi_create_string_utf8(env, "siberia.sent", NAPI_AUTO_LENGTH, &name);
  napi_create_threadsafe_function(env, argv[0], NULL, name, 0, 1, NULL, NULL, NULL, callSent, &onSent);
  napi_unref_threadsafe_function(env, onSent);
  return NULL;
}

// send(handles, surface, cols, rows, token) splits one surface into streams and copies each to its
// buffer without blocking. True means keep the surface until onSent fires. False means nothing is
// using it: no stream had a subscriber, or the last copy is still running, so that frame is dropped.
static napi_value send(napi_env env, napi_callback_info info) {
  ARGS(5);
  uint64_t t0 = siberia_now_ns();
  IOSurfaceRef src = surfaceArg(env, argv[1]); int cols = toInt(env, argv[2]), rows = toInt(env, argv[3]); uintptr_t token = (uintptr_t)toNum(env, argv[4]);
  uint32_t count = 0; napi_get_array_length(env, argv[0], &count);
  if (!src || !count || cols < 1 || rows < 1) return boolean(env, false);
  int tw = (int)IOSurfaceGetWidth(src) / cols, th = (int)IOSurfaceGetHeight(src) / rows;
  struct Item { Publisher *pub; siberia_frame frame; };
  auto *items = new std::vector<Item>();
  id<MTLCommandBuffer> cb = nil; id<MTLBlitCommandEncoder> blit = nil; id<MTLTexture> source = nil;
  @autoreleasepool {
    for (uint32_t i = 0; i < count; i++) {
      napi_value h; napi_get_element(env, argv[0], i, &h); Publisher *p = publishers[(size_t)toInt(env, h)];
      siberia_demand d; siberia_publisher_demand(p->p, &d);
      if (!d.subscribers) continue;
      bool expected = false;
      if (!p->busy.compare_exchange_strong(expected, true)) { busyDrops++; continue; }
      siberia_frame f;
      if (siberia_publisher_begin(p->p, &f) != SIBERIA_OK) { p->busy = false; slotDrops++; continue; }
      if (!p->textures) { ensureMetal(); p->textures = [NSMutableArray array]; for (int n = 0; n < siberia_publisher_slot_count(p->p); n++) [p->textures addObject:wrap(siberia_publisher_surface(p->p, n))]; }
      if (!cb) { source = wrap(src); cb = [queue commandBuffer]; blit = [cb blitCommandEncoder]; }
      [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((i % cols) * tw, (i / cols) * th, 0) sourceSize:MTLSizeMake(tw, th, 1) toTexture:p->textures[f.slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      items->push_back({ p, f });
    }
    if (!cb) { delete items; return boolean(env, false); }
    [blit endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      gpuNs += siberia_now_ns() - t0;
      for (auto &it : *items) { siberia_publisher_commit(it.pub->p, &it.frame); it.pub->busy = false; }
      delete items;
      if (onSent) napi_call_threadsafe_function(onSent, (void *)token, napi_tsfn_nonblocking);
    }];
    [cb commit];
  }
  uint64_t took = siberia_now_ns() - t0, was = sendMaxNs.load();
  sends++; sendNs += took; if (took > was) sendMaxNs = took;
  return boolean(env, true);
}

// subscribers(handle) -> how many are connected. Lets an app skip rendering or readback for nobody.
static napi_value subscriberCount(napi_env env, napi_callback_info info) {
  ARGS(1); siberia_demand d; siberia_publisher_demand(publishers[(size_t)toInt(env, argv[0])]->p, &d);
  return number(env, d.subscribers);
}

// write(handle, pixels): copies CPU pixels into the stream. False drops the frame.
static napi_value write_(napi_env env, napi_callback_info info) {
  ARGS(2); Publisher *p = publishers[(size_t)toInt(env, argv[0])]; void *data; size_t bytes; napi_get_typedarray_info(env, argv[1], NULL, &bytes, &data, NULL, NULL);
  siberia_frame f; uint64_t t0 = siberia_now_ns();
  if (siberia_publisher_begin(p->p, &f) != SIBERIA_OK) { slotDrops++; return boolean(env, false); }
  size_t w = IOSurfaceGetWidth(f.surface), h = IOSurfaceGetHeight(f.surface), row = IOSurfaceGetBytesPerRow(f.surface);
  if (bytes < w * h * 4) { siberia_publisher_cancel(p->p, &f); return boolean(env, false); }
  IOSurfaceLock(f.surface, 0, NULL);
  uint8_t *out = (uint8_t *)IOSurfaceGetBaseAddress(f.surface); const uint8_t *in = (const uint8_t *)data;
  if (row == w * 4) memcpy(out, in, w * h * 4); else for (size_t y = 0; y < h; y++) memcpy(out + y * row, in + y * w * 4, w * 4);
  IOSurfaceUnlock(f.surface, 0, NULL);
  siberia_publisher_commit(p->p, &f);
  uint64_t took = siberia_now_ns() - t0; sends++; sendNs += took; if (took > sendMaxNs.load()) sendMaxNs = took;
  return boolean(env, true);
}

// Copies a surface out as RGBA, scaling with nearest-neighbour sampling if it has to.
static void copyOut(siberia_subscriber *s, IOSurfaceRef surface, int w, int h, void *data) {
  siberia_format fmt; siberia_subscriber_format(s, &fmt);
  int sw = (int)fmt.width, sh = (int)fmt.height, swap = fmt.pixel_format != 'RGBA'; size_t row = IOSurfaceGetBytesPerRow(surface);
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
  const uint8_t *in = (const uint8_t *)IOSurfaceGetBaseAddress(surface); uint32_t *out = (uint32_t *)data;
  for (int y = 0; y < h; y++) {
    const uint32_t *src = (const uint32_t *)(in + (size_t)(sh == h ? y : (int)(((int64_t)y * 2 + 1) * sh / (h * 2))) * row); uint32_t *dst = out + (size_t)y * w;
    if (sw == w && !swap) { memcpy(dst, src, (size_t)w * 4); continue; }
    for (int x = 0; x < w; x++) { uint32_t p = src[sw == w ? x : (int)(((int64_t)x * 2 + 1) * sw / (w * 2))]; dst[x] = swap ? (p & 0xff00ff00u) | (p & 0xffu) << 16 | (p >> 16 & 0xffu) : p; }
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
}

// read(sub, width, height, pixels): copies the newest frame out as RGBA.
// Returns true, null with nothing new, or 'closed'.
static napi_value read_(napi_env env, napi_callback_info info) {
  ARGS(4); Subscriber *s = subscribers[(size_t)toInt(env, argv[0])]; int w = toInt(env, argv[1]), h = toInt(env, argv[2]); void *data; size_t bytes; napi_get_typedarray_info(env, argv[3], NULL, &bytes, &data, NULL, NULL);
  if (w < 1 || h < 1 || bytes < (size_t)w * h * 4) return null_(env);
  siberia_frame f; int r = siberia_subscriber_acquire(s->s, &f);
  if (r == SIBERIA_CLOSED) { napi_value v; napi_create_string_utf8(env, "closed", NAPI_AUTO_LENGTH, &v); return v; }
  if (r != SIBERIA_OK) return null_(env);
  copyOut(s->s, f.surface, w, h, data);
  siberia_subscriber_release(s->s);
  return boolean(env, true);
}

// sample(sub, surface, width, height, pixels): a small RGBA copy of a frame you already hold.
static napi_value sample(napi_env env, napi_callback_info info) {
  ARGS(5); Subscriber *s = subscribers[(size_t)toInt(env, argv[0])]; IOSurfaceRef surface = surfaceArg(env, argv[1]); int w = toInt(env, argv[2]), h = toInt(env, argv[3]); void *data; size_t bytes; napi_get_typedarray_info(env, argv[4], NULL, &bytes, &data, NULL, NULL);
  if (!surface || w < 1 || h < 1 || bytes < (size_t)w * h * 4) return boolean(env, false);
  copyOut(s->s, surface, w, h, data);
  return boolean(env, true);
}

// subscribe(name) -> handle, or null if the stream isn't there yet
static napi_value subscribe(napi_env env, napi_callback_info info) {
  ARGS(1); char name[SIBERIA_NAME_MAX * 4]; toStr(env, argv[0], name, sizeof name);
  auto *s = new Subscriber(); int r = siberia_subscriber_connect(name, &s->s);
  if (r == SIBERIA_ERR_NOT_FOUND) { delete s; return null_(env); }
  if (r) { delete s; return fail(env, r); }
  subscribers.push_back(s);
  return number(env, (double)subscribers.size() - 1);
}

// acquire(handle) -> { surface, width, height, sequence, timeNs }, null with nothing new, or 'closed'
static napi_value acquire(napi_env env, napi_callback_info info) {
  ARGS(1); Subscriber *s = subscribers[(size_t)toInt(env, argv[0])]; siberia_frame f;
  int r = siberia_subscriber_acquire(s->s, &f);
  if (r == SIBERIA_CLOSED) { napi_value v; napi_create_string_utf8(env, "closed", NAPI_AUTO_LENGTH, &v); return v; }
  if (r != SIBERIA_OK) return null_(env);
  napi_value o, buffer; void *data; napi_create_object(env, &o);
  napi_create_buffer_copy(env, sizeof f.surface, &f.surface, &data, &buffer);
  field(env, o, "surface", buffer);
  field(env, o, "width", number(env, (double)IOSurfaceGetWidth(f.surface)));
  field(env, o, "height", number(env, (double)IOSurfaceGetHeight(f.surface)));
  field(env, o, "sequence", number(env, (double)f.sequence));
  siberia_format fmt; siberia_subscriber_format(s->s, &fmt);
  napi_value pf; napi_create_string_utf8(env, fmt.pixel_format == 'RGBA' ? "rgba" : "bgra", NAPI_AUTO_LENGTH, &pf); field(env, o, "pixelFormat", pf);
  field(env, o, "timeNs", number(env, (double)f.time_ns));
  return o;
}
static napi_value release(napi_env env, napi_callback_info info) { ARGS(1); siberia_subscriber_release(subscribers[(size_t)toInt(env, argv[0])]->s); return NULL; }
static napi_value reportRefresh(napi_env env, napi_callback_info info) { ARGS(2); siberia_subscriber_report_refresh(subscribers[(size_t)toInt(env, argv[0])]->s, (uint64_t)toNum(env, argv[1])); return NULL; }
// Waits for a copy still running. Handles stop working after these.
static napi_value closePublisher(napi_env env, napi_callback_info info) {
  ARGS(1); size_t i = (size_t)toInt(env, argv[0]); Publisher *p = publishers[i];
  if (!p) return NULL;
  for (int t = 0; t < 200 && p->busy; t++) usleep(1000);
  siberia_publisher_destroy(p->p); publishers[i] = NULL; delete p;
  return NULL;
}
static napi_value closeSubscriber(napi_env env, napi_callback_info info) {
  ARGS(1); size_t i = (size_t)toInt(env, argv[0]); Subscriber *s = subscribers[i];
  if (s) { siberia_subscriber_close(s->s); subscribers[i] = NULL; delete s; }
  return NULL;
}
static napi_value now(napi_env env, napi_callback_info) { return number(env, (double)siberia_now_ns()); }

static napi_value list(napi_env env, napi_callback_info) {
  siberia_stream_info info[64]; int n = siberia_list(info, 64); napi_value out; napi_create_array(env, &out);
  for (int i = 0; i < n; i++) { napi_value o, name; napi_create_object(env, &o); napi_create_string_utf8(env, info[i].name, NAPI_AUTO_LENGTH, &name); field(env, o, "name", name); field(env, o, "pid", number(env, info[i].pid)); napi_set_element(env, out, (uint32_t)i, o); }
  return out;
}

static napi_value stats(napi_env env, napi_callback_info) {
  napi_value o; napi_create_object(env, &o); double n = sends ? (double)sends.load() : 1;
  field(env, o, "sends", number(env, (double)sends.load()));
  field(env, o, "sendCallMs", number(env, (double)sendNs.load() / n / 1e6));
  field(env, o, "sendCallMaxMs", number(env, (double)sendMaxNs.load() / 1e6));
  field(env, o, "gpuCopyMs", number(env, (double)gpuNs.load() / n / 1e6));
  field(env, o, "busyDrops", number(env, (double)busyDrops.load()));
  field(env, o, "slotDrops", number(env, (double)slotDrops.load()));
  return o;
}

NAPI_MODULE_INIT() {
#define EXPORT(n, fn) { napi_value f; napi_create_function(env, n, NAPI_AUTO_LENGTH, fn, NULL, &f); napi_set_named_property(env, exports, n, f); }
  EXPORT("publish", publish) EXPORT("subscribers", subscriberCount) EXPORT("write", write_) EXPORT("read", read_) EXPORT("sample", sample) EXPORT("onSent", setOnSent) EXPORT("send", send) EXPORT("subscribe", subscribe) EXPORT("acquire", acquire) EXPORT("release", release) EXPORT("reportRefresh", reportRefresh) EXPORT("closePublisher", closePublisher) EXPORT("closeSubscriber", closeSubscriber) EXPORT("now", now) EXPORT("list", list) EXPORT("stats", stats)
  return exports;
}
