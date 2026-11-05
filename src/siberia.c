#include "siberia.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/notify.h>
#include <sched.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x52424953u /* "SIBR" */
#define MAX_READERS SIBERIA_MAX_SUBSCRIBERS
#define MAX_SLOTS (MAX_READERS + 2)
#define NO_SLOT (-1)
#define SLOT_BITS 8
#define MSG_CONNECT 0x53420001
#define MSG_CONNECTED 0x53420002
#define CACHE_LINE 128 /* Apple silicon */

typedef struct {
  uint64_t sequence;
  uint64_t time_ns;
} slot_info;

/* Each reader writes only its own entry, on its own cache line. */
typedef struct {
  _Alignas(CACHE_LINE) _Atomic uint32_t pid; /* 0: free */
  _Atomic int32_t claimed;
  _Atomic uint32_t wanted_width, wanted_height, wanted_fps_milli;
  _Atomic uint64_t next_refresh_ns;
} reader_entry;

typedef struct {
  uint32_t magic;
  uint32_t version;
  siberia_format format;
  uint32_t slot_count;
  _Atomic uint32_t closed;
  _Alignas(CACHE_LINE) _Atomic uint64_t latest; /* sequence << 8 | slot; 0 before the first frame */
  _Alignas(CACHE_LINE) slot_info slots[MAX_SLOTS];
  reader_entry readers[MAX_READERS];
} control_block;

_Static_assert(sizeof(control_block) <= 4096, "the control block must fit one page");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "shared memory needs lock-free atomics");

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t token;
  uint32_t version;
  int32_t pid;
} connect_msg;

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t control;
  mach_msg_port_descriptor_t surfaces[MAX_SLOTS];
  int32_t status;
  int32_t reader;
} connected_msg;

typedef union {
  mach_msg_header_t header;
  connect_msg connect;
  connected_msg connected;
  mach_dead_name_notification_t dead;
  char space[sizeof(connected_msg) + 256];
} any_msg;

struct siberia_publisher {
  control_block *ctl;
  mach_port_t memory, service;
  dispatch_queue_t queue;
  dispatch_source_t source;
  dispatch_semaphore_t cancelled;
  IOSurfaceRef surfaces[MAX_SLOTS];
  mach_port_t tokens[MAX_READERS]; /* touched only on the queue */
  int max_readers;
  int writing;
  uint32_t next;
  uint64_t sequence;
  char path[1024];
};

struct siberia_subscriber {
  control_block *ctl;
  mach_port_t service, token;
  dispatch_source_t source;
  dispatch_semaphore_t cancelled;
  IOSurfaceRef surfaces[MAX_SLOTS];
  reader_entry *me;
  _Atomic uint32_t dead;
  _Atomic uint32_t held;
  uint64_t last;
};

uint64_t siberia_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

const char *siberia_status_text(int s) {
  switch (s) {
    case SIBERIA_OK: return "ok";
    case SIBERIA_NO_NEW_FRAME: return "no new frame";
    case SIBERIA_NO_FREE_SLOT: return "no free buffer";
    case SIBERIA_BUSY: return "a frame is already held";
    case SIBERIA_CLOSED: return "the publisher has gone";
    case SIBERIA_ERR_ARGUMENT: return "invalid argument";
    case SIBERIA_ERR_NAME_TAKEN: return "that stream name is in use";
    case SIBERIA_ERR_NOT_FOUND: return "no such stream";
    case SIBERIA_ERR_FULL: return "the stream has no room for another subscriber";
    case SIBERIA_ERR_VERSION: return "incompatible protocol version";
    default: return "system error";
  }
}

/* ---- The registry: one small file per stream, naming its Mach service. ---- */

static int registry_dir(char *out, size_t size) {
  char tmp[900];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) return -1;
  snprintf(out, size, "%ssiberia", tmp);
  if (mkdir(out, 0700) && errno != EEXIST) return -1;
  return 0;
}

