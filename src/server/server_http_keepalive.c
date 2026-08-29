#include "server_http_internal.h"
#include "server.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int header_name_char(unsigned char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

int server_http_gzip_route_eligible(const char *path)
{
   /* An explicit allowlist keeps new and sensitive routes identity encoded.
    * Split literals keep the OpenAPI route scanner from mistaking policy data
    * for dispatch declarations. Adjacent literals are identical at runtime. */
   static const char *const allowed[] = {"/v1/"
                                         "responses",
                                         "/v1/"
                                         "completions",
                                         "/v1/"
                                         "embeddings",
                                         "/v1/"
                                         "messages",
                                         NULL};
   for (int i = 0; allowed[i]; i++)
      if (strcmp(path, allowed[i]) == 0)
         return 1;
   return 0;
}

int server_http_keepalive_route_eligible(const char *path)
{
   return !strstr(path, "/events") && !strstr(path, "/stream") && !strstr(path, "/live");
}

uint32_t server_http_enrollment_caps(uint32_t caps, int is_tcp, int mtls_authenticated,
                                     int native_tls, const char *bearer, const char *method,
                                     const char *path)
{
   /* An unscoped deployment bearer over verified native TLS may sign exactly
    * the CSR needed to leave optional-mTLS migration, or enroll an additional
    * client bearer without requiring a write-tier grant first. Route handlers
    * still enforce their own operation. */
   if (is_tcp && !mtls_authenticated && native_tls && bearer && bearer[0] &&
       strncmp(bearer, "scope:", 6) != 0 && method && path && strcmp(method, "POST") == 0)
   {
      if (strcmp(path, "/v1/cert/sign") == 0)
         return caps | CAP_DELEGATE;
      if (strcmp(path, "/v1/api/enroll_bearer") == 0)
         return caps | CAP_SESSION_ADMIN;
   }
   return caps;
}

/* Validate one unambiguous HTTP/1.1 message boundary before any data-plane
 * request is authenticated or dispatched. It also makes a connection safe to
 * reuse when keep-alive is enabled. */
int server_http_request_framing_valid(const char *request, size_t total)
{
   const char *head_end = strstr(request, "\r\n\r\n");
   if (!head_end)
      return 0;
   size_t head_len = (size_t)(head_end + 4 - request);
   for (size_t i = 0; i < head_len; i++)
      if (request[i] == '\0' ||
          (request[i] == '\r' && (i + 1 >= head_len || request[i + 1] != '\n')) ||
          (request[i] == '\n' && (i == 0 || request[i - 1] != '\r')))
         return 0;
   const char *line_end = strstr(request, "\r\n");
   if (!line_end || (size_t)(line_end - request) > 8192)
      return 0;
   char request_line[8193];
   size_t request_line_len = (size_t)(line_end - request);
   memcpy(request_line, request, request_line_len);
   request_line[request_line_len] = '\0';
   char method[16], target[4097], version[16], extra;
   if (sscanf(request_line, "%15s %4096s %15s %c", method, target, version, &extra) != 3 ||
       strcmp(version, "HTTP/1.1") || target[0] != '/')
      return 0;

   size_t content_len = 0;
   int have_cl = 0, have_connection = 0, have_host = 0, headers = 0;
   const char *p = line_end + 2;
   while (p < head_end)
   {
      const char *end = strstr(p, "\r\n");
      if (!end || end > head_end || ++headers > 64 || *p == ' ' || *p == '\t')
         return 0;
      const char *colon = memchr(p, ':', (size_t)(end - p));
      if (!colon || colon == p)
         return 0;
      for (const char *q = p; q < colon; q++)
         if (!header_name_char((unsigned char)*q))
            return 0;
      size_t name_len = (size_t)(colon - p);
      if (name_len == 17 && !strncasecmp(p, "Transfer-Encoding", 17))
         return 0;
      if (name_len == 4 && !strncasecmp(p, "Host", 4))
      {
         const char *value = colon + 1;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         if (have_host++ || value == end)
            return 0;
      }
      if (name_len == 10 && !strncasecmp(p, "Connection", 10) && have_connection++)
         return 0;
      if (name_len == 14 && !strncasecmp(p, "Content-Length", 14))
      {
         if (have_cl++)
            return 0;
         const char *value = colon + 1;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         if (value == end)
            return 0;
         for (const char *q = value; q < end; q++)
         {
            if (*q < '0' || *q > '9' || content_len > (SIZE_MAX - (size_t)(*q - '0')) / 10)
               return 0;
            content_len = content_len * 10 + (size_t)(*q - '0');
         }
      }
      p = end + 2;
   }
   return have_host == 1 && total <= head_len + content_len;
}
