#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wfe_http_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int wfe_http_proxy_request(const char *method, const char *path, const char *query,
                           const char *body, int body_len, const char *principal, char *resp,
                           int resp_cap)
{
   (void)method;
   (void)path;
   (void)query;
   (void)body;
   (void)body_len;
   (void)principal;
   if (resp && resp_cap > 0)
      snprintf(resp, (size_t)resp_cap,
               "{\"error\":\"Go WFE control plane is unavailable on this platform\"}");
   return 503;
}

#else

#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define WFE_PROXY_TIMEOUT_SECS 30
#define WFE_PROXY_MAX_RESPONSE (16u * 1024u * 1024u)

static int proxy_error(char *resp, int cap, int status, const char *message)
{
   if (resp && cap > 0)
      snprintf(resp, (size_t)cap, "{\"error\":\"%s\"}", message);
   return status;
}

static int write_all(int fd, const char *data, size_t size)
{
   while (size > 0)
   {
      ssize_t n = write(fd, data, size);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      data += n;
      size -= (size_t)n;
   }
   return 0;
}

static char *decode_chunked(const char *body)
{
   size_t cap = strlen(body) + 1, used = 0;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   const char *p = body;
   for (;;)
   {
      char *end = NULL;
      unsigned long chunk = strtoul(p, &end, 16);
      if (!end || end == p || end[0] != '\r' || end[1] != '\n')
      {
         free(out);
         return NULL;
      }
      p = end + 2;
      if (chunk == 0)
         break;
      if (chunk > strlen(p) || used + chunk >= cap)
      {
         free(out);
         return NULL;
      }
      memcpy(out + used, p, chunk);
      used += chunk;
      p += chunk;
      if (p[0] != '\r' || p[1] != '\n')
      {
         free(out);
         return NULL;
      }
      p += 2;
   }
   out[used] = '\0';
   return out;
}

int wfe_http_proxy_request(const char *method, const char *path, const char *query,
                           const char *body, int body_len, const char *principal, char *resp,
                           int resp_cap)
{
   if (!method || !path || !resp || resp_cap <= 0 || body_len < 0 || strchr(method, '\r') ||
       strchr(method, '\n') || strchr(path, '\r') || strchr(path, '\n') ||
       (query && (strchr(query, '\r') || strchr(query, '\n'))) ||
       (principal && (strchr(principal, '\r') || strchr(principal, '\n'))))
      return proxy_error(resp, resp_cap, 400, "invalid workflow proxy request");
   if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0 &&
       strcmp(method, "DELETE") != 0)
      return proxy_error(resp, resp_cap, 405, "unsupported workflow proxy method");
   if (!body)
      body_len = 0;

   const char *socket_path = getenv("AIMEE_WFE_HTTP_SOCKET");
   if (!socket_path || !socket_path[0])
      return proxy_error(resp, resp_cap, 503, "Go WFE control plane is unavailable");

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return proxy_error(resp, resp_cap, 502, "could not create workflow proxy socket");
   struct timeval timeout = {.tv_sec = WFE_PROXY_TIMEOUT_SECS, .tv_usec = 0};
   if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
       setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
   {
      close(fd);
      return proxy_error(resp, resp_cap, 502, "could not configure workflow proxy socket");
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) >=
           (int)sizeof(addr.sun_path) ||
       connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      close(fd);
      return proxy_error(resp, resp_cap, 502, "Go WFE control plane did not accept the request");
   }

   const char *bearer = getenv("AIMEE_API_BEARER_TOKEN");
   if (!bearer)
      bearer = "";
   if (!query)
      query = "";
   if (!principal)
      principal = "";
   size_t head_cap =
       strlen(method) + strlen(path) + strlen(query) + strlen(bearer) + strlen(principal) + 512;
   char *head = malloc(head_cap);
   if (!head)
   {
      close(fd);
      return proxy_error(resp, resp_cap, 500, "out of memory building workflow request");
   }
   int head_len =
       snprintf(head, head_cap,
                "%s %s%s%s HTTP/1.1\r\nHost: aimee\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\n%s%s%s%s%s%sConnection: close\r\n\r\n",
                method, path, query[0] ? "?" : "", query, body_len,
                bearer[0] ? "Authorization: Bearer " : "", bearer, bearer[0] ? "\r\n" : "",
                principal[0] ? "X-Aimee-Webuser: " : "", principal, principal[0] ? "\r\n" : "");
   if (head_len <= 0 || (size_t)head_len >= head_cap ||
       write_all(fd, head, (size_t)head_len) != 0 ||
       (body_len > 0 && write_all(fd, body, (size_t)body_len) != 0))
   {
      free(head);
      close(fd);
      return proxy_error(resp, resp_cap, 502, "sending workflow request failed");
   }
   free(head);

   size_t cap = 8192, used = 0;
   char *raw = malloc(cap);
   if (!raw)
   {
      close(fd);
      return proxy_error(resp, resp_cap, 500, "out of memory reading workflow response");
   }
   for (;;)
   {
      if (used + 4097 > cap)
      {
         if (cap >= WFE_PROXY_MAX_RESPONSE)
         {
            free(raw);
            close(fd);
            return proxy_error(resp, resp_cap, 502, "workflow response is too large");
         }
         size_t next = cap * 2;
         if (next > WFE_PROXY_MAX_RESPONSE)
            next = WFE_PROXY_MAX_RESPONSE;
         char *grown = realloc(raw, next);
         if (!grown)
         {
            free(raw);
            close(fd);
            return proxy_error(resp, resp_cap, 500, "out of memory reading workflow response");
         }
         raw = grown;
         cap = next;
      }
      ssize_t n = read(fd, raw + used, cap - used - 1);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   close(fd);
   raw[used] = '\0';

   int status = 0;
   char *body_at = strstr(raw, "\r\n\r\n");
   if (sscanf(raw, "HTTP/1.%*d %d", &status) != 1 || !body_at)
   {
      free(raw);
      return proxy_error(resp, resp_cap, 502, "Go WFE control plane returned an invalid response");
   }
   body_at += 4;
   char *decoded = strcasestr(raw, "\r\nTransfer-Encoding: chunked") ? decode_chunked(body_at)
                                                                     : strdup(body_at);
   free(raw);
   if (!decoded)
      return proxy_error(resp, resp_cap, 502, "Go WFE control plane returned an invalid body");
   size_t response_len = strlen(decoded);
   if (response_len >= (size_t)resp_cap)
   {
      free(decoded);
      return proxy_error(resp, resp_cap, 502, "workflow response exceeds the public API limit");
   }
   memcpy(resp, decoded, response_len + 1);
   free(decoded);
   return status;
}

#endif
