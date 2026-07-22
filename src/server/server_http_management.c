#include "server_http_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int server_http_management_health_route(const char *method, const char *path)
{
   return method && path &&
          ((!strcmp(method, "POST") && !strcmp(path, "/v1/management/challenge")) ||
           (!strcmp(method, "GET") && !strcmp(path, "/v1/management/health")));
}

server_http_management_auth_t server_http_management_auth(const char *method, const char *path,
                                                          int management_lane, int verified_peer,
                                                          int management_profile,
                                                          const char *peer_cn)
{
   int route = server_http_management_health_route(method, path);
   int profile = verified_peer && management_profile;
   if (management_lane && route)
      return profile && peer_cn && strcmp(peer_cn, "p5-kb-management") == 0
                 ? SERVER_HTTP_MANAGEMENT_ALLOW
                 : SERVER_HTTP_MANAGEMENT_DENY;
   return management_lane || route || profile ? SERVER_HTTP_MANAGEMENT_DENY
                                              : SERVER_HTTP_MANAGEMENT_NOT_APPLICABLE;
}

static const char *g_management_start_error;

void server_http_management_set_error(const char *error)
{
   g_management_start_error = error;
}

const char *server_http_management_last_error(void)
{
   return g_management_start_error ? g_management_start_error : "management listener startup";
}

static int management_token(const char *value, size_t max)
{
   size_t n = value ? strnlen(value, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= 'a' && value[i] <= 'z') ||
            (value[i] >= '0' && value[i] <= '9') || value[i] == '.' || value[i] == '_' ||
            value[i] == '-'))
         return 0;
   return 1;
}

static int management_lower_hex(const char *value, size_t length)
{
   if (!value || strnlen(value, length + 1) != length)
      return 0;
   for (size_t i = 0; i < length; i++)
      if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
         return 0;
   return 1;
}

static int management_absolute_path(const char *path)
{
   size_t length = path ? strnlen(path, SERVER_HTTP_MGMT_PATH_MAX) : 0;
   if (length < 2 || length >= SERVER_HTTP_MGMT_PATH_MAX || path[0] != '/')
      return 0;
   const char *part = path + 1;
   while (*part)
   {
      const char *slash = strchr(part, '/');
      size_t n = slash ? (size_t)(slash - part) : strlen(part);
      if (!n || n > 255 || (n == 1 && part[0] == '.') ||
          (n == 2 && part[0] == '.' && part[1] == '.'))
         return 0;
      if (!slash)
         return 1;
      part = slash + 1;
   }
   return 0;
}

int server_http_management_bind_addr(const char *text, uint32_t *out)
{
   struct in_addr addr;
   if (out)
      *out = 0;
   if (!text || !out || strnlen(text, 16) >= 16 || inet_pton(AF_INET, text, &addr) != 1)
      return -1;
   /* inet_ntop supplies the one canonical spelling accepted by the environment
    * packet (no octal, leading-zero, hostname, or alternate textual address). */
   char canonical[INET_ADDRSTRLEN];
   if (!inet_ntop(AF_INET, &addr, canonical, sizeof(canonical)) || strcmp(canonical, text))
      return -1;
   uint32_t host = ntohl(addr.s_addr);
   if (host == INADDR_ANY || host == INADDR_BROADCAST || (host & 0xf0000000U) == 0xe0000000U ||
       (host & 0xffff0000U) == 0xa9fe0000U)
      return -1;
   *out = addr.s_addr;
   return 0;
}

