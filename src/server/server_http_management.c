#include "server_http_internal.h"
#include "server_mgmt_endpoint.h"
#include "kb_mgmt_endpoint.h"
#include "kb_mgmt_status.h"
#include "server_runtime_identity.h"
#include "runtime_secret.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static pthread_mutex_t g_management_action_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_management_action_idle = PTHREAD_COND_INITIALIZER;
static unsigned g_management_action_active;
/* Direct route unit tests run without a listener. A real listener start resets
 * this state, while stop closes the gate before it tears down any dependency. */
static int g_management_action_stopping;
extern int g_remote_writes;

int server_http_remote_writes(void)
{
   return g_remote_writes;
}

int server_http_management_action_begin(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   int ok = !g_management_action_stopping;
   if (ok)
      g_management_action_active++;
   pthread_mutex_unlock(&g_management_action_lock);
   return ok ? 0 : -1;
}

int server_http_management_action_allowed(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   int allowed = !g_management_action_stopping;
   pthread_mutex_unlock(&g_management_action_lock);
   return allowed;
}

void server_http_management_action_end(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   if (g_management_action_active)
      g_management_action_active--;
   if (!g_management_action_active)
      pthread_cond_broadcast(&g_management_action_idle);
   pthread_mutex_unlock(&g_management_action_lock);
}

void server_http_management_actions_start(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   g_management_action_stopping = 0;
   pthread_mutex_unlock(&g_management_action_lock);
}

void server_http_management_actions_shutdown_begin(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   g_management_action_stopping = 1;
   pthread_mutex_unlock(&g_management_action_lock);
}

void server_http_management_actions_stop_and_wait(void)
{
   pthread_mutex_lock(&g_management_action_lock);
   g_management_action_stopping = 1;
   while (g_management_action_active)
      pthread_cond_wait(&g_management_action_idle, &g_management_action_lock);
   pthread_mutex_unlock(&g_management_action_lock);
}

int server_http_management_health_route(const char *method, const char *path)
{
   return method && path &&
          ((!strcmp(method, "POST") && !strcmp(path, "/v1/management/challenge")) ||
           (!strcmp(method, "GET") && !strcmp(path, "/v1/management/health")));
}

int server_http_management_action_route(const char *method, const char *path)
{
   return method && path && !strcmp(method, "POST") &&
          (!strcmp(path, "/v1/management/action/challenge") ||
           !strcmp(path, "/v1/management/action"));
}

int server_http_management_read_route(const char *method, const char *path)
{
   return method && path &&
          ((!strcmp(method, "POST") && !strcmp(path, "/v1/management/read/challenge")) ||
           (!strcmp(method, "POST") && !strcmp(path, "/v1/management/read/config/challenge")) ||
           (!strcmp(method, "GET") && (!strcmp(path, "/v1/management/read/agents") ||
                                       !strcmp(path, "/v1/management/read/config"))));
}

int server_http_management_route(const char *method, const char *path)
{
   return server_http_management_health_route(method, path) ||
          server_http_management_action_route(method, path) ||
          server_http_management_read_route(method, path);
}

