/*
 * platform_event.h: portable event loop abstraction.
 *
 * Linux uses epoll, macOS uses kqueue, Windows uses WSAPoll.
 * The API mirrors a simplified epoll interface.
 */
#ifndef DEC_PLATFORM_EVENT_H
#define DEC_PLATFORM_EVENT_H 1

#include "platform.h"
#include <stdint.h>

/* Event flags (portable subset of epoll flags) */
#define PLAT_EV_IN  (1u << 0)
#define PLAT_EV_OUT (1u << 1)
#define PLAT_EV_ERR (1u << 2)
#define PLAT_EV_HUP (1u << 3)

/* A single ready event returned by platform_event_wait() */
typedef struct
{
   int fd;
   uint32_t events; /* bitmask of PLAT_EV_* */
} platform_event_t;

/* Opaque event loop handle */
typedef struct
{
#ifdef AIMEE_LINUX
   int epoll_fd;
#elif defined(AIMEE_MACOS)
   int kqueue_fd;
#elif defined(AIMEE_WINDOWS)
   /* WSAPoll uses a flat pollfd array, managed internally */
   void *poll_set; /* pointer to internal pollfd array */
   int poll_cap;
   int poll_count;
#endif
} platform_evloop_t;

/* Create a new event loop. Returns 0 on success, -1 on error. */
int platform_evloop_create(platform_evloop_t *loop);

/* Destroy the event loop and free resources. */
void platform_evloop_destroy(platform_evloop_t *loop);

/* Modify the watched events for |fd|.
 * Returns 0 on success, -1 on error. */
int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events);

#endif /* DEC_PLATFORM_EVENT_H */
