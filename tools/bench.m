// Real 4K Metal rendering between two processes.
//   bench pub <stream> <w> <h> <secs> <shared|messages|paced|demand> [budget_ms]
//   bench sub <stream> <secs> <shared|messages|paced>
// "messages" adds what most sharing systems do, a wake-up message per frame, for comparison.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach.h>
#import <mach/mach_time.h>
#import <servers/bootstrap.h>
#import <sys/resource.h>
#import "siberia.h"

static NSString *SRC = @"#include <metal_stdlib>\nusing namespace metal; struct V{float4 p [[position]];};"
 "vertex V vs(uint i [[vertex_id]]){float2 q[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};V o;o.p=float4(q[i],0,1);return o;}"
 "fragment float4 art(V i [[stage_in]], constant float &t [[buffer(0)]]){float2 x=i.p.xy/float2(3840,2160);float v=0;for(float k=1;k<5;k+=1)v+=sin(x.x*k*9+t*k)*cos(x.y*k*7-t);return float4(.5+.5*sin(v+t),.5+.5*sin(v*1.3),.5+.5*cos(v),1);}"
 "fragment float4 show(V i [[stage_in]], texture2d<float> s [[texture(0)]]){constexpr sampler n(filter::linear);return s.sample(n,i.p.xy/float2(s.get_width(),s.get_height()));}";

static uint64_t ms(double v) { return (uint64_t)(v * 1e6); }
static void sleep_until(uint64_t ns) { uint64_t now = siberia_now_ns(); if (ns > now) { struct timespec t = { (time_t)((ns - now) / 1000000000ull), (long)((ns - now) % 1000000000ull) }; nanosleep(&t, NULL); } }
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> d, id<MTLLibrary> l, NSString *fn) { MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new]; p.vertexFunction = [l newFunctionWithName:@"vs"]; p.fragmentFunction = [l newFunctionWithName:fn]; p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; return [d newRenderPipelineStateWithDescriptor:p error:nil]; }
static id<MTLTexture> wrap(id<MTLDevice> d, IOSurfaceRef s) { MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:IOSurfaceGetWidth(s) height:IOSurfaceGetHeight(s) mipmapped:NO]; t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; t.storageMode = MTLStorageModeShared; return [d newTextureWithDescriptor:t iosurface:s plane:0]; }

typedef struct { integer_t mach, unix_, csw; double cpu; } usage;
static usage now_usage(void) { task_events_info_data_t e; mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT; task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&e, &n); struct rusage r; getrusage(RUSAGE_SELF, &r); usage u = { e.syscalls_mach, e.syscalls_unix, e.csw, r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6 }; return u; }
static void report(const char *who, const char *mode, usage a, usage b, double secs, long frames, const char *extra) {
  double f = frames ? (double)frames : 1;
  printf("{\"who\":\"%s\",\"mode\":\"%s\",\"frames\":%ld,\"fps\":%.1f,\"cpu\":%.2f,\"machPerFrame\":%.1f,\"unixPerFrame\":%.1f,\"switchesPerFrame\":%.1f%s}\n", who, mode, frames, frames / secs, (b.cpu - a.cpu) / secs * 100, (b.mach - a.mach) / f, (b.unix_ - a.unix_) / f, (b.csw - a.csw) / f, extra);
}
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
typedef struct { mach_msg_header_t h; mach_msg_trailer_t t; } wake;
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

static int publisher(int c, char **a) {
  const char *name = a[2]; int w = atoi(a[3]), h = atoi(a[4]); double secs = atof(a[5]); const char *mode = a[6]; double budget = c > 7 ? atof(a[7]) : 8;
  int paced = !strcmp(mode, "paced"), msgs = !strcmp(mode, "messages"), demand = !strcmp(mode, "demand");
  siberia_format f = { (uint32_t)w, (uint32_t)h, 'BGRA', 4, 0 }; siberia_publisher *p; int r = siberia_publisher_create(name, &f, 2, &p); if (r) { fprintf(stderr, "%s\n", siberia_status_text(r)); return 2; }
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLLibrary> lib = [d newLibraryWithSource:SRC options:nil error:nil]; id<MTLRenderPipelineState> ps = pipeline(d, lib, @"art");
  NSMutableArray *tex = [NSMutableArray array]; for (int i = 0; i < siberia_publisher_slot_count(p); i++) [tex addObject:wrap(d, siberia_publisher_surface(p, i))];
  mach_port_t to = MACH_PORT_NULL; char svc[128]; snprintf(svc, sizeof svc, "com.cyberyak.siberia.bench.%s", name);
  uint64_t period = 16666667, next = siberia_now_ns(); siberia_demand dm;
  if (!demand) for (int i = 0; i < 1000; i++) { siberia_publisher_demand(p, &dm); if (dm.subscribers) break; usleep(10000); }
  if (msgs) for (int i = 0; i < 500 && !to; i++) { if (bootstrap_look_up(bootstrap_port, svc, &to)) { to = MACH_PORT_NULL; usleep(10000); } }
  usleep(500000);
  usage u0 = now_usage(); uint64_t t0 = siberia_now_ns(), end = t0 + (uint64_t)(secs * 1e9), bns = 0, cns = 0; long frames = 0, skipped = 0, slept = 0; double rsum = 0; next = t0;
  float t = 0;
  while (siberia_now_ns() < end) {
    if (paced) { siberia_publisher_demand(p, &dm); uint64_t target = dm.next_refresh_ns ? dm.next_refresh_ns - ms(budget) : next; while (target + period / 2 < next) target += period; sleep_until(target); next = target + period; }
    else { sleep_until(next); next += period; }
    if (demand) { siberia_publisher_demand(p, &dm); if (!dm.subscribers) { slept++; continue; } }
    @autoreleasepool {
      siberia_frame fr; uint64_t x = siberia_now_ns(); if (siberia_publisher_begin(p, &fr)) { skipped++; continue; } bns += siberia_now_ns() - x;
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = tex[fr.slot]; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp]; [e setRenderPipelineState:ps]; t += 1 / 60.f; [e setFragmentBytes:&t length:4 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      rsum += (siberia_now_ns() - fr.time_ns) / 1e6;
      x = siberia_now_ns(); siberia_publisher_commit(p, &fr); cns += siberia_now_ns() - x; frames++;
      if (to) { mach_msg_header_t m = { MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), sizeof m, to, MACH_PORT_NULL, 0, 1 }; mach_msg(&m, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 10, MACH_PORT_NULL); }
    }
  }
  usage u1 = now_usage(); char ex[200]; snprintf(ex, sizeof ex, ",\"beginNs\":%.0f,\"commitNs\":%.0f,\"renderMs\":%.2f,\"skipped\":%ld,\"idleTicks\":%ld", frames ? (double)bns / frames : 0, frames ? (double)cns / frames : 0, frames ? rsum / frames : 0, skipped, slept);
  report("pub", mode, u0, u1, secs, frames, ex); fflush(stdout); usleep(300000); siberia_publisher_destroy(p); return 0;
}

