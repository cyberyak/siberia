// Runs as several real processes, so crashes and races are the real thing.
//   test_siberia                everything
//   test_siberia stress         only the stress test; any torn frame fails
//   test_siberia expect-tears   stress test on the broken build; it must find torn frames
#include "siberia.h"
#include <fcntl.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static const char *self;
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { long long x = (a), y = (b); checks++; if (x != y) { failures++; printf("  FAILED %s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__, #a, x, y); } } while (0)

static const siberia_format BGRA = { 256, 256, 'BGRA', 4, 0 };
static char *unique(void) { static char n[64]; static int c; snprintf(n, sizeof n, "test %d.%d", getpid(), c++); return n; }
static void wait_ms(int ms) { usleep((useconds_t)ms * 1000); }

static pid_t spawn_child(const char *role, const char *name, const char *arg, int *out) {
  int fds[2]; pipe(fds);
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_adddup2(&fa, fds[1], 1); posix_spawn_file_actions_addclose(&fa, fds[0]);
  char *argv[] = { (char *)self, "child", (char *)role, (char *)name, (char *)arg, NULL };
  pid_t pid; posix_spawn(&pid, self, &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa); close(fds[1]); *out = fds[0];
  return pid;
}
static int read_line(int fd, char *buf, int size) { int n = 0; char c; while (n < size - 1 && read(fd, &c, 1) == 1 && c != '\n') buf[n++] = c; buf[n] = 0; return n; }

static void publish(siberia_publisher *p) { siberia_frame f; if (siberia_publisher_begin(p, &f) == SIBERIA_OK) siberia_publisher_commit(p, &f); }

static uint32_t *pixels(IOSurfaceRef s) { IOSurfaceLock(s, 0, NULL); return IOSurfaceGetBaseAddress(s); }

static int child(const char *role, const char *name, const char *arg) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (!strcmp(role, "publish")) {
    siberia_publisher *p; if (siberia_publisher_create(name, &BGRA, 1, &p)) return 2;
    publish(p); puts("ready"); for (;;) pause();
  }
  siberia_subscriber *s = NULL;
  for (int i = 0; i < 100 && siberia_subscriber_connect(name, &s); i++) wait_ms(20);
  if (!s) { puts("no-connect"); return 2; }
  if (!strcmp(role, "hold")) {
    siberia_frame f; while (siberia_subscriber_acquire(s, &f) != SIBERIA_OK) wait_ms(1);
    printf("held %u\n", f.slot); for (;;) pause();
  }
  if (!strcmp(role, "watch")) { // waits for the publisher to go away
    siberia_frame f; puts("watching");
    for (int i = 0; i < 500; i++) { int r = siberia_subscriber_acquire(s, &f); if (r == SIBERIA_OK) siberia_subscriber_release(s); else if (r == SIBERIA_CLOSED) { puts("closed"); return 0; } wait_ms(10); }
    puts("timeout"); return 1;
  }
  // reader: checks every pixel of every frame
  uint32_t *px[10]; int n = siberia_subscriber_slot_count(s); siberia_format fmt; siberia_subscriber_format(s, &fmt);
  for (int i = 0; i < n; i++) px[i] = pixels(siberia_subscriber_surface(s, i));
  size_t count = IOSurfaceGetBytesPerRow(siberia_subscriber_surface(s, 0)) * fmt.height / 4;
  uint64_t end = siberia_now_ns() + (uint64_t)atoi(arg) * 1000000000ull; long frames = 0, torn = 0; siberia_frame f; srand((unsigned)getpid());
  while (siberia_now_ns() < end) {
    int r = siberia_subscriber_acquire(s, &f);
    if (r == SIBERIA_NO_NEW_FRAME) continue;
    if (r) break;
    const uint32_t *p = px[f.slot]; uint32_t want = (uint32_t)f.sequence; int bad = 0;
    for (size_t i = 0; i < count; i++) if (p[i] != want) { bad = 1; break; }
    if (rand() % 8 == 0) usleep(50); // hold a frame now and then, as a slow reader would
    if (!bad) for (size_t i = 0; i < count; i++) if (p[i] != want) { bad = 1; break; } // it must not change while held
    torn += bad; frames++;
    siberia_subscriber_release(s);
  }
  printf("%ld %ld\n", frames, torn);
  siberia_subscriber_close(s);
  return 0;
}

static void test_arguments(void) {
  siberia_publisher *p; siberia_subscriber *s; char longname[100]; memset(longname, 'a', 99); longname[99] = 0;
  EQ(siberia_publisher_create("", &BGRA, 1, &p), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_publisher_create(longname, &BGRA, 1, &p), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_publisher_create(unique(), &BGRA, 0, &p), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_publisher_create(unique(), &BGRA, 9, &p), SIBERIA_ERR_ARGUMENT);
  siberia_format empty = BGRA; empty.width = 0;
  EQ(siberia_publisher_create(unique(), &empty, 1, &p), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_subscriber_connect(unique(), &s), SIBERIA_ERR_NOT_FOUND);
  CHECK(strcmp(siberia_status_text(SIBERIA_CLOSED), "ok"));
}

static void test_frames(void) {
  char *name = unique(); siberia_publisher *p; siberia_subscriber *s; siberia_frame w, r, r2, w2;
  EQ(siberia_publisher_create(name, &BGRA, 2, &p), SIBERIA_OK);
  EQ(siberia_publisher_slot_count(p), 4);
  EQ(siberia_subscriber_connect(name, &s), SIBERIA_OK);
  siberia_format f; siberia_subscriber_format(s, &f); EQ(f.width, 256); EQ(f.pixel_format, 'BGRA'); EQ(siberia_subscriber_slot_count(s), 4);
  EQ(siberia_subscriber_acquire(s, &r), SIBERIA_NO_NEW_FRAME);
  EQ(siberia_publisher_begin(p, &w), SIBERIA_OK);
  EQ(siberia_publisher_begin(p, &w2), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_subscriber_acquire(s, &r), SIBERIA_NO_NEW_FRAME); // begin alone publishes nothing
  w.time_ns = 1234; EQ(siberia_publisher_commit(p, &w), SIBERIA_OK);
  EQ(siberia_publisher_commit(p, &w), SIBERIA_ERR_ARGUMENT);
  EQ(siberia_subscriber_acquire(s, &r), SIBERIA_OK);
  EQ(r.sequence, 1); EQ(r.time_ns, 1234); EQ(r.slot, w.slot);
  CHECK(IOSurfaceGetID(r.surface) == IOSurfaceGetID(w.surface));
  EQ(siberia_subscriber_acquire(s, &r2), SIBERIA_BUSY);
  siberia_subscriber_release(s);
  EQ(siberia_subscriber_acquire(s, &r2), SIBERIA_NO_NEW_FRAME); // the same frame isn't new
  for (int i = 0; i < 5; i++) publish(p);
  EQ(siberia_subscriber_acquire(s, &r2), SIBERIA_OK); EQ(r2.sequence, 6); // skips to the newest
  // A held frame must never be handed back to the publisher.
  for (int i = 0; i < 50; i++) { EQ(siberia_publisher_begin(p, &w), SIBERIA_OK); CHECK(w.slot != r2.slot); siberia_publisher_commit(p, &w); }
  siberia_subscriber_release(s);
  EQ(siberia_publisher_begin(p, &w), SIBERIA_OK); siberia_publisher_cancel(p, &w);
  EQ(siberia_publisher_begin(p, &w), SIBERIA_OK); siberia_publisher_cancel(p, &w); // cancel frees it
  siberia_subscriber_close(s); siberia_publisher_destroy(p);
}

static void test_demand(void) {
  char *name = unique(); siberia_publisher *p; siberia_subscriber *a, *b; siberia_demand d;
  siberia_publisher_create(name, &BGRA, 2, &p);
  siberia_publisher_demand(p, &d); EQ(d.subscribers, 0);
  siberia_subscriber_connect(name, &a); siberia_subscriber_connect(name, &b);
  siberia_publisher_demand(p, &d); EQ(d.subscribers, 2); EQ(d.width, 0); EQ(d.fps_milli, 0);
  siberia_subscriber_request(a, 640, 360, 30000); siberia_subscriber_request(b, 1280, 720, 24000);
  siberia_publisher_demand(p, &d); EQ(d.width, 1280); EQ(d.height, 720); EQ(d.fps_milli, 30000);
  uint64_t now = siberia_now_ns();
  siberia_subscriber_report_refresh(a, now + 8000000); siberia_subscriber_report_refresh(b, now + 4000000);
  siberia_publisher_demand(p, &d); EQ(d.next_refresh_ns, now + 4000000);
  siberia_subscriber_request(b, 0, 0, 0);
  siberia_publisher_demand(p, &d); EQ(d.width, 0); EQ(d.fps_milli, 0); // one full-size reader wins
  siberia_subscriber_close(b);
  for (int i = 0; i < 100; i++) { siberia_publisher_demand(p, &d); if (d.subscribers == 1) break; wait_ms(5); }
  EQ(d.subscribers, 1);
  EQ(d.width, 640);
  siberia_subscriber_close(a); siberia_publisher_destroy(p);
}

static void test_registry(void) {
  char name[64]; snprintf(name, sizeof name, "Rack/1 é %d", getpid()); siberia_publisher *p, *q; siberia_subscriber *s; siberia_stream_info list[64]; int found = 0;
  EQ(siberia_publisher_create(name, &BGRA, 1, &p), SIBERIA_OK);
  EQ(siberia_publisher_create(name, &BGRA, 1, &q), SIBERIA_ERR_NAME_TAKEN);
  int n = siberia_list(list, 64);
  for (int i = 0; i < n; i++) if (!strcmp(list[i].name, name)) { found++; EQ(list[i].pid, getpid()); }
  EQ(found, 1);
  EQ(siberia_subscriber_connect(name, &s), SIBERIA_OK); siberia_subscriber_close(s);
  siberia_publisher_destroy(p);
  EQ(siberia_subscriber_connect(name, &s), SIBERIA_ERR_NOT_FOUND);
  n = siberia_list(list, 64); found = 0; for (int i = 0; i < n; i++) found += !strcmp(list[i].name, name);
  EQ(found, 0);
  EQ(siberia_publisher_create(name, &BGRA, 1, &q), SIBERIA_OK); siberia_publisher_destroy(q); // the name is reusable
}

static void test_full(void) {
  char *name = unique(); siberia_publisher *p; siberia_subscriber *a, *b;
  siberia_publisher_create(name, &BGRA, 1, &p);
  EQ(siberia_subscriber_connect(name, &a), SIBERIA_OK);
  EQ(siberia_subscriber_connect(name, &b), SIBERIA_ERR_FULL);
  siberia_subscriber_close(a);
  int r = SIBERIA_ERR_FULL; for (int i = 0; i < 100 && r == SIBERIA_ERR_FULL; i++) { wait_ms(5); r = siberia_subscriber_connect(name, &b); }
  EQ(r, SIBERIA_OK); // a closed subscriber frees its place
  siberia_subscriber_close(b); siberia_publisher_destroy(p);
}

static void test_subscriber_crash(void) {
  char *name = unique(); siberia_publisher *p; siberia_demand d; char line[64]; int fd, slot; siberia_frame f;
  siberia_publisher_create(name, &BGRA, 1, &p); publish(p);
  pid_t c = spawn_child("hold", name, "0", &fd);
  read_line(fd, line, sizeof line); CHECK(sscanf(line, "held %d", &slot) == 1);
  for (int i = 0; i < 20; i++) { EQ(siberia_publisher_begin(p, &f), SIBERIA_OK); CHECK((int)f.slot != slot); siberia_publisher_commit(p, &f); } // held: never reused
  kill(c, SIGKILL); waitpid(c, NULL, 0); close(fd);
  for (int i = 0; i < 200; i++) { siberia_publisher_demand(p, &d); if (!d.subscribers) break; wait_ms(5); }
  EQ(d.subscribers, 0);
  int reused = 0; for (int i = 0; i < 20; i++) { siberia_publisher_begin(p, &f); reused |= (int)f.slot == slot; siberia_publisher_commit(p, &f); }
  CHECK(reused); // its buffer came back
  siberia_subscriber *s; EQ(siberia_subscriber_connect(name, &s), SIBERIA_OK); // and so did its place
  siberia_subscriber_close(s); siberia_publisher_destroy(p);
}

static void test_publisher_crash(void) {
  char *name = unique(); char line[64]; int pubfd, subfd; siberia_publisher *p; siberia_stream_info list[64];
  pid_t pub = spawn_child("publish", name, "0", &pubfd);
  read_line(pubfd, line, sizeof line); EQ(strcmp(line, "ready"), 0);
  pid_t sub = spawn_child("watch", name, "0", &subfd);
  read_line(subfd, line, sizeof line); EQ(strcmp(line, "watching"), 0);
  kill(pub, SIGKILL); waitpid(pub, NULL, 0); close(pubfd);
  read_line(subfd, line, sizeof line); EQ(strcmp(line, "closed"), 0); // a subscriber finds out
  waitpid(sub, NULL, 0); close(subfd);
  int n = siberia_list(list, 64), found = 0; for (int i = 0; i < n; i++) found += !strcmp(list[i].name, name);
  EQ(found, 0); // a dead publisher isn't listed
  EQ(siberia_publisher_create(name, &BGRA, 1, &p), SIBERIA_OK); // and its name can be taken over
  siberia_publisher_destroy(p);
}

static void test_graceful_close(void) {
  char *name = unique(); siberia_publisher *p; siberia_subscriber *s; siberia_frame f;
  siberia_publisher_create(name, &BGRA, 1, &p); siberia_subscriber_connect(name, &s);
  siberia_publisher_destroy(p);
  EQ(siberia_subscriber_acquire(s, &f), SIBERIA_CLOSED);
  siberia_subscriber_close(s);
}

static void test_leaks(void) { // connecting repeatedly must not eat Mach ports
  char *name = unique(); siberia_publisher *p; siberia_subscriber *s; mach_port_name_array_t names; mach_port_type_array_t types; mach_msg_type_number_t a, b, c, d;
  siberia_publisher_create(name, &BGRA, 1, &p);
  siberia_subscriber_connect(name, &s); siberia_subscriber_close(s); wait_ms(30);
  mach_port_names(mach_task_self(), &names, &a, &types, &c);
  vm_deallocate(mach_task_self(), (vm_address_t)names, a * sizeof *names); vm_deallocate(mach_task_self(), (vm_address_t)types, c * sizeof *types);
  for (int i = 0; i < 50; i++) { int r = SIBERIA_ERR_FULL; while (r == SIBERIA_ERR_FULL) r = siberia_subscriber_connect(name, &s); EQ(r, SIBERIA_OK); siberia_subscriber_close(s); }
  wait_ms(50);
  mach_port_names(mach_task_self(), &names, &b, &types, &d);
  vm_deallocate(mach_task_self(), (vm_address_t)names, b * sizeof *names); vm_deallocate(mach_task_self(), (vm_address_t)types, d * sizeof *types);
  CHECK(b <= a + 5);
  if (b > a + 5) printf("  ports grew from %u to %u\n", a, b);
  siberia_publisher_destroy(p);
}

static void test_no_system_calls(void) { // the claim that a frame costs no kernel work
  char *name = unique(); siberia_publisher *p; siberia_subscriber *s; siberia_frame w, r; siberia_demand d;
  siberia_publisher_create(name, &BGRA, 1, &p); siberia_subscriber_connect(name, &s);
  task_events_info_data_t a, b; mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT; enum { FRAMES = 100000 };
  task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&a, &n);
  for (int i = 0; i < FRAMES; i++) {
    siberia_publisher_demand(p, &d); siberia_publisher_begin(p, &w); siberia_publisher_commit(p, &w);
    siberia_subscriber_report_refresh(s, 1); siberia_subscriber_acquire(s, &r); siberia_subscriber_release(s);
  }
  n = TASK_EVENTS_INFO_COUNT; task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&b, &n);
  int calls = (b.syscalls_mach - a.syscalls_mach) + (b.syscalls_unix - a.syscalls_unix);
  printf("  %d frames made %d system calls\n", FRAMES, calls);
  CHECK(calls < 50);
  siberia_subscriber_close(s); siberia_publisher_destroy(p);
}

