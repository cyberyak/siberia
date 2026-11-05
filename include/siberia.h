/* Siberia: live video shared between apps on one Mac, in GPU memory.
 *
 * Connecting uses Mach messages. After that, a frame costs a few atomic operations in shared
 * memory: no messages, system calls or copies. Draft protocol: docs/protocol.md.
 *
 * A publisher's calls must come from one thread at a time, and so must a subscriber's. The
 * exceptions are siberia_subscriber_release, which is safe from a GPU completion handler, and
 * the two calls that report what a subscriber wants.
 */
#pragma once
#include <IOSurface/IOSurfaceRef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIBERIA_VERSION 1
#define SIBERIA_NAME_MAX 63
#define SIBERIA_MAX_SUBSCRIBERS 8

typedef enum {
  SIBERIA_OK = 0,
  SIBERIA_NO_NEW_FRAME = 1,  /* normal: nothing newer than the last frame */
  SIBERIA_NO_FREE_SLOT = 2,  /* drop this frame; only expected while a crash is being cleaned up */
  SIBERIA_BUSY = 3,          /* release the frame you hold first */
  SIBERIA_CLOSED = 4,        /* the publisher has gone */
  SIBERIA_ERR_ARGUMENT = -1,
  SIBERIA_ERR_NAME_TAKEN = -2,
  SIBERIA_ERR_NOT_FOUND = -3,
  SIBERIA_ERR_FULL = -4,     /* the stream has all the subscribers it allows */
  SIBERIA_ERR_VERSION = -5,
  SIBERIA_ERR_SYSTEM = -6,
} siberia_status;

typedef struct siberia_publisher siberia_publisher;
typedef struct siberia_subscriber siberia_subscriber;

typedef struct {
  IOSurfaceRef surface;  /* owned by Siberia; don't release it */
  uint32_t slot;
  uint64_t sequence;     /* increases by one per published frame */
  uint64_t time_ns;      /* on siberia_now_ns's clock */
} siberia_frame;

typedef struct {
  uint32_t width, height;
  uint32_t pixel_format;     /* IOSurface four-character code, such as 'BGRA' */
  uint32_t bytes_per_pixel;
  uint32_t color_space;      /* passed through untouched: 0 sRGB, others to be defined */
} siberia_format;

/* The most any connected subscriber currently needs. Zero size or rate means "everything". */
typedef struct {
  uint32_t subscribers;
  uint32_t width, height;
  uint32_t fps_milli;        /* 60000 is 60 fps */
  uint64_t next_refresh_ns;  /* the soonest display refresh reported; 0 if none */
} siberia_demand;

typedef struct {
  char name[SIBERIA_NAME_MAX + 1];
  int pid;
} siberia_stream_info;

/* The clock for every time in this API. All processes share it. */
uint64_t siberia_now_ns(void);
const char *siberia_status_text(int status);

/* Streams on this Mac, for this user. Returns how many were written. */
int siberia_list(siberia_stream_info *out, int max);

/* Allocates max_subscribers + 2 frame buffers, which guarantees a free one is always available. */
int siberia_publisher_create(const char *name, const siberia_format *format, int max_subscribers, siberia_publisher **out);
void siberia_publisher_destroy(siberia_publisher *p);
/* Gives you a buffer to render into. Follow with commit or cancel. */
int siberia_publisher_begin(siberia_publisher *p, siberia_frame *frame);
/* Call when the GPU has finished writing the buffer, not merely when the work is submitted. */
int siberia_publisher_commit(siberia_publisher *p, const siberia_frame *frame);
void siberia_publisher_cancel(siberia_publisher *p, const siberia_frame *frame);
void siberia_publisher_demand(siberia_publisher *p, siberia_demand *out);
/* Every buffer, for creating GPU textures up front. */
int siberia_publisher_slot_count(const siberia_publisher *p);
IOSurfaceRef siberia_publisher_surface(const siberia_publisher *p, int slot);

int siberia_subscriber_connect(const char *name, siberia_subscriber **out);
void siberia_subscriber_close(siberia_subscriber *s);
void siberia_subscriber_format(const siberia_subscriber *s, siberia_format *out);
/* Claims the newest frame, which stays valid until release. One frame at a time. */
int siberia_subscriber_acquire(siberia_subscriber *s, siberia_frame *frame);
void siberia_subscriber_release(siberia_subscriber *s);
/* Ask for less than everything. Zero means "everything". */
void siberia_subscriber_request(siberia_subscriber *s, uint32_t width, uint32_t height, uint32_t fps_milli);
void siberia_subscriber_report_refresh(siberia_subscriber *s, uint64_t next_refresh_ns);
int siberia_subscriber_slot_count(const siberia_subscriber *s);
IOSurfaceRef siberia_subscriber_surface(const siberia_subscriber *s, int slot);

#ifdef __cplusplus
}
#endif