static int subscriber(int c, char **a) {
  (void)c; const char *name = a[2]; double secs = atof(a[3]); const char *mode = a[4]; int msgs = !strcmp(mode, "messages");
  siberia_subscriber *s = NULL; for (int i = 0; i < 300 && siberia_subscriber_connect(name, &s); i++) usleep(10000); if (!s) { fprintf(stderr, "no connect\n"); return 2; }
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLLibrary> lib = [d newLibraryWithSource:SRC options:nil error:nil]; id<MTLRenderPipelineState> ps = pipeline(d, lib, @"show");
  NSMutableArray *tex = [NSMutableArray array]; for (int i = 0; i < siberia_subscriber_slot_count(s); i++) [tex addObject:wrap(d, siberia_subscriber_surface(s, i))];
  siberia_format f; siberia_subscriber_format(s, &f); MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:f.width height:f.height mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate; id<MTLTexture> screen = [d newTextureWithDescriptor:td];
  mach_port_t port = MACH_PORT_NULL; char svc[128]; snprintf(svc, sizeof svc, "com.cyberyak.siberia.bench.%s", name);
  if (msgs) { mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port); mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND); bootstrap_register(bootstrap_port, svc, port); }
  usleep(600000);
  uint64_t period = 16666667, t0 = siberia_now_ns(), next = t0 + (uint64_t)(rand() % 16) * 1000000, end = t0 + (uint64_t)(secs * 1e9), ans = 0, rns = 0; long frames = 0, none = 0; long cap = (long)(secs * 130); double *ages = calloc((size_t)cap, sizeof(double)); usage u0 = now_usage(); long n = 0;
  while (siberia_now_ns() < end) {
    if (msgs) { wake m; if (mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, port, 100, MACH_PORT_NULL)) continue; }
    else { sleep_until(next); next += period; siberia_subscriber_report_refresh(s, next); }
    @autoreleasepool {
      siberia_frame fr; uint64_t x = siberia_now_ns(); int r = siberia_subscriber_acquire(s, &fr); ans += siberia_now_ns() - x;
      if (r == SIBERIA_CLOSED) break; if (r) { none++; continue; }
      if (n < cap) ages[n++] = (siberia_now_ns() - fr.time_ns) / 1e6;
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = screen; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp]; [e setRenderPipelineState:ps]; [e setFragmentTexture:tex[fr.slot] atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      x = siberia_now_ns(); siberia_subscriber_release(s); rns += siberia_now_ns() - x; frames++;
    }
  }
  usage u1 = now_usage(); qsort(ages, (size_t)n, sizeof(double), cmp); char ex[200]; snprintf(ex, sizeof ex, ",\"acquireNs\":%.0f,\"releaseNs\":%.0f,\"ageP50ms\":%.2f,\"ageP99ms\":%.2f,\"noFrame\":%ld", (double)ans / (frames + none + 1), frames ? (double)rns / frames : 0, n ? ages[n / 2] : -1, n ? ages[n * 99 / 100] : -1, none);
  report("sub", mode, u0, u1, secs, frames, ex); siberia_subscriber_close(s); return 0;
}

int main(int c, char **a) { @autoreleasepool { srand((unsigned)siberia_now_ns()); if (c > 6 && !strcmp(a[1], "pub")) return publisher(c, a); if (c > 4 && !strcmp(a[1], "sub")) return subscriber(c, a); fprintf(stderr, "see the top of bench.m\n"); return 1; } }
