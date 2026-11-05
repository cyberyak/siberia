// External process: a fake 4K video app that publishes a ring of IOSurfaces to Electron.
// usage: tx <w> <h> <fps> <seconds> [ringSize]
#import <Foundation/Foundation.h>
#import <mach/mach_time.h>
#import "proto.h"
static void bars(uint8_t *base, size_t bpr, int w, int y0, uint64_t v) {
  int cell = w / CELLS;
  for (int y = y0; y < y0 + BAR; y++) { uint32_t *row = (uint32_t *)(base + y * bpr);
    for (int i = 0; i < CELLS; i++) { uint32_t col = (v >> i & 1) ? 0xffffffff : 0xff000000; for (int x = i * cell; x < (i + 1) * cell; x++) row[x] = col; } }
}
int main(int c, char **a) { @autoreleasepool {
  int w = atoi(a[1]), h = atoi(a[2]); double fps = atof(a[3]), secs = atof(a[4]); int n = c > 5 ? atoi(a[5]) : 4;
  mach_port_t to = MACH_PORT_NULL;
  for (int i = 0; i < 300 && !to; i++) { to = lookup_service(SVC_IN); if (!to) usleep(100000); }
  if (!to) { fprintf(stderr, "no receiver\n"); return 2; }
  IOSurfaceRef ring[16];
  for (int i = 0; i < n; i++) {
    ring[i] = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w),(id)kIOSurfaceHeight:@(h),(id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfacePixelFormat:@(0x42475241)});
    IOSurfaceLock(ring[i], 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(ring[i]); size_t bpr = IOSurfaceGetBytesPerRow(ring[i]);
    for (int y = 0; y < h; y++) { uint32_t *r = (uint32_t *)(b + y * bpr); for (int x = 0; x < w; x++) r[x] = 0xff000000 | (x * 255 / w) << 16 | (y * 255 / h) << 8 | 0x40; }
    IOSurfaceUnlock(ring[i], 0, NULL);
    if (send_surface(to, ring[i], i + 1)) { fprintf(stderr, "send surface failed\n"); return 3; }
  }
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  uint64_t step = (uint64_t)(1e9 / fps * tb.denom / tb.numer), next = mach_absolute_time();
  long total = (long)(fps * secs), sent = 0, failed = 0;
  for (long f = 0; f < total; f++) {
    IOSurfaceRef s = ring[f % n]; IOSurfaceLock(s, 0, NULL);
    uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    uint64_t ms = (uint64_t)fmod([NSDate date].timeIntervalSince1970 * 1000, 16777216.0);
    uint64_t v = (uint64_t)(f & 0xffff) | ms << 16;
    bars(b, bpr, w, 0, v); bars(b, bpr, w, h - BAR, v);
    IOSurfaceUnlock(s, 0, NULL);
    if (send_frame(to, (uint32_t)(f % n) + 1, (uint32_t)f)) failed++; else sent++;
    next += step; mach_wait_until(next);
  }
  printf("{\"sent\":%ld,\"failed\":%ld}\n", sent, failed);
  return 0; } }