int server_http_management_config_from_env(server_http_management_config_t *out)
{
   static const char *const core_names[] = {
       "AIMEE_SERVER_MGMT_PORT",
       "AIMEE_SERVER_MGMT_TLS_CERT",
       "AIMEE_SERVER_MGMT_TLS_KEY",
       "AIMEE_SERVER_MGMT_CLIENT_CA",
   };
   g_management_start_error = NULL;
   if (!out)
   {
      g_management_start_error = "management configuration output";
      return -1;
   }
   memset(out, 0, sizeof(*out));
   size_t present = 0;
   for (size_t i = 0; i < sizeof(core_names) / sizeof(core_names[0]); i++)
   {
      const char *value = getenv(core_names[i]);
      if (value && value[0])
         present++;
      else if (value) /* an explicitly empty packet member is malformed */
      {
         g_management_start_error = core_names[i];
         present = sizeof(core_names) / sizeof(core_names[0]) + 1;
      }
   }
   if (!present)
   {
      if (getenv("AIMEE_SERVER_MGMT_BIND"))
      {
         g_management_start_error = "AIMEE_SERVER_MGMT_BIND";
         return -1;
      }
      return 0;
   }
   if (present != sizeof(core_names) / sizeof(core_names[0]))
   {
      if (!g_management_start_error)
         for (size_t i = 0; i < sizeof(core_names) / sizeof(core_names[0]); i++)
            if (!getenv(core_names[i]) || !getenv(core_names[i])[0])
            {
               g_management_start_error = core_names[i];
               break;
            }
      return -1;
   }

   const char *bind_config = getenv("AIMEE_SERVER_MGMT_BIND");
   const char *bind_text = bind_config ? bind_config : "127.0.0.1";
   const char *port_text = getenv(core_names[0]);
   const char *cert = getenv(core_names[1]);
   const char *key = getenv(core_names[2]);
   const char *client_ca = getenv(core_names[3]);
   char *end = NULL;
   errno = 0;
   unsigned long port =
       port_text && port_text[0] >= '1' && port_text[0] <= '9' ? strtoul(port_text, &end, 10) : 0;
   const char *server_id = getenv("AIMEE_SERVER_ID");
   const char *status_key_id = getenv("AIMEE_MGMT_STATUS_KEY_ID");
   const char *status_public = getenv("AIMEE_MGMT_STATUS_PUBLIC_KEY");
   if (server_http_management_bind_addr(bind_text, &out->bind_addr))
      g_management_start_error = "AIMEE_SERVER_MGMT_BIND";
   else if (errno || !end || *end || port < 1 || port > UINT16_MAX)
      g_management_start_error = "AIMEE_SERVER_MGMT_PORT";
   else if (!management_absolute_path(cert))
      g_management_start_error = "AIMEE_SERVER_MGMT_TLS_CERT";
   else if (!management_absolute_path(key))
      g_management_start_error = "AIMEE_SERVER_MGMT_TLS_KEY";
   else if (!management_absolute_path(client_ca))
      g_management_start_error = "AIMEE_SERVER_MGMT_CLIENT_CA";
   else if (!management_token(server_id, 127))
      g_management_start_error = "AIMEE_SERVER_ID";
   else if (!management_token(status_key_id, 64))
      g_management_start_error = "AIMEE_MGMT_STATUS_KEY_ID";
   else if (!management_lower_hex(status_public, 64))
      g_management_start_error = "AIMEE_MGMT_STATUS_PUBLIC_KEY";
   if (g_management_start_error)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   out->enabled = 1;
   out->port = (int)port;
   snprintf(out->bind, sizeof(out->bind), "%s", bind_text);
   snprintf(out->cert, sizeof(out->cert), "%s", cert);
   snprintf(out->key, sizeof(out->key), "%s", key);
   snprintf(out->client_ca, sizeof(out->client_ca), "%s", client_ca);
   return 0;
}

static const char *management_find_bytes(const char *begin, const char *end, const char *needle,
                                         size_t needle_len)
{
   if (!begin || !end || end < begin || needle_len > (size_t)(end - begin))
      return NULL;
   for (const char *p = begin; p + needle_len <= end; p++)
      if (!memcmp(p, needle, needle_len))
         return p;
   return NULL;
}

/* Validate the request grammar and ambiguity-sensitive framing before looking
 * at the peer identity. Exact management routes apply the narrower canonical
 * packet check below as a second step. */
int server_http_management_request_syntax_valid(const char *method, const char *path,
                                                const char *request, size_t request_len)
{
   if (!method || !path || !request || !request_len || request_len >= SHTTP_READ_MAX ||
       memchr(request, '\0', request_len))
      return 0;
   const char *finish = request + request_len;
   const char *line_end = management_find_bytes(request, finish, "\r\n", 2);
   const char *headers_end = management_find_bytes(request, finish, "\r\n\r\n", 4);
   if (!line_end || !headers_end || headers_end + 4 != finish)
      return 0;
   size_t method_len = strlen(method), path_len = strlen(path);
   size_t expected_len = method_len + 1 + path_len + sizeof(" HTTP/1.1") - 1;
   if (expected_len != (size_t)(line_end - request) || memcmp(request, method, method_len) ||
       request[method_len] != ' ' || memcmp(request + method_len + 1, path, path_len) ||
       memcmp(request + method_len + 1 + path_len, " HTTP/1.1", sizeof(" HTTP/1.1") - 1))
      return 0;

   int content_length_count = 0;
   const char *line = line_end + 2;
   while (line < headers_end)
   {
      const char *eol = management_find_bytes(line, headers_end + 2, "\r\n", 2);
      const char *colon = memchr(line, ':', (size_t)(headers_end - line));
      if (!eol || eol > headers_end || !colon || colon >= eol || colon == line ||
          colon[-1] == ' ' || colon[-1] == '\t' || line[0] == ' ' || line[0] == '\t')
         return 0;
      size_t name_len = (size_t)(colon - line);
      const char *value = colon + 1;
      while (value < eol && (*value == ' ' || *value == '\t'))
         value++;
      const char *value_end = eol;
      while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
         value_end--;
      if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0)
      {
         if (++content_length_count != 1 || value_end - value != 1 || value[0] != '0')
            return 0;
      }
      else if ((name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) ||
               (name_len == 6 && strncasecmp(line, "Expect", 6) == 0) ||
               (name_len == 7 && strncasecmp(line, "Upgrade", 7) == 0))
         return 0;
      line = eol + 2;
   }
   return line == headers_end + 2;
}

