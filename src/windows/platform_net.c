/* platform_net.c: TCP client over Winsock (Windows). */
#include "platform_net.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define NETDBG(...)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (getenv("AIMEE_NET_DEBUG"))                                                               \
         fprintf(stderr, "[net] " __VA_ARGS__);                                                    \
   } while (0)

/* Winsock requires one-time initialization before any socket call. Bring it up
 * lazily on first connect; the single startup is intentionally never paired
 * with WSACleanup — it lives for the process lifetime (standard for CLIs). */
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

int platform_net_connect(const char *host, const char *port, int timeout_ms)
{
   if (!host || !port)
      return -1;
   NETDBG("connect host=%s port=%s\n", host, port);
   if (ensure_winsock() != 0)
   {
      NETDBG("ensure_winsock failed wsa=%d\n", WSAGetLastError());
      return -1;
   }
   if (timeout_ms <= 0)
      timeout_ms = 10000;

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;

   struct addrinfo *res = NULL;
   int gai = getaddrinfo(host, port, &hints, &res);
   if (gai != 0 || !res)
   {
      NETDBG("getaddrinfo failed gai=%d wsa=%d\n", gai, WSAGetLastError());
      return -1;
   }

   SOCKET sock = INVALID_SOCKET;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (sock == INVALID_SOCKET)
         continue;
      NETDBG("connect() fam=%d\n", ai->ai_family);

      /* Non-blocking connect with a bounded select() wait. */
      u_long nb = 1;
      ioctlsocket(sock, FIONBIO, &nb);

      int rc = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
      if (rc == 0)
      {
         /* connected immediately */
      }
      else if (WSAGetLastError() == WSAEWOULDBLOCK)
      {
         fd_set wf;
         FD_ZERO(&wf);
         FD_SET(sock, &wf);
         struct timeval tv;
         tv.tv_sec = timeout_ms / 1000;
         tv.tv_usec = (timeout_ms % 1000) * 1000;
         rc = select(0, NULL, &wf, NULL, &tv);
         if (rc <= 0)
         {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
         }
         int err = 0;
         int elen = sizeof(err);
         if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0)
         {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
         }
      }
      else
      {
         NETDBG("connect() immediate failure wsa=%d\n", WSAGetLastError());
         closesocket(sock);
         sock = INVALID_SOCKET;
         continue;
      }

      /* Restore blocking mode for simple send/recv framing. */
      nb = 0;
      ioctlsocket(sock, FIONBIO, &nb);
      break;
   }

   freeaddrinfo(res);
   if (sock == INVALID_SOCKET)
   {
      NETDBG("all candidates failed for %s:%s\n", host, port);
      return -1;
   }
   NETDBG("connected fd=%d\n", (int)(intptr_t)sock);
   return (int)(intptr_t)sock;
}

int platform_net_send_all(int fd, const void *buf, size_t len)
{
   SOCKET sock = (SOCKET)(intptr_t)fd;
   const char *p = (const char *)buf;
   size_t off = 0;
   while (off < len)
   {
      int chunk = (len - off > INT_MAX) ? INT_MAX : (int)(len - off);
      int n = send(sock, p + off, chunk, 0);
      if (n <= 0)
         return -1;
      off += (size_t)n;
   }
   return 0;
}

long platform_net_recv(int fd, void *buf, size_t len)
{
   SOCKET sock = (SOCKET)(intptr_t)fd;
   int cap = (len > INT_MAX) ? INT_MAX : (int)len;
   int n = recv(sock, (char *)buf, cap, 0);
   return (long)n;
}

void platform_net_close(int fd)
{
   if (fd >= 0)
      closesocket((SOCKET)(intptr_t)fd);
}
