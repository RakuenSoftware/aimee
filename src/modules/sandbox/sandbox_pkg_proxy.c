/* sandbox_pkg_proxy.c — the socket I/O of the delegate-sandbox package proxy.
 *
 * C owns the resolver, sockets and byte transport. Request classification,
 * allowlisting and SSRF address policy live in the Go sandbox module and are
 * reached over the event bus. */

#include "sandbox_pkg_proxy.h"

#include "cJSON.h"
#include "log.h"
#include "headers/module_json_call.h"

#include <aimee/sandbox/module_api.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SBX_POLICY_TIMEOUT_MS 5000
#define SBX_POLICY_MAX_BODY   (256u * 1024u)

typedef enum
{
   SBX_REQ_INVALID = 0,
   SBX_REQ_API,
   SBX_REQ_CONNECT,
   SBX_REQ_ABSOLUTE,
} sbx_req_kind_t;

static cJSON *proxy_policy_call(uint32_t event_kind, uint32_t stage_id, cJSON *payload)
{
   return aimee_module_json_call(event_kind, stage_id, payload, SBX_POLICY_MAX_BODY,
                                 SBX_POLICY_TIMEOUT_MS, NULL);
}

/* Ask the sandbox module to parse and authorize one proxy request. The module
 * owns the default allowlist; omitting `allowlist` is distinct from explicitly
 * supplying an empty one. */
static int proxy_request_policy(const char *line, const char *allowlist, sbx_req_kind_t *kind,
                                char *host, size_t hostcap, int *port, int *allowed,
                                char **forward_head)
{
   if (!line || !kind || !host || hostcap == 0 || !port || !allowed || !forward_head)
      return -1;
   *kind = SBX_REQ_INVALID;
   host[0] = '\0';
   *port = 0;
   *allowed = 0;
   *forward_head = NULL;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   cJSON_AddStringToObject(payload, "line", line);
   if (allowlist)
      cJSON_AddStringToObject(payload, "allowlist", allowlist);

   cJSON *reply = proxy_policy_call(AIMEE_SANDBOX_EVENT_PROXY_REQUEST,
                                    AIMEE_SANDBOX_STAGE_PROXY_REQUEST, payload);
   if (!reply)
      return -1;
   const cJSON *reply_kind = cJSON_GetObjectItemCaseSensitive(reply, "kind");
   const cJSON *reply_host = cJSON_GetObjectItemCaseSensitive(reply, "host");
   const cJSON *reply_port = cJSON_GetObjectItemCaseSensitive(reply, "port");
   const cJSON *reply_allowed = cJSON_GetObjectItemCaseSensitive(reply, "allowed");
   const cJSON *reply_forward = cJSON_GetObjectItemCaseSensitive(reply, "forward_head");
   int rc = -1;
   if (cJSON_IsNumber(reply_kind) && reply_kind->valueint >= SBX_REQ_INVALID &&
       reply_kind->valueint <= SBX_REQ_ABSOLUTE && cJSON_IsNumber(reply_port) &&
       reply_port->valueint >= 0 && reply_port->valueint <= 65535 && cJSON_IsBool(reply_allowed) &&
       cJSON_IsString(reply_host) && strlen(reply_host->valuestring) < hostcap &&
       (!reply_forward || cJSON_IsString(reply_forward)) &&
       !(reply_kind->valueint == SBX_REQ_ABSOLUTE && cJSON_IsTrue(reply_allowed) && !reply_forward))
   {
      *kind = (sbx_req_kind_t)reply_kind->valueint;
      snprintf(host, hostcap, "%s", reply_host->valuestring);
      *port = reply_port->valueint;
      *allowed = cJSON_IsTrue(reply_allowed);
      *forward_head = reply_forward ? strdup(reply_forward->valuestring) : NULL;
      if (!reply_forward || *forward_head)
         rc = 0;
   }
   cJSON_Delete(reply);
   return rc;
}

/* Fail closed on malformed addresses and on an unavailable policy module. The
 * sockaddr is the exact resolver result that proxy_dial will connect, so there
 * is no DNS-rebinding gap between the checked address and the connected one. */