static int valid_name(const char *name) {
  if (!name) return 0;
  size_t n = strlen(name);
  return n > 0 && n <= SIBERIA_NAME_MAX;
}

static int stream_path(const char *name, char *out, size_t size) {
  char dir[960], enc[SIBERIA_NAME_MAX * 3 + 1], *e = enc;
  if (registry_dir(dir, sizeof dir)) return -1;
  for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
    if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '-' || *c == '_') *e++ = (char)*c;
    else e += sprintf(e, "%%%02X", *c);
  }
  *e = 0;
  snprintf(out, size, "%s/%s", dir, enc);
  return 0;
}

static void decode_name(const char *enc, char *out) {
  int n = 0;
  for (; *enc && n < SIBERIA_NAME_MAX; n++) {
    unsigned v;
    if (*enc == '%' && sscanf(enc + 1, "%2X", &v) == 1) { out[n] = (char)v; enc += 3; }
    else out[n] = *enc++;
  }
  out[n] = 0;
}

static int read_entry(const char *path, int *pid, char *service, size_t size) {
  char fmt[32];
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  snprintf(fmt, sizeof fmt, "%%d %%%zus", size - 1);
  int got = fscanf(f, fmt, pid, service);
  fclose(f);
  return got == 2 ? 0 : -1;
}

static int pid_alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

int siberia_list(siberia_stream_info *out, int max) {
  char dir[960], path[1280], service[128];
  int n = 0, pid;
  if (!out || max <= 0 || registry_dir(dir, sizeof dir)) return 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  for (struct dirent *e; n < max && (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (read_entry(path, &pid, service, sizeof service) || !pid_alive(pid)) continue;
    decode_name(e->d_name, out[n].name);
    out[n++].pid = pid;
  }
  closedir(d);
  return n;
}

/* link() fails if the name exists, so two publishers can never both claim it. */
static int claim_name(const char *path, const char *service) {
  char tmp[1100], line[160], other[128];
  int pid;
  snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return SIBERIA_ERR_SYSTEM;
  int len = snprintf(line, sizeof line, "%d %s\n", getpid(), service);
  ssize_t wrote = write(fd, line, (size_t)len);
  close(fd);
  int status = wrote == len ? SIBERIA_ERR_NAME_TAKEN : SIBERIA_ERR_SYSTEM;
  for (int i = 0; i < 2 && wrote == len; i++) {
    if (!link(tmp, path)) { status = SIBERIA_OK; break; }
    if (errno != EEXIST) { status = SIBERIA_ERR_SYSTEM; break; }
    if (!read_entry(path, &pid, other, sizeof other) && pid_alive(pid)) break;
    unlink(path); /* left behind by a crash */
  }
  unlink(tmp);
  return status;
}

/* ---- Shared memory ---- */

static control_block *map_control(mach_port_t entry) {
  mach_vm_address_t addr = 0;
  kern_return_t k = mach_vm_map(mach_task_self(), &addr, sizeof(control_block), 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_NONE);
  return k == KERN_SUCCESS ? (control_block *)addr : NULL;
}

static void unmap_control(control_block *ctl) {
  if (ctl) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)ctl, sizeof(control_block));
}

