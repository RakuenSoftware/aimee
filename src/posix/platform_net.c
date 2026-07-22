/* platform_net.c: TCP client over BSD sockets (POSIX: Linux + macOS). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform_net.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int platform_net_connect(const char *host, const char *port, int timeout_ms)
{
   if (!host || !port)
      return -1;
   if (timeout_ms <= 0)
      timeout_ms = 10000;

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
   hints.ai_socktype = SOCK_STREAM;

   struct addrinfo *res = NULL;
   if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
      return -1;

   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
         continue;

      /* Non-blocking connect with a bounded wait, mirroring platform_ipc. */
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0)
         fcntl(fd, F_SETFL, flags | O_NONBLOCK);

      int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
      if (rc == 0)
      {
         /* connected immediately */
      }
      else if (errno == EINPROGRESS)
      {
         struct pollfd pfd = {.fd = fd, .events = POLLOUT};
         rc = poll(&pfd, 1, timeout_ms);
         if (rc <= 0)
         {
            close(fd);
            fd = -1;
            continue;
         }
         int err = 0;
         socklen_t elen = sizeof(err);
         if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
         {
            close(fd);
            fd = -1;
            continue;
         }
      }
      else
      {
         close(fd);
         fd = -1;
         continue;
      }

      /* Restore blocking mode for simple send/recv framing. */
      if (flags >= 0)
         fcntl(fd, F_SETFL, flags);
      break;
   }

   freeaddrinfo(res);
   return fd;
}

int platform_net_send_all(int fd, const void *buf, size_t len)
{
   const char *p = (const char *)buf;
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = send(fd, p + off, len - off, 0);
      if (n <= 0)
      {
         if (n < 0 && errno == EINTR)
            continue;
         return -1;
      }
      off += (size_t)n;
   }
   return 0;
}

long platform_net_recv(int fd, void *buf, size_t len)
{
   for (;;)
   {
      ssize_t n = recv(fd, buf, len, 0);
      if (n < 0 && errno == EINTR)
         continue;
      return (long)n;
   }
}

void platform_net_close(int fd)
{
   if (fd >= 0)
      close(fd);
}
