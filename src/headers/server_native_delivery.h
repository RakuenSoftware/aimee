/* Thin transport bridge only. Native encoding, account routing and publication
 * live in aimee-qwen. Call ONLY after normal per-request auth/capability checks.
 * This bridge forwards the attested principal, never caller identity headers. */
#ifndef AIMEE_SERVER_NATIVE_DELIVERY_H
#define AIMEE_SERVER_NATIVE_DELIVERY_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "server_conn_io.h"

static inline int server_native_delivery_route(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   if (!strcmp(method, "GET") && !strcmp(path, "/v1/native/bootstrap"))
      return 1;
   if (!strcmp(method, "POST") && !strcmp(path, "/v1/native/select"))
      return 1;
   const char *prefix = "/v1/native/leases/";
   if (strncmp(path, prefix, strlen(prefix)))
      return 0;
   const char *lease = path + strlen(prefix);
   if (strlen(lease) < 65)
      return 0;
   for (int i = 0; i < 64; i++)
      if (!((lease[i] >= '0' && lease[i] <= '9') || (lease[i] >= 'a' && lease[i] <= 'f')))
         return 0;
   if (lease[64] != '/')
      return 0;
   const char *name = lease + 65;
   return (!strcmp(method, "GET") &&
           (!strcmp(name, "index.json") || !strcmp(name, "compact.pack"))) ||
          (!strcmp(method, "POST") && !strcmp(name, "approve"));
}
#if defined(__linux__)
static inline int server_native_send(int fd, const void *data, size_t n)
{
   const char *p = data;
   while (n)
   {
      ssize_t sent = send(fd, p, n, MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR)
         continue;
      if (sent <= 0)
         return -1;
      p += sent;
      n -= (size_t)sent;
   }
   return 0;
}
#endif
/* Returns an HTTP error before any bytes have been relayed, or zero once the
 * backend response has started. A truncated response is closed, never appended
 * to with a second HTTP response. Constant 64 KiB transfer memory per request. */
static inline int server_native_delivery_forward(int client, const char *method, const char *path,
                                                 const char *principal, const char *body,
                                                 size_t body_len)
{
#if defined(__linux__)
   if (!server_native_delivery_route(method, path))
      return 404;
   if (!principal || !*principal || strlen(principal) > 1024)
      return 403;
   if (body_len > 131072)
      return 413;
   const char *socket_path = getenv("AIMEE_NATIVE_SOCKET");
   if (!socket_path || socket_path[0] != '/')
      return 503;
   struct sockaddr_un addr = {.sun_family = AF_UNIX};
   if (strlen(socket_path) >= sizeof(addr.sun_path))
      return 503;
   struct stat st;
   if (lstat(socket_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
       (st.st_mode & 0077))
      return 503;
   strcpy(addr.sun_path, socket_path);
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return 503;
   struct timeval timeout = {.tv_sec = 660};
   if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
       connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
   {
      close(fd);
      return 503;
   }
   struct ucred peer;
   socklen_t peer_len = sizeof(peer);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) || peer.uid != geteuid())
   {
      close(fd);
      return 503;
   }
   const char hex[] = "0123456789abcdef";
   char encoded[2049];
   size_t n = strlen(principal);
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)principal[i];
      encoded[2 * i] = hex[c >> 4];
      encoded[2 * i + 1] = hex[c & 15];
   }
   encoded[2 * n] = 0;
   char headers[4096];
   int len =
       snprintf(headers, sizeof(headers),
                "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: "
                "application/json\r\nContent-Length: %zu\r\nX-Aimee-Native-Principal: %s\r\n\r\n",
                method, path, body_len, encoded);
   if (len < 0 || (size_t)len >= sizeof(headers) || server_native_send(fd, headers, (size_t)len) ||
       (body_len && server_native_send(fd, body, body_len)))
   {
      close(fd);
      return 503;
   }
   int started = 0;
   size_t total = 0;
   char chunk[65536];
   for (;;)
   {
      ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
      if (got < 0 && errno == EINTR)
         continue;
      if (got <= 0)
         break;
      total += (size_t)got;
      if (total > ((size_t)1 << 30) + 65536)
         break;
      started = 1;
      if (server_conn_io_write_all(client, chunk, (int)got) < 0)
         break;
   }
   close(fd);
   return started ? 0 : 503;
#else
   (void)client;
   (void)method;
   (void)path;
   (void)principal;
   (void)body;
   (void)body_len;
   return 503; /* Linux SO_PEERCRED preview only. */
#endif
}
#endif
