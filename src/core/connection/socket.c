/* Portable TCP connection engine shared by thinclient, aimee-server, and
 * aimee-kb. Keep OS socket details in this module rather than product clients. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <aimee/core/connection/socket.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define NETDBG(...)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (getenv("AIMEE_NET_DEBUG"))                                                               \
         fprintf(stderr, "[net] " __VA_ARGS__);                                                    \
   } while (0)

static int ensure_winsock(void)
{
   static LONG started = 0;
   if (InterlockedCompareExchange(&started, 1, 0) == 0)
   {
      WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
      {
         InterlockedExchange(&started, 0);
         return -1;
      }
   }
   return 0;
}

int aimee_core_socket_connect(const char *host, const char *port, int timeout_ms)
{
   if (timeout_ms <= 0)
      timeout_ms = 10000;

   aimee_core_control_t control;
   int connected = -1;
   if (aimee_core_control_init_timeout(&control, timeout_ms, 0, NULL, NULL) != AIMEE_CORE_OK ||
       aimee_core_socket_connect_controlled(host, port, 0, &control, &connected) != AIMEE_CORE_OK)
      return -1;
   return connected;
}

aimee_core_result_t aimee_core_socket_connect_controlled(const char *host, const char *port,
                                                         unsigned flags,
                                                         const aimee_core_control_t *control,
                                                         int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   if (!host || !*host || !port || !*port || !control || !out_fd ||
       (flags & ~(AIMEE_CORE_CONNECT_NUMERIC_HOST | AIMEE_CORE_CONNECT_NONBLOCKING)))
      return AIMEE_CORE_INVALID;
   NETDBG("connect host=%s port=%s\n", host, port);
   if (ensure_winsock() != 0)
   {
      NETDBG("Winsock initialization failed\n");
      return AIMEE_CORE_IO_ERROR;
   }

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   hints.ai_flags = (flags & AIMEE_CORE_CONNECT_NUMERIC_HOST) ? AI_NUMERICHOST : 0;
   struct addrinfo *addresses = NULL;
   if (getaddrinfo(host, port, &hints, &addresses) != 0 || !addresses)
   {
      NETDBG("address resolution failed for %s:%s\n", host, port);
      return AIMEE_CORE_IO_ERROR;
   }

   SOCKET connected = INVALID_SOCKET;
   aimee_core_result_t result = AIMEE_CORE_IO_ERROR;
   for (struct addrinfo *address = addresses; address; address = address->ai_next)
   {
      result = aimee_core_control_check(control);
      if (result != AIMEE_CORE_OK)
         break;
      SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate == INVALID_SOCKET)
         continue;
      u_long nonblocking = 1;
      if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0)
      {
         closesocket(candidate);
         continue;
      }
      int rc = connect(candidate, address->ai_addr, (int)address->ai_addrlen);
      if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK)
      {
         result = aimee_core_wait_fd((int)(intptr_t)candidate, AIMEE_CORE_WAIT_WRITE, control);
         int error = 0;
         int error_length = sizeof(error);
         if (result == AIMEE_CORE_OK &&
             getsockopt(candidate, SOL_SOCKET, SO_ERROR, (char *)&error, &error_length) == 0 &&
             error == 0)
            rc = 0;
         else
            rc = -1;
      }
      if (rc == 0)
      {
         if (!(flags & AIMEE_CORE_CONNECT_NONBLOCKING))
         {
            nonblocking = 0;
            if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0)
            {
               closesocket(candidate);
               result = AIMEE_CORE_IO_ERROR;
               continue;
            }
         }
         result = aimee_core_control_check(control);
         if (result == AIMEE_CORE_OK)
         {
            connected = candidate;
            break;
         }
      }
      closesocket(candidate);
      if (result == AIMEE_CORE_TIMEOUT || result == AIMEE_CORE_CANCELLED)
         break;
   }
   freeaddrinfo(addresses);
   if (connected == INVALID_SOCKET)
   {
      NETDBG("all connection candidates failed for %s:%s\n", host, port);
      return result == AIMEE_CORE_OK ? AIMEE_CORE_IO_ERROR : result;
   }
   NETDBG("connected fd=%d\n", (int)(intptr_t)connected);
   *out_fd = (int)(intptr_t)connected;
   return AIMEE_CORE_OK;
}

int aimee_core_socket_set_timeouts(int fd, int receive_timeout_ms, int send_timeout_ms)
{
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   DWORD receive = receive_timeout_ms > 0 ? (DWORD)receive_timeout_ms : 0;
   DWORD send_timeout = send_timeout_ms > 0 ? (DWORD)send_timeout_ms : 0;
   return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receive, sizeof(receive)) ==
                      0 &&
                  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout,
                             sizeof(send_timeout)) == 0
              ? 0
              : -1;
}

int aimee_core_socket_write_all(int fd, const void *buffer, size_t length)
{
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   const char *bytes = buffer;
   size_t offset = 0;
   while (offset < length)
   {
      int chunk = length - offset > INT_MAX ? INT_MAX : (int)(length - offset);
      int written = send(socket_fd, bytes + offset, chunk, 0);
      if (written <= 0)
         return -1;
      offset += (size_t)written;
   }
   return 0;
}

aimee_core_result_t aimee_core_socket_write_all_controlled(int fd, const void *buffer,
                                                           size_t length,
                                                           const aimee_core_control_t *control,
                                                           size_t *bytes_written)
{
   if (bytes_written)
      *bytes_written = 0;
   if (fd < 0 || (!buffer && length) || !control || !bytes_written)
      return AIMEE_CORE_INVALID;
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   const char *bytes = buffer;
   while (*bytes_written < length)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      int chunk = length - *bytes_written > INT_MAX ? INT_MAX : (int)(length - *bytes_written);
      int written = send(socket_fd, bytes + *bytes_written, chunk, 0);
      if (written > 0)
      {
         *bytes_written += (size_t)written;
         continue;
      }
      int error = WSAGetLastError();
      if (written < 0 && error == WSAEINTR)
         continue;
      if (written < 0 && error == WSAEWOULDBLOCK)
      {
         checked = aimee_core_wait_fd(fd, AIMEE_CORE_WAIT_WRITE, control);
         if (checked == AIMEE_CORE_OK)
            continue;
         return checked;
      }
      return AIMEE_CORE_IO_ERROR;
   }
   return aimee_core_control_check(control);
}

long aimee_core_socket_read(int fd, void *buffer, size_t length)
{
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   int capacity = length > INT_MAX ? INT_MAX : (int)length;
   return (long)recv(socket_fd, buffer, capacity, 0);
}

aimee_core_result_t aimee_core_socket_read_controlled(int fd, void *buffer, size_t length,
                                                      const aimee_core_control_t *control,
                                                      size_t *bytes_read)
{
   if (bytes_read)
      *bytes_read = 0;
   if (fd < 0 || !buffer || !length || !control || !bytes_read)
      return AIMEE_CORE_INVALID;
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   for (;;)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      int capacity = length > INT_MAX ? INT_MAX : (int)length;
      int received = recv(socket_fd, buffer, capacity, 0);
      if (received > 0)
      {
         *bytes_read = (size_t)received;
         return aimee_core_control_check(control);
      }
      if (received == 0)
         return AIMEE_CORE_EOF;
      int error = WSAGetLastError();
      if (error == WSAEINTR)
         continue;
      if (error == WSAEWOULDBLOCK)
      {
         checked = aimee_core_wait_fd(fd, AIMEE_CORE_WAIT_READ, control);
         if (checked == AIMEE_CORE_OK)
            continue;
         return checked;
      }
      return AIMEE_CORE_IO_ERROR;
   }
}

int aimee_core_socket_wait_readable(int fd, int timeout_ms)
{
   SOCKET socket_fd = (SOCKET)(intptr_t)fd;
   fd_set readable;
   FD_ZERO(&readable);
   FD_SET(socket_fd, &readable);
   struct timeval timeout = {.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0,
                             .tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0};
   int rc = select(0, &readable, NULL, NULL, &timeout);
   return rc > 0 ? 1 : (rc == 0 ? 0 : -1);
}

void aimee_core_socket_close(int fd)
{
   if (fd >= 0)
      closesocket((SOCKET)(intptr_t)fd);
}

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int aimee_core_socket_connect(const char *host, const char *port, int timeout_ms)
{
   if (timeout_ms <= 0)
      timeout_ms = 10000;

   aimee_core_control_t control;
   int connected = -1;
   if (aimee_core_control_init_timeout(&control, timeout_ms, 0, NULL, NULL) != AIMEE_CORE_OK ||
       aimee_core_socket_connect_controlled(host, port, 0, &control, &connected) != AIMEE_CORE_OK)
      return -1;
   return connected;
}

aimee_core_result_t aimee_core_socket_connect_addresses(const struct addrinfo *addresses,
                                                        unsigned flags,
                                                        const aimee_core_control_t *control,
                                                        int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   if (!addresses || !control || !out_fd ||
       (flags & ~(AIMEE_CORE_CONNECT_NUMERIC_HOST | AIMEE_CORE_CONNECT_NONBLOCKING)))
      return AIMEE_CORE_INVALID;
   aimee_core_result_t result = AIMEE_CORE_IO_ERROR;
   for (const struct addrinfo *address = addresses; address; address = address->ai_next)
   {
      result = aimee_core_control_check(control);
      if (result != AIMEE_CORE_OK)
         return result;
      int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate < 0)
         continue;
      int descriptor_flags = fcntl(candidate, F_GETFD);
      if (descriptor_flags >= 0)
         (void)fcntl(candidate, F_SETFD, descriptor_flags | FD_CLOEXEC);
      int original_flags = fcntl(candidate, F_GETFL, 0);
      if (original_flags < 0 || fcntl(candidate, F_SETFL, original_flags | O_NONBLOCK) != 0)
      {
         close(candidate);
         continue;
      }
      int rc = connect(candidate, address->ai_addr, address->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS)
      {
         result = aimee_core_wait_fd(candidate, AIMEE_CORE_WAIT_WRITE, control);
         int error = 0;
         socklen_t error_length = sizeof(error);
         if (result == AIMEE_CORE_OK &&
             (getsockopt(candidate, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error))
            result = AIMEE_CORE_IO_ERROR;
      }
      else
         result = rc == 0 ? AIMEE_CORE_OK : AIMEE_CORE_IO_ERROR;
      if (result == AIMEE_CORE_OK && !(flags & AIMEE_CORE_CONNECT_NONBLOCKING) &&
          fcntl(candidate, F_SETFL, original_flags) != 0)
         result = AIMEE_CORE_IO_ERROR;
      if (result == AIMEE_CORE_OK)
      {
         int one = 1;
         (void)setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
         result = aimee_core_control_check(control);
         if (result == AIMEE_CORE_OK)
         {
            *out_fd = candidate;
            return AIMEE_CORE_OK;
         }
      }
      close(candidate);
      if (result == AIMEE_CORE_TIMEOUT || result == AIMEE_CORE_CANCELLED)
         return result;
   }
   return result;
}

aimee_core_result_t aimee_core_socket_connect_controlled(const char *host, const char *port,
                                                         unsigned flags,
                                                         const aimee_core_control_t *control,
                                                         int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   if (!host || !*host || !port || !*port || !control || !out_fd ||
       (flags & ~(AIMEE_CORE_CONNECT_NUMERIC_HOST | AIMEE_CORE_CONNECT_NONBLOCKING)))
      return AIMEE_CORE_INVALID;
   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = (flags & AIMEE_CORE_CONNECT_NUMERIC_HOST) ? AI_NUMERICHOST : 0;
   struct addrinfo *addresses = NULL;
   if (getaddrinfo(host, port, &hints, &addresses) != 0 || !addresses)
      return AIMEE_CORE_IO_ERROR;
   aimee_core_result_t result =
       aimee_core_socket_connect_addresses(addresses, flags, control, out_fd);
   freeaddrinfo(addresses);
   return result;
}

int aimee_core_socket_set_timeouts(int fd, int receive_timeout_ms, int send_timeout_ms)
{
   struct timeval receive = {.tv_sec = receive_timeout_ms > 0 ? receive_timeout_ms / 1000 : 0,
                             .tv_usec =
                                 receive_timeout_ms > 0 ? (receive_timeout_ms % 1000) * 1000 : 0};
   struct timeval send_timeout = {.tv_sec = send_timeout_ms > 0 ? send_timeout_ms / 1000 : 0,
                                  .tv_usec =
                                      send_timeout_ms > 0 ? (send_timeout_ms % 1000) * 1000 : 0};
   return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)) == 0 &&
                  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) == 0
              ? 0
              : -1;
}

int aimee_core_socket_write_all(int fd, const void *buffer, size_t length)
{
   const char *bytes = buffer;
   size_t offset = 0;
   while (offset < length)
   {
      ssize_t written = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
      if (written < 0 && errno == EINTR)
         continue;
      if (written <= 0)
         return -1;
      offset += (size_t)written;
   }
   return 0;
}

aimee_core_result_t aimee_core_socket_write_all_controlled(int fd, const void *buffer,
                                                           size_t length,
                                                           const aimee_core_control_t *control,
                                                           size_t *bytes_written)
{
   if (bytes_written)
      *bytes_written = 0;
   if (fd < 0 || (!buffer && length) || !control || !bytes_written)
      return AIMEE_CORE_INVALID;
   const unsigned char *bytes = buffer;
   while (*bytes_written < length)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      ssize_t written = send(fd, bytes + *bytes_written, length - *bytes_written, MSG_NOSIGNAL);
      if (written > 0)
      {
         *bytes_written += (size_t)written;
         continue;
      }
      if (written < 0 && errno == EINTR)
         continue;
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         checked = aimee_core_wait_fd(fd, AIMEE_CORE_WAIT_WRITE, control);
         if (checked == AIMEE_CORE_OK)
            continue;
         return checked;
      }
      return AIMEE_CORE_IO_ERROR;
   }
   return aimee_core_control_check(control);
}

long aimee_core_socket_read(int fd, void *buffer, size_t length)
{
   for (;;)
   {
      ssize_t received = recv(fd, buffer, length, 0);
      if (received < 0 && errno == EINTR)
         continue;
      return (long)received;
   }
}

aimee_core_result_t aimee_core_socket_read_controlled(int fd, void *buffer, size_t length,
                                                      const aimee_core_control_t *control,
                                                      size_t *bytes_read)
{
   if (bytes_read)
      *bytes_read = 0;
   if (fd < 0 || !buffer || !length || !control || !bytes_read)
      return AIMEE_CORE_INVALID;
   for (;;)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      ssize_t received = recv(fd, buffer, length, 0);
      if (received > 0)
      {
         *bytes_read = (size_t)received;
         return aimee_core_control_check(control);
      }
      if (received == 0)
         return AIMEE_CORE_EOF;
      if (errno == EINTR)
         continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
         checked = aimee_core_wait_fd(fd, AIMEE_CORE_WAIT_READ, control);
         if (checked == AIMEE_CORE_OK)
            continue;
         return checked;
      }
      return AIMEE_CORE_IO_ERROR;
   }
}

int aimee_core_socket_wait_readable(int fd, int timeout_ms)
{
   for (;;)
   {
      struct pollfd readable = {.fd = fd, .events = POLLIN};
      int rc = poll(&readable, 1, timeout_ms);
      if (rc > 0)
      {
         if (readable.revents & POLLIN)
            return 1;
         return -1;
      }
      if (rc == 0)
         return 0;
      if (errno != EINTR)
         return -1;
   }
}

void aimee_core_socket_close(int fd)
{
   if (fd >= 0)
      close(fd);
}
#endif