server_http_management_auth_t server_http_management_auth(const char *method, const char *path,
                                                          int management_lane, int verified_peer,
                                                          int management_profile,
                                                          const char *peer_cn)
{
   int route = server_http_management_route(method, path);
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
       "AIMEE_SERVER_MGMT_CLIENT_CA",
   };
   static const char *const checkpoint_names[] = {
       "AIMEE_SERVER_MGMT_STATUS_ENDPOINT",   "AIMEE_SERVER_MGMT_STATUS_CA_FILE",
       "AIMEE_SERVER_MGMT_STATUS_LEAF_PIN",   "AIMEE_SERVER_MGMT_STATUS_CLIENT_CERT",
   };
   static const char mgmt_key_name[] = "AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY";
   static const char checkpoint_key_name[] =
       "AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY";
   g_management_start_error = NULL;
   if (!out)
   {
      g_management_start_error = "management configuration output";
      return -1;
   }
   memset(out, 0, sizeof(*out));
   if (getenv("AIMEE_SERVER_MGMT_TLS_KEY") || getenv("AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY"))
   {
      g_management_start_error = "management private-key file variables are forbidden";
      return -1;
   }
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
   if (runtime_secret_has(mgmt_key_name))
      present++;
   if (!present)
   {
      if (getenv("AIMEE_SERVER_MGMT_BIND"))
      {
         g_management_start_error = "AIMEE_SERVER_MGMT_BIND";
         return -1;
      }
      return 0;
   }
   if (present != sizeof(core_names) / sizeof(core_names[0]) + 1)
   {
      if (!g_management_start_error)
         for (size_t i = 0; i < sizeof(core_names) / sizeof(core_names[0]); i++)
            if (!getenv(core_names[i]) || !getenv(core_names[i])[0])
            {
               g_management_start_error = core_names[i];
               break;
            }
      if (!g_management_start_error && !runtime_secret_has(mgmt_key_name))
         g_management_start_error = mgmt_key_name;
      return -1;
   }

   const char *bind_config = getenv("AIMEE_SERVER_MGMT_BIND");
   const char *bind_text = bind_config ? bind_config : "127.0.0.1";
   const char *port_text = getenv(core_names[0]);
   const char *cert = getenv(core_names[1]);
   const char *client_ca = getenv(core_names[2]);
   char *end = NULL;
   errno = 0;
   unsigned long port =
       port_text && port_text[0] >= '1' && port_text[0] <= '9' ? strtoul(port_text, &end, 10) : 0;
   char server_id_buf[128];
   const char *server_id =
       server_runtime_server_id_load(server_id_buf, sizeof(server_id_buf)) ? server_id_buf : NULL;
   const char *status_key_id = getenv("AIMEE_MGMT_STATUS_KEY_ID");
   const char *status_public = getenv("AIMEE_MGMT_STATUS_PUBLIC_KEY");
   const char *token_issuer = getenv("AIMEE_SERVER_MGMT_ISSUER");
   const char *trust_bundle = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   size_t checkpoint_present = 0;
   for (size_t i = 0; i < sizeof(checkpoint_names) / sizeof(checkpoint_names[0]); i++)
      if (getenv(checkpoint_names[i]) && getenv(checkpoint_names[i])[0])
         checkpoint_present++;
   if (runtime_secret_has(checkpoint_key_name))
      checkpoint_present++;
   const char *checkpoint_endpoint = getenv(checkpoint_names[0]);
   const char *checkpoint_ca = getenv(checkpoint_names[1]);
   const char *checkpoint_pin = getenv(checkpoint_names[2]);
   const char *checkpoint_cert = getenv(checkpoint_names[3]);
   const char *checkpoint_secondary = getenv("AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN");
   if (server_http_management_bind_addr(bind_text, &out->bind_addr))
      g_management_start_error = "AIMEE_SERVER_MGMT_BIND";
   else if (errno || !end || *end || port < 1 || port > UINT16_MAX)
      g_management_start_error = "AIMEE_SERVER_MGMT_PORT";
   else if (!management_absolute_path(cert))
      g_management_start_error = "AIMEE_SERVER_MGMT_TLS_CERT";
   else if (!management_absolute_path(client_ca))
      g_management_start_error = "AIMEE_SERVER_MGMT_CLIENT_CA";
   else if (!management_token(server_id, 127))
      g_management_start_error = "AIMEE_SERVER_ID";
   else if (!management_token(status_key_id, 64))
      g_management_start_error = "AIMEE_MGMT_STATUS_KEY_ID";
   else if (!management_lower_hex(status_public, 64))
      g_management_start_error = "AIMEE_MGMT_STATUS_PUBLIC_KEY";
   else if (!token_issuer || strncmp(token_issuer, "https://", 8) || !token_issuer[8])
      g_management_start_error = "AIMEE_SERVER_MGMT_ISSUER";
   else if (!management_absolute_path(trust_bundle))
      g_management_start_error = "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE";
   else if (checkpoint_present != sizeof(checkpoint_names) / sizeof(checkpoint_names[0]) + 1)
      g_management_start_error = "management checkpoint packet";
   else if (kb_mgmt_endpoint_validate(checkpoint_endpoint) != 0)
      g_management_start_error = checkpoint_names[0];
   else if (!management_absolute_path(checkpoint_ca))
      g_management_start_error = checkpoint_names[1];
   else if (!management_lower_hex(checkpoint_pin, 64))
      g_management_start_error = checkpoint_names[2];
   else if (!management_absolute_path(checkpoint_cert))
      g_management_start_error = checkpoint_names[3];
   else if (checkpoint_secondary && !management_lower_hex(checkpoint_secondary, 64))
      g_management_start_error = "AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN";
   if (g_management_start_error)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   out->enabled = 1;
   out->port = (int)port;
   snprintf(out->bind, sizeof(out->bind), "%s", bind_text);
   snprintf(out->cert, sizeof(out->cert), "%s", cert);
   snprintf(out->key, sizeof(out->key), "%s", mgmt_key_name);
   snprintf(out->client_ca, sizeof(out->client_ca), "%s", client_ca);
   snprintf(out->status_endpoint, sizeof(out->status_endpoint), "%s", checkpoint_endpoint);
   snprintf(out->status_ca, sizeof(out->status_ca), "%s", checkpoint_ca);
   snprintf(out->status_leaf_pin, sizeof(out->status_leaf_pin), "%s", checkpoint_pin);
   snprintf(out->status_secondary_leaf_pin, sizeof(out->status_secondary_leaf_pin), "%s",
            checkpoint_secondary ? checkpoint_secondary : "");
   snprintf(out->status_client_cert, sizeof(out->status_client_cert), "%s", checkpoint_cert);
   snprintf(out->status_client_key, sizeof(out->status_client_key), "%s", checkpoint_key_name);
   snprintf(out->status_key_id, sizeof(out->status_key_id), "%s", status_key_id);
   snprintf(out->status_public_key, sizeof(out->status_public_key), "%s", status_public);
   return 0;
}

