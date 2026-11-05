// External process: reads frames Electron publishes and checks every one.
// usage: rx <seconds> <result.json>
#import <Foundation/Foundation.h>
#import "proto.h"
static uint32_t bits(const uint8_t *base, size_t bpr, int w, int y) {
  int cell = w / CELLS; uint64_t v = 0;
  for (int i = 0; i < CELLS; i++) { const uint8_t *p = base + y * bpr + (size_t)(i * cell + cell / 2) * 4; if (p[1] > 128) v |= 1ULL << i; }
  return 0; }
static uint64_t bits64(const uint8_t *base, size_t bpr, int w, int y) {
  int cell = w / CELLS; uint64_t v = 0;
  for (int i = 0; i < CELLS; i++) { const uint8_t *p = base + y * bpr + (size_t)(i * cell + cell / 2) * 4; if (p[1] > 128) v |= 1ULL << i; }
  return v; }
int main(int c, char **a) { @autoreleasepool {
  double secs = c > 1 ? atof(a[1]) : 10; const char *out = c > 2 ? a[2] : "/dev/stdout";
  mach_port_t port = register_service(SVC_OUT);
  if (!port) { fprintf(stderr, "register failed\n"); return 2; }
  fprintf(stderr, "rx ready\n");
  IOSurfaceRef surfaces[4096] = {0}; rx_buf m;
  long frames = 0, torn = 0, gaps = 0, dup = 0, surfaceMsgs = 0; int lasts[64]; memset(lasts,-1,sizeof lasts); NSMutableArray *lat = [NSMutableArray array];
  double t0 = 0, tEnd = 0;
  for (;;) {
    NSDate *now = [NSDate date];
    if (t0 && now.timeIntervalSince1970 > tEnd) break;
    kern_return_t k = mach_msg(&m.s.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, port, t0 ? 200 : 30000, 0);
    if (k == MACH_RCV_TIMED_OUT) { if (!t0) break; continue; }
    if (k) { fprintf(stderr, "rcv %x\n", k); break; }
    if (m.s.h.msgh_id == MSG_SURFACE) {
      surfaces[m.s.sid & 4095] = IOSurfaceLookupFromMachPort(m.s.port.name);
      mach_port_deallocate(mach_task_self(), m.s.port.name); surfaceMsgs++; continue; }
    IOSurfaceRef s = surfaces[m.f.sid & 4095]; if (!s) continue;
    if (!t0) { t0 = now.timeIntervalSince1970; tEnd = t0 + secs; }
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s); int w = (int)IOSurfaceGetWidth(s), h = (int)IOSurfaceGetHeight(s);
    uint64_t top = bits64(base, bpr, w, BAR / 2), bot = bits64(base, bpr, w, h - BAR / 2);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    double nowMs = [NSDate date].timeIntervalSince1970 * 1000;
    int fr = (int)(top & 0xffff); uint32_t ms = (uint32_t)(top >> 16) & 0xffffff;
    frames++;
    if (top != bot) torn++;
    int st = ((m.f.sid - 1) / 10) & 63; if (lasts[st] >= 0) { int d = (fr - lasts[st]) & 0xffff; if (d == 0) dup++; else if (d > 1) gaps += d - 1; }
    lasts[st] = fr;
    uint32_t l = ((uint32_t)fmod(nowMs, 16777216.0) - ms) & 0xffffff;
    if (l < 2000) [lat addObject:@(l)];
  }
  NSArray *s = [lat sortedArrayUsingSelector:@selector(compare:)]; double el = secs;
  id p50 = s.count ? s[s.count / 2] : @(-1), p99 = s.count ? s[s.count * 99 / 100] : @(-1);
  NSDictionary *r = @{@"frames":@(frames),@"fps":@(frames/el),@"torn":@(torn),@"skipped":@(gaps),@"duplicates":@(dup),@"surfaces":@(surfaceMsgs),@"latencyP50ms":p50,@"latencyP99ms":p99};
  NSData *j = [NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil];
  [j writeToFile:@(out) atomically:YES]; fwrite(j.bytes, 1, j.length, stdout); puts("");
  return 0; } }