static IOSurfaceRef make_surface(const siberia_format *f) {
  const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
  int32_t values[] = { (int32_t)f->width, (int32_t)f->height, (int32_t)f->bytes_per_pixel, (int32_t)f->pixel_format };
  const void *nums[4];
  for (int i = 0; i < 4; i++) nums[i] = CFNumberCreate(NULL, kCFNumberSInt32Type, &values[i]);
  CFDictionaryRef d = CFDictionaryCreate(NULL, keys, nums, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  IOSurfaceRef s = IOSurfaceCreate(d);
  CFRelease(d);
  for (int i = 0; i < 4; i++) CFRelease(nums[i]);
  return s;
}

/* ---- Publisher ---- */

static void drop_reader(siberia_publisher *p, mach_port_t token) {
  for (int i = 0; i < p->max_readers; i++) {
    if (p->tokens[i] != token) continue;
    atomic_store(&p->ctl->readers[i].claimed, NO_SLOT);
    atomic_store(&p->ctl->readers[i].pid, 0);
    p->tokens[i] = MACH_PORT_NULL;
    mach_port_deallocate(mach_task_self(), token);
  }
  mach_port_deallocate(mach_task_self(), token); /* the notification's own reference */
}

static void answer_connect(siberia_publisher *p, connect_msg *in) {
  int reader = -1, status = SIBERIA_OK;
  mach_port_t token = in->token.name, previous = MACH_PORT_NULL;
  if (in->version != SIBERIA_VERSION) status = SIBERIA_ERR_VERSION;
  else {
    for (int i = 0; i < p->max_readers; i++) if (p->tokens[i] == MACH_PORT_NULL) { reader = i; break; }
    if (reader < 0) status = SIBERIA_ERR_FULL;
  }
  /* Tells us when this subscriber's token dies, by closing or by crashing. It's the only cleanup path. */
  if (status == SIBERIA_OK && mach_port_request_notification(mach_task_self(), token, MACH_NOTIFY_DEAD_NAME, 1, p->service, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) != KERN_SUCCESS) status = SIBERIA_ERR_SYSTEM;
  if (status == SIBERIA_OK) {
    reader_entry *r = &p->ctl->readers[reader];
    atomic_store(&r->claimed, NO_SLOT);
    atomic_store(&r->wanted_width, 0);
    atomic_store(&r->wanted_height, 0);
    atomic_store(&r->wanted_fps_milli, 0);
    atomic_store(&r->next_refresh_ns, 0);
    atomic_store(&r->pid, (uint32_t)in->pid);
    p->tokens[reader] = token;
  } else mach_port_deallocate(mach_task_self(), token);

  connected_msg out;
  memset(&out, 0, sizeof out);
  out.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
  out.header.msgh_size = sizeof out;
  out.header.msgh_remote_port = in->header.msgh_remote_port;
  out.header.msgh_id = MSG_CONNECTED;
  out.body.msgh_descriptor_count = 1 + MAX_SLOTS;
  out.control.type = MACH_MSG_PORT_DESCRIPTOR;
  out.control.disposition = MACH_MSG_TYPE_COPY_SEND;
  out.control.name = status == SIBERIA_OK ? p->memory : MACH_PORT_NULL;
  for (int i = 0; i < MAX_SLOTS; i++) {
    out.surfaces[i].type = MACH_MSG_PORT_DESCRIPTOR;
    out.surfaces[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
    out.surfaces[i].name = status == SIBERIA_OK && p->surfaces[i] ? IOSurfaceCreateMachPort(p->surfaces[i]) : MACH_PORT_NULL;
  }
  out.status = status;
  out.reader = reader;
  /* If the subscriber died mid-handshake the notification above still cleans up. */
  if (mach_msg(&out.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof out, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL) != MACH_MSG_SUCCESS) mach_msg_destroy(&out.header);
}

static void publisher_messages(void *ctx) {
  siberia_publisher *p = ctx;
  any_msg m;
  while (mach_msg(&m.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, p->service, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {
    if (m.header.msgh_id == MSG_CONNECT && m.header.msgh_size >= sizeof(connect_msg)) answer_connect(p, &m.connect);
    else if (m.header.msgh_id == MACH_NOTIFY_DEAD_NAME) drop_reader(p, m.dead.not_port);
    else mach_msg_destroy(&m.header);
  }
}

static void signal_cancelled_ctx(void *ctx) { dispatch_semaphore_signal(((siberia_publisher *)ctx)->cancelled); }

int siberia_publisher_create(const char *name, const siberia_format *f, int max_subscribers, siberia_publisher **out) {
  static _Atomic uint32_t counter;
  if (!out || !valid_name(name) || !f || !f->width || !f->height || !f->bytes_per_pixel || !f->pixel_format || max_subscribers < 1 || max_subscribers > MAX_READERS) return SIBERIA_ERR_ARGUMENT;
  siberia_publisher *p = calloc(1, sizeof *p);
  if (!p) return SIBERIA_ERR_SYSTEM;
  mach_port_t task = mach_task_self();
  memory_object_size_t size = sizeof(control_block);
  char service[128];
  int status = SIBERIA_ERR_SYSTEM;
  p->writing = NO_SLOT;
  p->max_readers = max_subscribers;
  if (mach_make_memory_entry_64(task, &size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_DEFAULT, &p->memory, MACH_PORT_NULL) != KERN_SUCCESS) goto fail;
  if (!(p->ctl = map_control(p->memory))) goto fail;
  p->ctl->magic = MAGIC;
  p->ctl->version = SIBERIA_VERSION;
  p->ctl->format = *f;
  p->ctl->slot_count = (uint32_t)max_subscribers + 2;
  for (uint32_t i = 0; i < p->ctl->slot_count; i++) if (!(p->surfaces[i] = make_surface(f))) goto fail;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p->service) != KERN_SUCCESS) goto fail;
  if (mach_port_insert_right(task, p->service, p->service, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) goto fail;
  snprintf(service, sizeof service, "com.cyberyak.siberia.%d.%u.%llu", getpid(), atomic_fetch_add(&counter, 1), siberia_now_ns());
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (bootstrap_register(bootstrap_port, service, p->service) != KERN_SUCCESS) goto fail;
#pragma clang diagnostic pop
  if (stream_path(name, p->path, sizeof p->path)) goto fail;
  if ((status = claim_name(p->path, service)) != SIBERIA_OK) { p->path[0] = 0; goto fail; }
  p->queue = dispatch_queue_create("siberia.publisher", DISPATCH_QUEUE_SERIAL);
  p->cancelled = dispatch_semaphore_create(0);
  p->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, p->service, 0, p->queue);
  dispatch_set_context(p->source, p);
  dispatch_source_set_event_handler_f(p->source, publisher_messages);
  dispatch_source_set_cancel_handler_f(p->source, signal_cancelled_ctx);
  dispatch_activate(p->source);
  *out = p;
  return SIBERIA_OK;
fail:
  siberia_publisher_destroy(p);
  return status;
}

void siberia_publisher_destroy(siberia_publisher *p) {
  if (!p) return;
  mach_port_t task = mach_task_self();
  if (p->ctl) atomic_store(&p->ctl->closed, 1);
  if (p->source) {
    dispatch_source_cancel(p->source);
    dispatch_semaphore_wait(p->cancelled, DISPATCH_TIME_FOREVER);
    dispatch_release(p->source);
    dispatch_release(p->queue);
    dispatch_release(p->cancelled);
  }
  if (p->path[0]) unlink(p->path);
  for (int i = 0; i < MAX_READERS; i++) if (p->tokens[i]) mach_port_deallocate(task, p->tokens[i]);
  if (p->service) { mach_port_mod_refs(task, p->service, MACH_PORT_RIGHT_RECEIVE, -1); mach_port_deallocate(task, p->service); }
  for (int i = 0; i < MAX_SLOTS; i++) if (p->surfaces[i]) CFRelease(p->surfaces[i]);
  unmap_control(p->ctl);
  if (p->memory) mach_port_deallocate(task, p->memory);
  free(p);
}

int siberia_publisher_begin(siberia_publisher *p, siberia_frame *frame) {
  if (!p || !frame || p->writing != NO_SLOT) return SIBERIA_ERR_ARGUMENT;
  control_block *c = p->ctl;
  uint64_t latest = atomic_load(&c->latest);
  int current = latest ? (int)(latest & ((1u << SLOT_BITS) - 1)) : NO_SLOT;
  uint32_t n = c->slot_count;
  for (uint32_t step = 0; step < n; step++) {
    int slot = (int)((p->next + step) % n), taken = slot == current;
    /* Sequentially consistent with the subscriber's claim: see docs/protocol.md. */
    for (int r = 0; !taken && r < p->max_readers; r++) taken = atomic_load(&c->readers[r].claimed) == slot && atomic_load(&c->readers[r].pid) != 0;
    if (taken) continue;
    p->next = (uint32_t)slot + 1;
    p->writing = slot;
    frame->slot = (uint32_t)slot;
    frame->surface = p->surfaces[slot];
    frame->sequence = p->sequence + 1;
    frame->time_ns = siberia_now_ns();
    return SIBERIA_OK;
  }
  return SIBERIA_NO_FREE_SLOT;
}

int siberia_publisher_commit(siberia_publisher *p, const siberia_frame *frame) {
  if (!p || !frame || p->writing != (int)frame->slot) return SIBERIA_ERR_ARGUMENT;
  slot_info *s = &p->ctl->slots[frame->slot];
  p->sequence++;
  s->sequence = p->sequence;
  s->time_ns = frame->time_ns;
  atomic_store(&p->ctl->latest, p->sequence << SLOT_BITS | frame->slot);
  p->writing = NO_SLOT;
  return SIBERIA_OK;
}

void siberia_publisher_cancel(siberia_publisher *p, const siberia_frame *frame) {
  if (p && frame && p->writing == (int)frame->slot) p->writing = NO_SLOT;
}

void siberia_publisher_demand(siberia_publisher *p, siberia_demand *out) {
  if (!p || !out) return;
  memset(out, 0, sizeof *out);
  int fullW = 0, fullH = 0, fullRate = 0;
  uint64_t now = siberia_now_ns();
  for (int i = 0; i < p->max_readers; i++) {
    reader_entry *r = &p->ctl->readers[i];
    if (!atomic_load_explicit(&r->pid, memory_order_acquire)) continue;
    uint32_t w = atomic_load_explicit(&r->wanted_width, memory_order_relaxed), h = atomic_load_explicit(&r->wanted_height, memory_order_relaxed), fps = atomic_load_explicit(&r->wanted_fps_milli, memory_order_relaxed);
    uint64_t next = atomic_load_explicit(&r->next_refresh_ns, memory_order_relaxed);
    out->subscribers++;
    if (w) { if (w > out->width) out->width = w; } else fullW = 1;
    if (h) { if (h > out->height) out->height = h; } else fullH = 1;
    if (fps) { if (fps > out->fps_milli) out->fps_milli = fps; } else fullRate = 1;
    if (next > now && (!out->next_refresh_ns || next < out->next_refresh_ns)) out->next_refresh_ns = next;
  }
  if (fullW) out->width = 0;
  if (fullH) out->height = 0;
  if (fullRate) out->fps_milli = 0;
}

int siberia_publisher_slot_count(const siberia_publisher *p) { return p ? (int)p->ctl->slot_count : 0; }
IOSurfaceRef siberia_publisher_surface(const siberia_publisher *p, int slot) { return p && slot >= 0 && slot < (int)p->ctl->slot_count ? p->surfaces[slot] : NULL; }

/* ---- Subscriber ---- */

static void signal_cancelled_sub(void *ctx) { dispatch_semaphore_signal(((siberia_subscriber *)ctx)->cancelled); }

static void subscriber_messages(void *ctx) {
  siberia_subscriber *s = ctx;
  any_msg m;
  while (mach_msg(&m.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, s->token, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {
    if (m.header.msgh_id == MACH_NOTIFY_DEAD_NAME) { atomic_store(&s->dead, 1); mach_port_deallocate(mach_task_self(), m.dead.not_port); }
    else mach_msg_destroy(&m.header);
  }
}

int siberia_subscriber_connect(const char *name, siberia_subscriber **out) {
  if (!out || !valid_name(name)) return SIBERIA_ERR_ARGUMENT;
  char path[1024], service[128];
  int pid, status = SIBERIA_ERR_SYSTEM;
  mach_port_t task = mach_task_self(), reply = MACH_PORT_NULL, memory = MACH_PORT_NULL, previous = MACH_PORT_NULL;
  if (stream_path(name, path, sizeof path)) return SIBERIA_ERR_SYSTEM;
  if (read_entry(path, &pid, service, sizeof service) || !pid_alive(pid)) return SIBERIA_ERR_NOT_FOUND;
  siberia_subscriber *s = calloc(1, sizeof *s);
  if (!s) return SIBERIA_ERR_SYSTEM;
  if (bootstrap_look_up(bootstrap_port, service, &s->service) != KERN_SUCCESS) { status = SIBERIA_ERR_NOT_FOUND; goto fail; }
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &s->token) != KERN_SUCCESS) goto fail;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) goto fail;

  connect_msg m;
  memset(&m, 0, sizeof m);
  m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
  m.header.msgh_size = sizeof m;
  m.header.msgh_remote_port = s->service;
  m.header.msgh_local_port = reply;
  m.header.msgh_id = MSG_CONNECT;
  m.body.msgh_descriptor_count = 1;
  m.token.type = MACH_MSG_PORT_DESCRIPTOR;
  m.token.disposition = MACH_MSG_TYPE_MAKE_SEND;
  m.token.name = s->token;
  m.version = SIBERIA_VERSION;
  m.pid = getpid();
  if (mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL) != MACH_MSG_SUCCESS) { status = SIBERIA_ERR_NOT_FOUND; goto fail; }
  any_msg in;
  if (mach_msg(&in.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, reply, 3000, MACH_PORT_NULL) != MACH_MSG_SUCCESS) goto fail;
  if (in.header.msgh_id != MSG_CONNECTED) { mach_msg_destroy(&in.header); goto fail; }
  if (in.connected.status != SIBERIA_OK) { status = in.connected.status; mach_msg_destroy(&in.header); goto fail; }
  memory = in.connected.control.name;
  s->ctl = map_control(memory);
  mach_port_deallocate(task, memory);
  for (int i = 0; i < MAX_SLOTS; i++) {
    mach_port_t p = in.connected.surfaces[i].name;
    if (p == MACH_PORT_NULL) continue;
    s->surfaces[i] = IOSurfaceLookupFromMachPort(p);
    mach_port_deallocate(task, p);
  }
  if (!s->ctl) goto fail;
  if (s->ctl->magic != MAGIC || s->ctl->version != SIBERIA_VERSION) { status = SIBERIA_ERR_VERSION; goto fail; }
  if (in.connected.reader < 0 || in.connected.reader >= MAX_READERS) goto fail;
  s->me = &s->ctl->readers[in.connected.reader];
  for (uint32_t i = 0; i < s->ctl->slot_count; i++) if (!s->surfaces[i]) goto fail;

  s->cancelled = dispatch_semaphore_create(0);
  s->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, s->token, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
  dispatch_set_context(s->source, s);
  dispatch_source_set_event_handler_f(s->source, subscriber_messages);
  dispatch_source_set_cancel_handler_f(s->source, signal_cancelled_sub);
  dispatch_activate(s->source);
  /* Notifies us if the publisher crashes. */
  if (mach_port_request_notification(task, s->service, MACH_NOTIFY_DEAD_NAME, 1, s->token, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) != KERN_SUCCESS) atomic_store(&s->dead, 1);
  mach_port_mod_refs(task, reply, MACH_PORT_RIGHT_RECEIVE, -1);
  *out = s;
  return SIBERIA_OK;
fail:
  if (reply) mach_port_mod_refs(task, reply, MACH_PORT_RIGHT_RECEIVE, -1);
  siberia_subscriber_close(s);
  return status;
}