int server_http_management_checkpoint_files_valid(const server_http_management_config_t *c)
{
   if (!c || !c->enabled)
      return c ? 1 : 0;
   const char *paths[] = {c->status_ca, c->status_client_cert};
   for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
   {
      struct stat st;
      if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
          (st.st_mode & (S_IWGRP | S_IWOTH)))
         return 0;
   }
   return 1;
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
         if (++content_length_count != 1 || value == value_end)
            return 0;
         size_t digits = (size_t)(value_end - value);
         if (digits > 3 || (digits > 1 && value[0] == '0'))
            return 0;
         for (size_t i = 0; i < digits; i++)
            if (value[i] < '0' || value[i] > '9')
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

int server_http_management_action_framing_valid(const char *method, const char *path,
                                                const char *request, size_t request_len)
{
   if (!server_http_management_action_route(method, path) || !request || !request_len ||
       !server_http_management_request_syntax_valid(method, path, request, request_len))
      return 0;
   const char *end = management_find_bytes(request, request + request_len, "\r\n\r\n", 4);
   const char *line = management_find_bytes(request, request + request_len, "\r\n", 2);
   if (!end || !line)
      return 0;
   line += 2;
   int host = 0, ct = 0, cl = 0, conn = 0, auth = 0, staple = 0;
   size_t body_len = 0;
   while (line < end)
   {
      const char *eol = management_find_bytes(line, end + 2, "\r\n", 2);
      const char *colon = eol ? memchr(line, ':', (size_t)(eol - line)) : NULL;
      if (!eol || !colon || colon == line)
         return 0;
      const char *v = colon + 1;
      while (v < eol && (*v == ' ' || *v == '\t'))
         v++;
      const char *ve = eol;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
         ve--;
      size_t n = (size_t)(colon - line), vn = (size_t)(ve - v);
      if (n == 4 && !strncasecmp(line, "Host", n))
         host = ++host == 1 && vn > 0 ? host : 99;
      else if (n == 12 && !strncasecmp(line, "Content-Type", n))
         ct = ++ct == 1 && vn == 16 && !strncasecmp(v, "application/json", 16) ? ct : 99;
      else if (n == 14 && !strncasecmp(line, "Content-Length", n))
      {
         cl++;
         body_len = 0;
         for (size_t i = 0; i < vn; i++)
         {
            if (v[i] < '0' || v[i] > '9' || body_len > (SIZE_MAX - (size_t)(v[i] - '0')) / 10)
               return 0;
            body_len = body_len * 10 + (size_t)(v[i] - '0');
         }
         if (!vn)
            return 0;
      }
      else if (n == 10 && !strncasecmp(line, "Connection", n))
      {
         int challenge = !strcmp(path, "/v1/management/action/challenge");
         int exact = challenge ? vn == 10 && !strncasecmp(v, "keep-alive", 10)
                               : vn == 5 && !strncasecmp(v, "close", 5);
         conn = ++conn == 1 && exact ? conn : 99;
      }
      else if (n == 13 && !strncasecmp(line, "Authorization", n))
         auth = ++auth == 1 && vn > 7 && vn <= 4096 && !strncasecmp(v, "Bearer ", 7) ? auth : 99;
      else if (n == 25 && !strncasecmp(line, "X-Aimee-Management-Status", n))
         staple = ++staple == 1 && vn > 0 && vn <= KB_MGMT_STATUS_JSON_MAX ? staple : 99;
      else if ((n == 17 && !strncasecmp(line, "Transfer-Encoding", n)) ||
               (n == 6 && !strncasecmp(line, "Expect", n)) ||
               (n == 7 && !strncasecmp(line, "Upgrade", n)) ||
               (n >= 6 && !strncasecmp(line, "Proxy-", 6)) ||
               (n == 9 && !strncasecmp(line, "Forwarded", n)) ||
               (n >= 12 && !strncasecmp(line, "X-Forwarded-", 12)) ||
               (n == 16 && !strncasecmp(line, "Content-Encoding", n)) ||
               (n == 7 && !strncasecmp(line, "Trailer", n)))
         return 0;
      line = eol + 2;
   }
   int challenge = !strcmp(path, "/v1/management/action/challenge");
   return host == 1 && ct == 1 && cl == 1 && conn == 1 &&
          (challenge ? body_len == 0 && auth == 0 && staple == 0
                     : body_len > 0 && body_len <= SERVER_MGMT_ACTION_BODY_MAX && auth == 1 &&
                           staple == 1);
}