static int proxy_address_blocked(const struct sockaddr *address)
{
   if (!address)
      return 1;
   char ip[INET6_ADDRSTRLEN];
   const void *bytes = NULL;
   if (address->sa_family == AF_INET)
      bytes = &((const struct sockaddr_in *)address)->sin_addr;
   else if (address->sa_family == AF_INET6)
      bytes = &((const struct sockaddr_in6 *)address)->sin6_addr;
   else
      return 1;
   if (!inet_ntop(address->sa_family, bytes, ip, sizeof(ip)))
      return 1;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return 1;
   cJSON_AddStringToObject(payload, "ip", ip);
   cJSON *reply = proxy_policy_call(AIMEE_SANDBOX_EVENT_PROXY_ADDRESS,
                                    AIMEE_SANDBOX_STAGE_PROXY_ADDRESS, payload);
   if (!reply)
      return 1;
   const cJSON *blocked = cJSON_GetObjectItemCaseSensitive(reply, "blocked");
   int result = !cJSON_IsBool(blocked) || cJSON_IsTrue(blocked);
   cJSON_Delete(reply);
   return result;
}

/* --- socket I/O ---------------------------------------------------------------
 * The functions above only marshal policy calls. The plumbing below owns the
 * resolver, sockets and byte transport. */

static int write_all(int fd, const char *p, size_t n)
{
   size_t off = 0;
   while (off < n)
   {
      ssize_t w = write(fd, p + off, n - off);
      if (w < 0 && errno == EINTR)
         continue;
      if (w <= 0)
         return -1;
      off += (size_t)w;
   }
   return 0;
}

/* getaddrinfo(host,port), then for each candidate re-apply the SSRF guard to the
 * exact address we are about to dial (no rebinding window: the checked sockaddr IS
 * the connected one) and connect to the first that passes. Writes the dialed IP into
 * ipbuf. Returns the connected fd, or -1 (with *why set). */
