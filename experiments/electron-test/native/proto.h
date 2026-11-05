// Test wire format. Surfaces travel once as Mach ports; frames are small messages.
#pragma once
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <IOSurface/IOSurface.h>
#include <stdint.h>
#define SVC_OUT "com.cyberyak.siberia.out"   // Electron -> external receiver
#define SVC_IN  "com.cyberyak.siberia.in"    // external sender -> Electron
#define CELLS 40
#define BAR 32
enum { MSG_SURFACE = 1001, MSG_FRAME = 1002 };
typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t port; uint32_t sid, w, hh; } surf_msg;
typedef struct { mach_msg_header_t h; uint32_t sid; uint32_t seq; } frame_msg;
typedef union { surf_msg s; frame_msg f; char pad[256]; } rx_buf;

static inline kern_return_t send_surface(mach_port_t to, IOSurfaceRef s, uint32_t sid) {
  mach_port_t p = IOSurfaceCreateMachPort(s);
  surf_msg m = {0};
  m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  m.h.msgh_size = sizeof m; m.h.msgh_remote_port = to; m.h.msgh_id = MSG_SURFACE;
  m.b.msgh_descriptor_count = 1;
  m.port.name = p; m.port.disposition = MACH_MSG_TYPE_MOVE_SEND; m.port.type = MACH_MSG_PORT_DESCRIPTOR;
  m.sid = sid; m.w = (uint32_t)IOSurfaceGetWidth(s); m.hh = (uint32_t)IOSurfaceGetHeight(s);
  return mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, 0, 1000, 0);
}
static inline kern_return_t send_frame(mach_port_t to, uint32_t sid, uint32_t seq) {
  frame_msg m = {0};
  m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  m.h.msgh_size = sizeof m; m.h.msgh_remote_port = to; m.h.msgh_id = MSG_FRAME;
  m.sid = sid; m.seq = seq;
  return mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, 0, 20, 0);
}
static inline mach_port_t register_service(const char *name) {
  mach_port_t p = MACH_PORT_NULL;
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p);
  mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
  kern_return_t k = bootstrap_register(bootstrap_port, (char *)name, p);
  return k == KERN_SUCCESS ? p : MACH_PORT_NULL;
}
static inline mach_port_t lookup_service(const char *name) {
  mach_port_t p = MACH_PORT_NULL;
  return bootstrap_look_up(bootstrap_port, name, &p) == KERN_SUCCESS ? p : MACH_PORT_NULL;
}
