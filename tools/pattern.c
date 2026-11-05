// A plain C app on either end of a Siberia stream, for testing Electron from outside.
// Frames carry a frame number and a timestamp as bars at the top and bottom.
//   pattern pub <stream> <w> <h> <fps> <secs>
//   pattern sub <stream> <secs> <result.json>
#include "siberia.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define CELLS 40
#define BAR 32
static double wall_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }
static uint32_t stamp(void) { double m = wall_ms(); return (uint32_t)(m - 16777216.0 * (double)(uint64_t)(m / 16777216.0)); }
static void bars(uint8_t *base, size_t row, int w, int top, uint64_t v) {
  int cell = w / CELLS;
  for (int y = top; y < top + BAR; y++) { uint32_t *p = (uint32_t *)(base + y * row); for (int i = 0; i < CELLS; i++) { uint32_t c = (v >> i & 1) ? 0xffffffff : 0xff000000; for (int x = i * cell; x < (i + 1) * cell; x++) p[x] = c; } }
}
static uint64_t read_bars(const uint8_t *base, size_t row, int w, int y) {
  int cell = w / CELLS; uint64_t v = 0;
  for (int i = 0; i < CELLS; i++) { const uint8_t *p = base + y * row + (size_t)(i * cell + cell / 2) * 4; if (p[1] > 128) v |= 1ULL << i; }
  return v;
}
static void sleep_until(uint64_t ns) { uint64_t now = siberia_now_ns(); if (ns > now) { struct timespec t = { (time_t)((ns - now) / 1000000000ull), (long)((ns - now) % 1000000000ull) }; nanosleep(&t, NULL); } }
static int cmp(const void *a, const void *b) { return (int)(*(uint32_t *)a - *(uint32_t *)b); }

static int pub(char **a) {
  int w = atoi(a[3]), h = atoi(a[4]); double fps = atof(a[5]), secs = atof(a[6]);
  siberia_format f = { (uint32_t)w, (uint32_t)h, 'BGRA', 4, 0 }; siberia_publisher *p; int r = siberia_publisher_create(a[2], &f, 2, &p);
  if (r) { fprintf(stderr, "%s\n", siberia_status_text(r)); return 2; }
  for (int i = 0; i < siberia_publisher_slot_count(p); i++) {
    IOSurfaceRef s = siberia_publisher_surface(p, i); IOSurfaceLock(s, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++) { uint32_t *q = (uint32_t *)(b + y * row); for (int x = 0; x < w; x++) q[x] = 0xff000040u | (uint32_t)(x * 255 / w) << 16 | (uint32_t)(y * 255 / h) << 8; }
    IOSurfaceUnlock(s, 0, NULL);
  }
  siberia_demand d; for (int i = 0; i < 1000; i++) { siberia_publisher_demand(p, &d); if (d.subscribers) break; usleep(10000); }
  uint64_t period = (uint64_t)(1e9 / fps), next = siberia_now_ns(), end = next + (uint64_t)(secs * 1e9); long sent = 0, dropped = 0;
  while (siberia_now_ns() < end) {
    siberia_frame fr; if (siberia_publisher_begin(p, &fr)) { dropped++; } else {
      IOSurfaceLock(fr.surface, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(fr.surface); size_t row = IOSurfaceGetBytesPerRow(fr.surface); uint64_t v = (fr.sequence & 0xffff) | (uint64_t)stamp() << 16;
      bars(b, row, w, 0, v); bars(b, row, w, h - BAR, v); IOSurfaceUnlock(fr.surface, 0, NULL);
      siberia_publisher_commit(p, &fr); sent++;
    }
    next += period; sleep_until(next);
  }
  printf("{\"sent\":%ld,\"dropped\":%ld}\n", sent, dropped); fflush(stdout); usleep(200000); siberia_publisher_destroy(p); return 0;
}

static int sub(char **a) {
  double secs = atof(a[3]); siberia_subscriber *s = NULL;
  for (int i = 0; i < 2000 && siberia_subscriber_connect(a[2], &s); i++) usleep(10000);
  if (!s) { fprintf(stderr, "no stream\n"); return 2; }
  const uint8_t *base[16]; size_t row = 0; siberia_format f; siberia_subscriber_format(s, &f); int n = siberia_subscriber_slot_count(s);
  for (int i = 0; i < n; i++) { IOSurfaceRef q = siberia_subscriber_surface(s, i); IOSurfaceLock(q, kIOSurfaceLockReadOnly, NULL); base[i] = IOSurfaceGetBaseAddress(q); row = IOSurfaceGetBytesPerRow(q); }
  long frames = 0, torn = 0, skipped = 0, dup = 0; int last = -1; uint64_t lastSeq = 0, t0 = 0, end = 0; size_t cap = (size_t)(secs * 1000), lats = 0; uint32_t *lat = calloc(cap, 4);
  for (;;) {
    uint64_t now = siberia_now_ns(); if (t0 && now > end) break;
    siberia_frame fr; int r = siberia_subscriber_acquire(s, &fr);
    if (r == SIBERIA_CLOSED) break;
    if (r) { usleep(500); continue; }
    if (!t0) { t0 = now; end = t0 + (uint64_t)(secs * 1e9); }
    uint64_t top = read_bars(base[fr.slot], row, (int)f.width, BAR / 2), bot = read_bars(base[fr.slot], row, (int)f.width, (int)f.height - BAR / 2);
    uint32_t back = (stamp() - (uint32_t)(top >> 16)) & 0xffffff; siberia_subscriber_release(s);
    int fn = (int)(top & 0xffff); frames++; if (top != bot) torn++;
    if (last >= 0) { int d = (fn - last) & 0xffff; if (!d) dup++; else skipped += d - 1; } last = fn;
    if (lastSeq && fr.sequence > lastSeq + 1) { /* counted through the bars */ } lastSeq = fr.sequence;
    if (back < 2000 && lats < cap) lat[lats++] = back;
  }
  qsort(lat, lats, 4, cmp);
  FILE *o = fopen(a[4], "w"); if (!o) return 3;
  fprintf(o, "{\"frames\":%ld,\"fps\":%.1f,\"torn\":%ld,\"skipped\":%ld,\"duplicates\":%ld,\"latencyP50ms\":%u,\"latencyP99ms\":%u}\n", frames, frames / secs, torn, skipped, dup, lats ? lat[lats / 2] : 0, lats ? lat[lats * 99 / 100] : 0);
  fclose(o); siberia_subscriber_close(s); return 0;
}

int main(int c, char **a) { if (c > 6 && !strcmp(a[1], "pub")) return pub(a); if (c > 4 && !strcmp(a[1], "sub")) return sub(a); fprintf(stderr, "see the top of pattern.c\n"); return 1; }