void siberia_subscriber_close(siberia_subscriber *s) {
  if (!s) return;
  mach_port_t task = mach_task_self();
  if (s->me) atomic_store(&s->me->claimed, NO_SLOT);
  if (s->source) {
    dispatch_source_cancel(s->source);
    dispatch_semaphore_wait(s->cancelled, DISPATCH_TIME_FOREVER);
    dispatch_release(s->source);
    dispatch_release(s->cancelled);
  }
  if (s->token) mach_port_mod_refs(task, s->token, MACH_PORT_RIGHT_RECEIVE, -1); /* tells the publisher */
  if (s->service) mach_port_deallocate(task, s->service);
  for (int i = 0; i < MAX_SLOTS; i++) if (s->surfaces[i]) CFRelease(s->surfaces[i]);
  unmap_control(s->ctl);
  free(s);
}

void siberia_subscriber_format(const siberia_subscriber *s, siberia_format *out) { if (s && out) *out = s->ctl->format; }

int siberia_subscriber_acquire(siberia_subscriber *s, siberia_frame *frame) {
  if (!s || !frame) return SIBERIA_ERR_ARGUMENT;
  if (atomic_load_explicit(&s->held, memory_order_acquire)) return SIBERIA_BUSY;
  if (atomic_load_explicit(&s->dead, memory_order_relaxed) || atomic_load_explicit(&s->ctl->closed, memory_order_relaxed)) return SIBERIA_CLOSED;
  control_block *c = s->ctl;
  for (int attempt = 0; attempt < 64; attempt++) {
    uint64_t latest = atomic_load(&c->latest), sequence = latest >> SLOT_BITS;
    if (!latest || sequence == s->last) return SIBERIA_NO_NEW_FRAME;
    int slot = (int)(latest & ((1u << SLOT_BITS) - 1));
    if (slot >= (int)c->slot_count) return SIBERIA_ERR_SYSTEM;
#ifdef SIBERIA_TEST_YIELD
    sched_yield(); /* widens the race the recheck below exists to catch */
#endif
    atomic_store(&s->me->claimed, slot);
#ifndef SIBERIA_TEST_NO_RECHECK
    /* If latest still names this slot, the publisher can't reuse it without seeing our claim. */
    if (atomic_load(&c->latest) != latest) { atomic_store(&s->me->claimed, NO_SLOT); continue; }
#endif
    frame->slot = (uint32_t)slot;
    frame->surface = s->surfaces[slot];
    frame->sequence = sequence;
    frame->time_ns = c->slots[slot].time_ns;
    s->last = sequence;
    atomic_store_explicit(&s->held, 1, memory_order_release);
    return SIBERIA_OK;
  }
  return SIBERIA_NO_NEW_FRAME;
}

void siberia_subscriber_release(siberia_subscriber *s) {
  if (!s) return;
  atomic_store(&s->me->claimed, NO_SLOT);
  atomic_store_explicit(&s->held, 0, memory_order_release);
}

void siberia_subscriber_request(siberia_subscriber *s, uint32_t width, uint32_t height, uint32_t fps_milli) {
  if (!s) return;
  atomic_store_explicit(&s->me->wanted_width, width, memory_order_relaxed);
  atomic_store_explicit(&s->me->wanted_height, height, memory_order_relaxed);
  atomic_store_explicit(&s->me->wanted_fps_milli, fps_milli, memory_order_relaxed);
}

void siberia_subscriber_report_refresh(siberia_subscriber *s, uint64_t next_refresh_ns) {
  if (s) atomic_store_explicit(&s->me->next_refresh_ns, next_refresh_ns, memory_order_relaxed);
}

int siberia_subscriber_slot_count(const siberia_subscriber *s) { return s ? (int)s->ctl->slot_count : 0; }
IOSurfaceRef siberia_subscriber_surface(const siberia_subscriber *s, int slot) { return s && slot >= 0 && slot < (int)s->ctl->slot_count ? s->surfaces[slot] : NULL; }