int server_http_management_framing_valid(const char *method, const char *path, const char *request,
                                         size_t request_len)
{
   if (!server_http_management_health_route(method, path) || !request || !request_len ||
       !server_http_management_request_syntax_valid(method, path, request, request_len))
      return 0;
   const char *finish = request + request_len;
   const char *line_end = management_find_bytes(request, finish, "\r\n", 2);
   const char *headers_end = management_find_bytes(request, finish, "\r\n\r\n", 4);
   if (!line_end || !headers_end || headers_end + 4 != finish)
      return 0;
   size_t method_len = strlen(method), path_len = strlen(path);
   size_t expected_len = method_len + 1 + path_len + sizeof(" HTTP/1.1") - 1;
   if (expected_len != (size_t)(line_end - request) || memcmp(request, method, method_len) ||
       request[method_len] != ' ' || memcmp(request + method_len + 1, path, path_len) ||
       memcmp(request + method_len + 1 + path_len, " HTTP/1.1", sizeof(" HTTP/1.1") - 1))
      return 0;

   int content_length_count = 0, host_count = 0, content_type_count = 0;
   int connection_count = 0, status_count = 0;
   const char *line = line_end + 2;
   while (line < headers_end)
   {
      const char *eol = management_find_bytes(line, headers_end + 2, "\r\n", 2);
      const char *colon = memchr(line, ':', (size_t)(headers_end - line));
      if (!eol || eol > headers_end || !colon || colon >= eol || colon == line ||
          colon[-1] == ' ' || colon[-1] == '\t' || line[0] == ' ' || line[0] == '\t')
         return 0;
      size_t name_len = (size_t)(colon - line);
      const char *value = colon + 1;
      while (value < eol && (*value == ' ' || *value == '\t'))
         value++;
      const char *value_end = eol;
      while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
         value_end--;
      if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0)
      {
         content_length_count++;
         if (content_length_count != 1 || value_end - value != 1 || value[0] != '0')
            return 0;
      }
      else if (name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
         return 0;
      else if ((name_len == 6 && strncasecmp(line, "Expect", 6) == 0) ||
               (name_len == 7 && strncasecmp(line, "Upgrade", 7) == 0))
         return 0;
      else if (name_len == 4 && strncasecmp(line, "Host", 4) == 0)
      {
         if (++host_count != 1 || value == value_end)
            return 0;
      }
      else if (name_len == 12 && strncasecmp(line, "Content-Type", 12) == 0)
      {
         static const char expected[] = "application/json";
         if (++content_type_count != 1 || value_end - value != (ptrdiff_t)sizeof(expected) - 1 ||
             strncasecmp(value, expected, sizeof(expected) - 1) != 0)
            return 0;
      }
      else if (name_len == 10 && strncasecmp(line, "Connection", 10) == 0)
      {
         static const char expected[] = "keep-alive";
         if (++connection_count != 1 || value_end - value != (ptrdiff_t)sizeof(expected) - 1 ||
             strncasecmp(value, expected, sizeof(expected) - 1) != 0)
            return 0;
      }
      else if (name_len == 25 && strncasecmp(line, "X-Aimee-Management-Status", 25) == 0)
      {
         if (++status_count != 1 || value == value_end ||
             strcmp(path, "/v1/management/health") != 0)
            return 0;
      }
      line = eol + 2;
   }
   int health = strcmp(path, "/v1/management/health") == 0;
   return line == headers_end + 2 && content_length_count == 1 && host_count == 1 &&
          content_type_count == 1 && connection_count == 1 && status_count == health;
}