int server_http_management_read_framing_valid(const char *method, const char *path,
                                              const char *request, size_t request_len)
{
   if (!server_http_management_read_route(method, path) || !request || !request_len ||
       !server_http_management_request_syntax_valid(method, path, request, request_len))
      return 0;
   const char *end = management_find_bytes(request, request + request_len, "\r\n\r\n", 4);
   const char *line = management_find_bytes(request, request + request_len, "\r\n", 2);
   if (!end || !line)
      return 0;
   line += 2;
   int host = 0, ct = 0, cl = 0, conn = 0, auth = 0, staple = 0;
   size_t body_len = 0;
   while (line < end)
   {
      const char *eol = management_find_bytes(line, end + 2, "\r\n", 2);
      const char *colon = eol ? memchr(line, ':', (size_t)(eol - line)) : NULL;
      if (!eol || !colon || colon == line)
         return 0;
      const char *v = colon + 1;
      while (v < eol && (*v == ' ' || *v == '\t'))
         v++;
      const char *ve = eol;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
         ve--;
      size_t n = (size_t)(colon - line), vn = (size_t)(ve - v);
      if (n == 4 && !strncasecmp(line, "Host", n))
         host = ++host == 1 && vn > 0 ? host : 99;
      else if (n == 12 && !strncasecmp(line, "Content-Type", n))
         ct = ++ct == 1 && vn == 16 && !strncasecmp(v, "application/json", 16) ? ct : 99;
      else if (n == 14 && !strncasecmp(line, "Content-Length", n))
      {
         cl++;
         body_len = 0;
         for (size_t i = 0; i < vn; ++i)
         {
            if (v[i] < '0' || v[i] > '9' || body_len > (SIZE_MAX - (size_t)(v[i] - '0')) / 10)
               return 0;
            body_len = body_len * 10 + (size_t)(v[i] - '0');
         }
         if (!vn)
            return 0;
      }
      else if (n == 10 && !strncasecmp(line, "Connection", n))
      {
         int challenge = !strcmp(path, "/v1/management/read/challenge") ||
                         !strcmp(path, "/v1/management/read/config/challenge");
         int exact = challenge ? vn == 10 && !strncasecmp(v, "keep-alive", 10)
                               : vn == 5 && !strncasecmp(v, "close", 5);
         conn = ++conn == 1 && exact ? conn : 99;
      }
      else if (n == 13 && !strncasecmp(line, "Authorization", n))
         auth = ++auth == 1 && vn > 7 && vn <= 4096 && !strncasecmp(v, "Bearer ", 7) ? auth : 99;
      else if (n == 25 && !strncasecmp(line, "X-Aimee-Management-Status", n))
         staple = ++staple == 1 && vn > 0 && vn <= KB_MGMT_STATUS_JSON_MAX ? staple : 99;
      else if ((n == 17 && !strncasecmp(line, "Transfer-Encoding", n)) ||
               (n == 6 && !strncasecmp(line, "Expect", n)) ||
               (n == 7 && !strncasecmp(line, "Upgrade", n)) ||
               (n >= 6 && !strncasecmp(line, "Proxy-", 6)) ||
               (n == 9 && !strncasecmp(line, "Forwarded", n)) ||
               (n >= 12 && !strncasecmp(line, "X-Forwarded-", 12)) ||
               (n == 16 && !strncasecmp(line, "Content-Encoding", n)) ||
               (n == 7 && !strncasecmp(line, "Trailer", n)))
         return 0;
      line = eol + 2;
   }
   int challenge = !strcmp(path, "/v1/management/read/challenge") ||
                   !strcmp(path, "/v1/management/read/config/challenge");
   return host == 1 && cl == 1 && body_len == 0 && conn == 1 &&
          (challenge ? ct == 1 && auth == 0 && staple == 0 : ct == 0 && auth == 1 && staple == 1);
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