static void stress(int seconds, long *frames, long *torn, long *published, long *noslot) {
  char *name = unique(); siberia_publisher *p; enum { READERS = 3 }; int fds[READERS]; pid_t pids[READERS]; char secs[8]; snprintf(secs, sizeof secs, "%d", seconds);
  siberia_publisher_create(name, &BGRA, READERS, &p);
  int n = siberia_publisher_slot_count(p); uint32_t *px[10]; for (int i = 0; i < n; i++) px[i] = pixels(siberia_publisher_surface(p, i));
  size_t count = IOSurfaceGetBytesPerRow(siberia_publisher_surface(p, 0)) * BGRA.height / 4;
  publish(p);
  for (int i = 0; i < READERS; i++) pids[i] = spawn_child("reader", name, secs, &fds[i]);
  uint64_t end = siberia_now_ns() + (uint64_t)(seconds + 1) * 1000000000ull; siberia_frame f; *published = *noslot = 0;
  while (siberia_now_ns() < end) {
    int r = siberia_publisher_begin(p, &f);
    if (r) { ++*noslot; continue; }
    uint32_t v = (uint32_t)f.sequence, *d = px[f.slot];
    for (size_t i = 0; i < count; i++) d[i] = v;
    siberia_publisher_commit(p, &f); ++*published;
  }
  *frames = *torn = 0; char line[64];
  for (int i = 0; i < READERS; i++) { long a = 0, b = 0; read_line(fds[i], line, sizeof line); sscanf(line, "%ld %ld", &a, &b); *frames += a; *torn += b; waitpid(pids[i], NULL, 0); close(fds[i]); }
  siberia_publisher_destroy(p);
}

int main(int argc, char **argv) {
  self = argv[0];
  if (argc >= 5 && !strcmp(argv[1], "child")) return child(argv[2], argv[3], argv[4]);
  long frames, torn, published, noslot;
  const char *mode = argc > 1 ? argv[1] : "all";
  if (!strcmp(mode, "all")) {
#define RUN(t) do { printf("%s\n", #t); t(); } while (0)
    RUN(test_arguments); RUN(test_frames); RUN(test_demand); RUN(test_registry); RUN(test_full); RUN(test_graceful_close); RUN(test_subscriber_crash); RUN(test_publisher_crash); RUN(test_leaks); RUN(test_no_system_calls);
  }
  stress(4, &frames, &torn, &published, &noslot);
  printf("stress: published %ld, read %ld frames across 3 processes, %ld torn, %ld blocked\n", published, frames, torn, noslot);
  if (!strcmp(mode, "expect-tears")) { if (torn > 0 && frames > 0) { puts("OK: the stress test catches the broken build"); return 0; } puts("FAILED: the broken build produced no torn frames, so this test proves nothing"); return 1; }
  CHECK(frames > 1000); EQ(torn, 0); EQ(noslot, 0);
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
