/* platform_event.c: epoll-based event loop (Linux) */
#include "platform_event.h"
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

int platform_evloop_create(platform_evloop_t *loop)
{
   loop->epoll_fd = epoll_create1(0);
   return (loop->epoll_fd < 0) ? -1 : 0;
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   if (loop->epoll_fd >= 0)
   {
      close(loop->epoll_fd);
      loop->epoll_fd = -1;
   }
}

static uint32_t to_epoll(uint32_t flags)
{
   uint32_t ev = 0;
   if (flags & PLAT_EV_IN)
      ev |= EPOLLIN;
   if (flags & PLAT_EV_OUT)
      ev |= EPOLLOUT;
   return ev;
}

int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   struct epoll_event ev = {.events = to_epoll(events), .data.fd = fd};
   return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}