static int proxy_dial(const char *host, int port, char *ipbuf, size_t ipcap, const char **why)
{
   *why = "resolve-failed";
   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   char portstr[8];
   snprintf(portstr, sizeof(portstr), "%d", port);
   struct addrinfo *res = NULL;
   if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
      return -1;

   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      if (proxy_address_blocked(ai->ai_addr))
      {
         *why = "ssrf-blocked";
         continue;
      }
      fd = socket(ai->ai_family, SOCK_STREAM, 0);
      if (fd < 0)
      {
         *why = "socket-failed";
         continue;
      }
      /* Bound the blocking connect so a blackholed allowlisted host cannot pin a
       * server connection worker indefinitely. */
      struct timeval ctv = {.tv_sec = 15, .tv_usec = 0};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      {
         if (ipbuf && ipcap)
         {
            const void *a = ai->ai_family == AF_INET
                                ? (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                                : (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
            if (!inet_ntop(ai->ai_family, a, ipbuf, (socklen_t)ipcap))
               ipbuf[0] = '\0';
         }
         *why = NULL;
         break;
      }
      *why = "connect-failed";
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   return fd;
}

/* Bidirectional byte pump, bounded by BOTH a wall-clock deadline and a total byte cap
 * so a compromised delegate cannot pin a worker or push unbounded data through an
 * (opaque, TLS) tunnel to exfiltrate/DoS. Returns bytes each way via the out params. */
#define PROXY_PUMP_DEADLINE_SEC 600
#define PROXY_PUMP_MAX_BYTES    (2UL * 1024 * 1024 * 1024)
static void proxy_pump(int a, int b, unsigned long *a2b, unsigned long *b2a)
{
   struct pollfd pf[2];
   pf[0].fd = a;
   pf[1].fd = b;
   char buf[65536];
   time_t deadline = time(NULL) + PROXY_PUMP_DEADLINE_SEC;
   unsigned long total = 0;
   for (;;)
   {
      if (time(NULL) >= deadline)
         return; /* wall-clock cap: applies even to a stalled/idle tunnel */
      pf[0].events = pf[1].events = POLLIN;
      pf[0].revents = pf[1].revents = 0;
      int pr = poll(pf, 2, 30000); /* short poll so the deadline is re-checked */
      if (pr < 0)
         return;
      if (pr == 0)
         continue; /* idle tick — loop re-checks the wall-clock deadline */
      for (int i = 0; i < 2; i++)
      {
         if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
         {
            int src = i == 0 ? a : b, dst = i == 0 ? b : a;
            ssize_t n = read(src, buf, sizeof(buf));
            if (n <= 0)
               return;
            if (write_all(dst, buf, (size_t)n) != 0)
               return;
            total += (unsigned long)n;
            if (total > PROXY_PUMP_MAX_BYTES)
               return; /* byte cap */
            if (i == 0 && a2b)
               *a2b += (unsigned long)n;
            else if (i == 1 && b2a)
               *b2a += (unsigned long)n;
         }
      }
   }
}

int sandbox_pkg_proxy_serve(int client_fd, int is_uds, const char *head, const char *allowlist,
                            const char *tag)
{
   /* Hard refusal on any non-UDS caller: the proxy must never be reachable from the
    * public TCP/TLS listener, independent of the caller's own listener check. */
   if (!is_uds)
   {
      aimee_log(LOG_ERROR, "sandbox-proxy",
                "refusing proxy request on a non-UDS socket (would expose egress on the "
                "public listener)");
      return 0;
   }
   if (!tag)
      tag = "sandbox";
   if (!head)
      return 0;

   char host[256];
   int port = 0;
   int allowed = 0;
   char *forward_head = NULL;
   sbx_req_kind_t kind = SBX_REQ_INVALID;
   if (proxy_request_policy(head, allowlist, &kind, host, sizeof(host), &port, &allowed,
                            &forward_head) != 0)
   {
      aimee_log(LOG_WARN, "sandbox-proxy", "%s: DENY reason=policy-unavailable", tag);
      (void)write_all(client_fd, "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }

   if (kind == SBX_REQ_INVALID || kind == SBX_REQ_API)
   {
      free(forward_head);
      (void)write_all(client_fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }
   if (!allowed)
   {
      free(forward_head);
      aimee_log(LOG_WARN, "sandbox-proxy", "%s: DENY host=%s port=%d (allowlist/port)", tag, host,
                port);
      (void)write_all(client_fd, "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }

   char ip[64] = "";
   const char *why = NULL;
   int up = proxy_dial(host, port, ip, sizeof(ip), &why);
   if (up < 0)
   {
      free(forward_head);
      aimee_log(LOG_WARN, "sandbox-proxy", "%s: DENY host=%s port=%d reason=%s", tag, host, port,
                why ? why : "?");
      (void)write_all(client_fd, "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }

   unsigned long up_bytes = 0, down_bytes = 0;
   if (kind == SBX_REQ_CONNECT)
   {
      /* sizeof-1, not a hand-counted length: the literal is 39 bytes and a wrong
       * count drops the final CRLF, so the client never sees the \r\n\r\n header
       * terminator, never starts its TLS handshake, and the tunnel hangs. */
      static const char connect_ok[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
      if (write_all(client_fd, connect_ok, sizeof(connect_ok) - 1) == 0)
         proxy_pump(client_fd, up, &up_bytes, &down_bytes);
   }
   else /* SBX_REQ_ABSOLUTE */
   {
      if (forward_head && write_all(up, forward_head, strlen(forward_head)) == 0)
         proxy_pump(client_fd, up, &up_bytes, &down_bytes);
   }
   free(forward_head);
   close(up);
   aimee_log(LOG_INFO, "sandbox-proxy", "%s: OK host=%s port=%d ip=%s up=%lu down=%lu kind=%s", tag,
             host, port, ip[0] ? ip : "?", up_bytes, down_bytes,
             kind == SBX_REQ_CONNECT ? "connect" : "http");
   return 0;
}
