#include <aimee/core/connection/control.h>

#include <limits.h>
#include <stddef.h>

#ifdef _WIN32
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <time.h>
#endif

int64_t aimee_core_now_ns(void)
{
#ifdef _WIN32
   ULONGLONG milliseconds = GetTickCount64();
   if (milliseconds > (ULONGLONG)INT64_MAX / 1000000ULL)
      return -1;
   return (int64_t)milliseconds * 1000000LL;
#else
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
       (int64_t)now.tv_sec > INT64_MAX / 1000000000LL)
      return -1;
   return (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
#endif
}

aimee_core_result_t aimee_core_control_init(aimee_core_control_t *control, int64_t deadline_ns,
                                            int cancel_poll_ms, aimee_core_cancel_fn cancelled,
                                            void *cancel_context)
{
   if (!control || deadline_ns < 0 || cancel_poll_ms < 0 || (cancelled && !cancel_poll_ms))
      return AIMEE_CORE_INVALID;
   control->deadline_ns = deadline_ns;
   control->cancel_poll_ms = cancel_poll_ms;
   control->cancelled = cancelled;
   control->cancel_context = cancel_context;
   return aimee_core_control_check(control);
}

aimee_core_result_t aimee_core_control_init_timeout(aimee_core_control_t *control, int timeout_ms,
                                                    int cancel_poll_ms,
                                                    aimee_core_cancel_fn cancelled,
                                                    void *cancel_context)
{
   if (!control || timeout_ms < 0)
      return AIMEE_CORE_INVALID;
   int64_t deadline = 0;
   if (timeout_ms)
   {
      int64_t now = aimee_core_now_ns();
      int64_t duration = (int64_t)timeout_ms * 1000000LL;
      if (now < 0 || now > INT64_MAX - duration)
         return AIMEE_CORE_INVALID;
      deadline = now + duration;
   }
   return aimee_core_control_init(control, deadline, cancel_poll_ms, cancelled, cancel_context);
}

aimee_core_result_t aimee_core_control_check(const aimee_core_control_t *control)
{
   if (!control)
      return AIMEE_CORE_INVALID;
   if (control->cancelled && control->cancelled(control->cancel_context))
      return AIMEE_CORE_CANCELLED;
   if (control->deadline_ns)
   {
      int64_t now = aimee_core_now_ns();
      if (now < 0)
         return AIMEE_CORE_IO_ERROR;
      if (now >= control->deadline_ns)
         return AIMEE_CORE_TIMEOUT;
   }
   return AIMEE_CORE_OK;
}

int aimee_core_control_remaining_ms(const aimee_core_control_t *control)
{
   if (aimee_core_control_check(control) != AIMEE_CORE_OK)
      return 0;
   int timeout = -1;
   if (control->deadline_ns)
   {
      int64_t now = aimee_core_now_ns();
      if (now < 0 || now >= control->deadline_ns)
         return 0;
      int64_t nanoseconds = control->deadline_ns - now;
      int64_t milliseconds = (nanoseconds + 999999LL) / 1000000LL;
      timeout = milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
   }
   if (control->cancelled && control->cancel_poll_ms > 0 &&
       (timeout < 0 || control->cancel_poll_ms < timeout))
      timeout = control->cancel_poll_ms;
   return timeout;
}

aimee_core_result_t aimee_core_wait_fd(int fd, aimee_core_wait_t events,
                                       const aimee_core_control_t *control)
{
   if (fd < 0 || !control || (events != AIMEE_CORE_WAIT_READ && events != AIMEE_CORE_WAIT_WRITE))
      return AIMEE_CORE_INVALID;
   for (;;)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      int timeout = aimee_core_control_remaining_ms(control);
#ifdef _WIN32
      SOCKET socket_fd = (SOCKET)(intptr_t)fd;
      fd_set readable;
      fd_set writable;
      FD_ZERO(&readable);
      FD_ZERO(&writable);
      FD_SET(socket_fd, events == AIMEE_CORE_WAIT_READ ? &readable : &writable);
      struct timeval value;
      struct timeval *timeout_value = NULL;
      if (timeout >= 0)
      {
         value.tv_sec = timeout / 1000;
         value.tv_usec = (timeout % 1000) * 1000;
         timeout_value = &value;
      }
      int rc = select(0, events == AIMEE_CORE_WAIT_READ ? &readable : NULL,
                      events == AIMEE_CORE_WAIT_WRITE ? &writable : NULL, NULL, timeout_value);
      if (rc < 0)
      {
         int error = WSAGetLastError();
         if (error == WSAEINTR)
            continue;
         return AIMEE_CORE_IO_ERROR;
      }
#else
      struct pollfd descriptor = {.fd = fd,
                                  .events = events == AIMEE_CORE_WAIT_READ ? POLLIN : POLLOUT};
      int rc = poll(&descriptor, 1, timeout);
      if (rc < 0)
      {
         if (errno == EINTR)
            continue;
         return AIMEE_CORE_IO_ERROR;
      }
      if (rc > 0)
      {
         if (descriptor.revents & (POLLERR | POLLNVAL))
            return AIMEE_CORE_IO_ERROR;
         if (descriptor.revents & descriptor.events)
            return aimee_core_control_check(control);
         if (descriptor.revents & POLLHUP)
            return AIMEE_CORE_IO_ERROR;
         continue;
      }
#endif
      if (rc > 0)
         return aimee_core_control_check(control);
      checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      /* A cancellation slice elapsed; check again and continue. A finite
       * deadline is reported by the check above, never as a generic I/O error. */
   }
}
