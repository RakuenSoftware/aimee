#include "kb_http_servers.h"
#include "../../modules/db2/c/server_registry.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_mgmt_endpoint.h"
#include "kb_reqctx.h"
#include "cJSON.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t health_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t health_cv = PTHREAD_COND_INITIALIZER;
static kb_http_servers_health_handler_fn health_handler;
static void *health_ctx;
static unsigned int health_inflight;
static int health_unregistering;
static pthread_mutex_t action_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t action_cv = PTHREAD_COND_INITIALIZER;
static kb_http_servers_action_handler_fn action_handler;
static void *action_ctx;
static unsigned action_inflight;
static int action_unregistering;
static pthread_mutex_t read_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cv = PTHREAD_COND_INITIALIZER;
static kb_http_servers_read_handler_fn read_handler;
static void *read_ctx;
static unsigned read_inflight;
static int read_unregistering;

static int read_correlation(char out[44])
{
   static const char alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   unsigned char random[32];
   memset(random, 0, sizeof(random));
   if (RAND_bytes(random, sizeof(random)) != 1)
   {
      OPENSSL_cleanse(random, sizeof(random));
      out[0] = 0;
      return -1;
   }
   uint32_t accumulator = 0;
   unsigned bits = 0;
   size_t n = 0;
   for (size_t i = 0; i < sizeof(random); ++i)
   {
      accumulator = (accumulator << 8) | random[i];
      bits += 8;
      while (bits >= 6)
      {
         bits -= 6;
         out[n++] = alphabet[(accumulator >> bits) & 63];
         accumulator &= bits ? (1U << bits) - 1U : 0U;
      }
   }
   out[n++] = alphabet[(accumulator << 4) & 63];
   out[n] = 0;
   OPENSSL_cleanse(random, sizeof(random));
   return 0;
}

int kb_http_servers_read_register(kb_http_servers_read_handler_fn handler, void *ctx)
{
   if (!handler)
      return -1;
   pthread_mutex_lock(&read_mu);
   int rc = !read_handler && !read_unregistering && !read_inflight ? 0 : -1;
   if (!rc)
      read_handler = handler, read_ctx = ctx;
   pthread_mutex_unlock(&read_mu);
   return rc;
}

int kb_http_servers_read_unregister(kb_http_servers_read_handler_fn handler, void *ctx)
{
   pthread_mutex_lock(&read_mu);
   if (!handler || read_handler != handler || read_ctx != ctx || read_unregistering)
   {
      pthread_mutex_unlock(&read_mu);
      return -1;
   }
   read_unregistering = 1;
   read_handler = NULL;
   read_ctx = NULL;
   while (read_inflight)
      pthread_cond_wait(&read_cv, &read_mu);
   read_unregistering = 0;
   pthread_cond_broadcast(&read_cv);
   pthread_mutex_unlock(&read_mu);
   return 0;
}

int kb_http_servers_action_register(kb_http_servers_action_handler_fn handler, void *ctx)
{
   int rc = -1;
   if (!handler)
      return -1;
   pthread_mutex_lock(&action_mu);
   if (!action_handler && !action_unregistering && action_inflight == 0)
   {
      action_handler = handler;
      action_ctx = ctx;
      rc = 0;
   }
   pthread_mutex_unlock(&action_mu);
   return rc;
}

int kb_http_servers_action_unregister(kb_http_servers_action_handler_fn handler, void *ctx)
{
   if (!handler)
      return -1;
   pthread_mutex_lock(&action_mu);
   if (action_handler != handler || action_ctx != ctx || action_unregistering)
   {
      pthread_mutex_unlock(&action_mu);
      return -1;
   }
   action_unregistering = 1;
   action_handler = NULL;
   action_ctx = NULL;
   while (action_inflight)
      pthread_cond_wait(&action_cv, &action_mu);
   action_unregistering = 0;
   pthread_cond_broadcast(&action_cv);
   pthread_mutex_unlock(&action_mu);
   return 0;
}

int kb_http_servers_health_register(kb_http_servers_health_handler_fn handler, void *ctx)
{
   int rc = -1;
   if (!handler)
      return -1;
   pthread_mutex_lock(&health_mu);
   if (!health_handler && !health_unregistering && health_inflight == 0)
   {
      health_handler = handler;
      health_ctx = ctx;
      rc = 0;
   }
   pthread_mutex_unlock(&health_mu);
   return rc;
}

int kb_http_servers_health_unregister(kb_http_servers_health_handler_fn handler, void *ctx)
{
   if (!handler)
      return -1;
   pthread_mutex_lock(&health_mu);
   if (health_handler != handler || health_ctx != ctx || health_unregistering)
   {
      pthread_mutex_unlock(&health_mu);
      return -1;
   }
   health_unregistering = 1;
   health_handler = NULL;
   health_ctx = NULL;
   while (health_inflight != 0)
      pthread_cond_wait(&health_cv, &health_mu);
   health_unregistering = 0;
   pthread_cond_broadcast(&health_cv);
   pthread_mutex_unlock(&health_mu);
   return 0;
}

static int health_result_status(kb_management_health_result_t rc)
{
   switch (rc)
   {
   case KB_MANAGEMENT_HEALTH_OK:
      return 200;
   case KB_MANAGEMENT_HEALTH_NOT_FOUND:
      return 404;
   case KB_MANAGEMENT_HEALTH_DENIED:
      return 403;
   case KB_MANAGEMENT_HEALTH_CONFLICT:
      return 409;
   case KB_MANAGEMENT_HEALTH_UNAVAILABLE:
      return 503;
   case KB_MANAGEMENT_HEALTH_INTEGRITY:
      return 502;
   case KB_MANAGEMENT_HEALTH_INVALID:
      return 400;
   }
   return 503;
}

static const char *health_result_error(kb_management_health_result_t rc)
{
   switch (rc)
   {
   case KB_MANAGEMENT_HEALTH_NOT_FOUND:
      return "server not found";
   case KB_MANAGEMENT_HEALTH_DENIED:
      return "server health denied";
   case KB_MANAGEMENT_HEALTH_CONFLICT:
      return "server changed during health check";
   case KB_MANAGEMENT_HEALTH_INTEGRITY:
      return "server health verification failed";
   case KB_MANAGEMENT_HEALTH_INVALID:
      return "invalid server health request";
   case KB_MANAGEMENT_HEALTH_OK:
   case KB_MANAGEMENT_HEALTH_UNAVAILABLE:
   default:
      return "server health unavailable";
   }
}

static int health_emit_success(const char *server_id, char *out, int cap)
{
   cJSON *root = cJSON_CreateObject();
   if (!root || !cJSON_AddStringToObject(root, "server_id", server_id) ||
       !cJSON_AddStringToObject(root, "status", "ok"))
   {
      cJSON_Delete(root);
      snprintf(out, (size_t)cap, "{\"error\":\"out of memory\"}");
      return 503;
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json || strlen(json) >= (size_t)cap)
   {
      free(json);
      snprintf(out, (size_t)cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out, (size_t)cap, "%s", json);
   free(json);
   return 200;
}

static int health_dispatch(const kb_principal_t *actor, int64_t team_id, const char *server_id,
                           char *out, int cap)
{
   kb_http_servers_health_handler_fn handler;
   void *ctx;

   pthread_mutex_lock(&health_mu);
   handler = health_handler;
   ctx = health_ctx;
   if (handler)
      health_inflight++;
   pthread_mutex_unlock(&health_mu);

   if (!handler)
   {
      snprintf(out, (size_t)cap, "{\"error\":\"server health unavailable\"}");
      return 503;
   }

   kb_management_health_result_t rc = handler(ctx, actor, team_id, server_id);

   pthread_mutex_lock(&health_mu);
   health_inflight--;
   if (health_inflight == 0)
      pthread_cond_broadcast(&health_cv);
   pthread_mutex_unlock(&health_mu);

   if (rc == KB_MANAGEMENT_HEALTH_OK)
      return health_emit_success(server_id, out, cap);
   int status = health_result_status(rc);
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", health_result_error(rc));
   return status;
}

static int q_unique(const char *s, const char *k, char *o, size_t n)
{
   if (!s || !k || !o || n == 0)
      return 0;
   size_t l = strlen(k);
   int found = 0;
   while (*s)
   {
      if (*s == '&')
         s++;
      const char *end = strchr(s, '&');
      size_t segment_len = end ? (size_t)(end - s) : strlen(s);
      if (segment_len > l && !memcmp(s, k, l) && s[l] == '=')
      {
         const char *value = s + l + 1;
         size_t value_len = segment_len - l - 1;
         if (found || value_len >= n)
            return -1;
         memcpy(o, value, value_len);
         o[value_len] = 0;
         found = 1;
      }
      if (!end)
         break;
      s = end + 1;
   }
   return found;
}

static int q_exact_team(const char *s, char *out, size_t cap)
{
   if (!s || strncmp(s, "team=", 5) || strchr(s, '&') || strchr(s, '%') || strchr(s, ';') ||
       strchr(s, '#') || strchr(s, '?'))
      return -1;
   size_t n = strlen(s + 5);
   if (!n || n >= cap)
      return -1;
   memcpy(out, s + 5, n + 1);
   return 0;
}

static int positive_team(const char *text, int64_t *team_out)
{
   if (!text || !team_out || text[0] < '1' || text[0] > '9')
      return -1;
   for (const char *p = text + 1; *p; p++)
      if (*p < '0' || *p > '9')
         return -1;
   char *end = NULL;
   errno = 0;
   long long value = strtoll(text, &end, 10);
   if (errno == ERANGE || !end || *end || value <= 0)
      return -1;
   *team_out = (int64_t)value;
   return 0;
}

static int server_id_valid(const char *text)
{
   size_t n = text ? strnlen(text, 128) : 0;
   if (!n || n > 127)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z') ||
            (text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == '_' ||
            text[i] == '-'))
         return 0;
   return 1;
}

static int action_status(kb_management_action_result_t rc)
{
   switch (rc)
   {
   case KB_MANAGEMENT_ACTION_OK:
      return 200;
   case KB_MANAGEMENT_ACTION_NOT_FOUND:
      return 404;
   case KB_MANAGEMENT_ACTION_DENIED:
      return 403;
   case KB_MANAGEMENT_ACTION_CONFLICT:
      return 409;
   case KB_MANAGEMENT_ACTION_INTEGRITY:
   case KB_MANAGEMENT_ACTION_INDETERMINATE:
      return 502;
   case KB_MANAGEMENT_ACTION_INVALID:
      return 400;
   case KB_MANAGEMENT_ACTION_UNAVAILABLE:
      return 503;
   }
   return 503;
}

static int action_dispatch(const kb_principal_t *actor, int64_t team, const char *server,
                           const char *body, size_t body_len, char *out, int cap)
{
   pthread_mutex_lock(&action_mu);
   kb_http_servers_action_handler_fn handler = action_handler;
   void *ctx = action_ctx;
   if (handler)
      action_inflight++;
   pthread_mutex_unlock(&action_mu);
   if (!handler)
   {
      snprintf(out, (size_t)cap, "{\"error\":\"server action unavailable\"}");
      return 503;
   }
   kb_management_action_result_t rc = handler(ctx, actor, team, server, body, body_len);
   pthread_mutex_lock(&action_mu);
   action_inflight--;
   if (!action_inflight)
      pthread_cond_broadcast(&action_cv);
   pthread_mutex_unlock(&action_mu);
   int status = action_status(rc);
   snprintf(out, (size_t)cap,
            rc == KB_MANAGEMENT_ACTION_OK ? "{\"result\":\"succeeded\"}"
                                          : "{\"error\":\"server action failed\"}");
   return status;
}

static int read_dispatch(const kb_principal_t *actor, int64_t team, const char *server, char *out,
                         int cap, server_mgmt_read_selector_t selector)
{
   char correlation[44];
   memset(out, 0, (size_t)cap);
   int have_correlation = !read_correlation(correlation);
   pthread_mutex_lock(&read_mu);
   kb_http_servers_read_handler_fn handler = read_handler;
   void *ctx = read_ctx;
   if (handler)
      read_inflight++;
   pthread_mutex_unlock(&read_mu);
   if (!handler)
   {
      if (have_correlation)
         snprintf(out, (size_t)cap,
                  "{\"error\":{\"code\":\"unavailable\",\"message\":\"Service unavailable.\","
                  "\"correlation_id\":\"%s\"}}",
                  correlation);
      else
         snprintf(out, (size_t)cap,
                  "{\"error\":{\"code\":\"unavailable\",\"message\":\"Service unavailable.\"}}");
      return 503;
   }
   kb_management_read_result_t rc = handler(ctx, actor, team, server, selector, out, (size_t)cap);
   pthread_mutex_lock(&read_mu);
   if (!--read_inflight)
      pthread_cond_broadcast(&read_cv);
   pthread_mutex_unlock(&read_mu);
   if (rc == KB_MANAGEMENT_READ_OK)
      return 200;
   memset(out, 0, (size_t)cap);
   int status = rc == KB_MANAGEMENT_READ_INVALID     ? 400
                : rc == KB_MANAGEMENT_READ_DENIED    ? 403
                : rc == KB_MANAGEMENT_READ_NOT_FOUND ? 404
                : rc == KB_MANAGEMENT_READ_CONFLICT  ? 409
                : rc == KB_MANAGEMENT_READ_INTEGRITY ? 502
                                                     : 503;
   const char *code = status == 400   ? "invalid_request"
                      : status == 403 ? "forbidden"
                      : status == 404 ? "not_found"
                      : status == 409 ? "conflict"
                      : status == 502 ? "integrity"
                                      : "unavailable";
   const char *message = status == 400   ? "Invalid request."
                         : status == 403 ? "Forbidden."
                         : status == 404 ? "Not found."
                         : status == 409 ? "Request conflict."
                         : status == 502 ? "Integrity verification failed."
                                         : "Service unavailable.";
   if (have_correlation)
      snprintf(out, (size_t)cap,
               "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"correlation_id\":\"%s\"}}", code,
               message, correlation);
   else
      snprintf(out, (size_t)cap, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
   return status;
}

int kb_http_servers_route_ex(const char *m, const char *p, const char *qs, const char *body,
                             size_t body_len, char *out, int cap)
{
   if (!m || !p || !out || cap <= 0)
      return -1;
   if (!strcmp(p, "/v1/servers"))
   {
      /* list route below */
   }
   else if (!strncmp(p, "/v1/servers/", 12))
   {
      const char *id = p + 12, *slash = strchr(id, '/');
      char sid[128];
      if (!slash || slash == id || (size_t)(slash - id) >= sizeof(sid))
         return -1;
      int is_health = !strcmp(slash, "/health");
      int is_action = !strcmp(slash, "/actions");
      int is_agents = !strcmp(slash, "/agents");
      int is_config = !strcmp(slash, "/config");
      int is_read = is_agents || is_config;
      if (!is_health && !is_action && !is_read)
         return -1;
      if (((is_health || is_read) && strcmp(m, "GET")) || (is_action && strcmp(m, "POST")))
         return 405;
      memcpy(sid, id, (size_t)(slash - id));
      sid[slash - id] = 0;
      if (!server_id_valid(sid))
      {
         snprintf(out, (size_t)cap, "{\"error\":\"invalid server id\"}");
         return 400;
      }
      char t[32];
      int64_t team;
      int team_query = (is_action || is_read) ? (q_exact_team(qs, t, sizeof(t)) ? -1 : 1)
                                              : q_unique(qs, "team", t, sizeof(t));
      if (team_query == 0)
      {
         snprintf(out, cap, "{\"error\":\"team is required\"}");
         return 400;
      }
      if (team_query < 0)
      {
         snprintf(out, cap, "{\"error\":\"invalid team\"}");
         return 400;
      }
      if (positive_team(t, &team) != 0)
      {
         snprintf(out, cap, "{\"error\":\"invalid team\"}");
         return 400;
      }
      const kb_principal_t *actor = kb_reqctx_actor();
      if (!actor || !actor->authenticated)
      {
         snprintf(out, (size_t)cap, "{\"error\":\"authentication required\"}");
         return 401;
      }
      if (is_action)
      {
         kb_management_action_body_t parsed;
         if (!body || kb_management_action_body_parse(body, body_len, &parsed))
         {
            snprintf(out, (size_t)cap, "{\"error\":\"invalid action\"}");
            return 400;
         }
         OPENSSL_cleanse(&parsed, sizeof(parsed));
         return action_dispatch(actor, team, sid, body, body_len, out, cap);
      }
      if (is_read)
      {
         if ((body && body_len) || body_len)
            return 400;
         return read_dispatch(actor, team, sid, out, cap,
                              is_agents ? SERVER_MGMT_READ_SELECTOR_AGENTS
                                        : SERVER_MGMT_READ_SELECTOR_CONFIG);
      }
      return health_dispatch(actor, team, sid, out, cap);
   }
   else
      return -1;
   if (strcmp(m, "GET"))
      return 405;
   char t[32];
   if (q_exact_team(qs, t, sizeof(t)) != 0)
   {
      snprintf(out, cap, "{\"error\":\"invalid team\"}");
      return 400;
   }
   int64_t team;
   if (positive_team(t, &team) != 0)
   {
      snprintf(out, cap, "{\"error\":\"invalid team\"}");
      return 400;
   }
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor || !actor->authenticated)
   {
      snprintf(out, (size_t)cap, "{\"error\":\"authentication required\"}");
      return 401;
   }
   int scope_rc = db2_tenant_scope_begin(actor, team);
   if (scope_rc != 0)
   {
      snprintf(out, (size_t)cap,
               scope_rc == DB2_ERR_TENANT_DENIED ? "{\"error\":\"team access denied\"}"
                                                 : "{\"error\":\"tenant scope unavailable\"}");
      return scope_rc == DB2_ERR_TENANT_DENIED ? 403 : 503;
   }
   db2_server_row_t rows[64];
   int n = db2_server_registry_list(team, rows, 64);
   if (n < 0)
   {
      db2_tenant_scope_rollback();
      snprintf(out, cap, "{\"error\":\"registry unavailable\"}");
      return 503;
   }
   if (db2_tenant_scope_commit() != 0)
   {
      snprintf(out, cap, "{\"error\":\"registry unavailable\"}");
      return 503;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *servers = cJSON_AddArrayToObject(root, "servers");
   if (!root || !servers)
   {
      cJSON_Delete(root);
      snprintf(out, cap, "{\"error\":\"out of memory\"}");
      return 503;
   }
   for (int i = 0; i < n; i++)
   {
      if (kb_mgmt_endpoint_validate(rows[i].endpoint) != 0)
      {
         cJSON_Delete(root);
         snprintf(out, cap, "{\"error\":\"unsafe registered endpoint\"}");
         return 500;
      }
      cJSON *r = cJSON_CreateObject();
      if (!r)
      {
         cJSON_Delete(root);
         return 503;
      }
      cJSON_AddStringToObject(r, "server_id", rows[i].server_id);
      cJSON_AddStringToObject(r, "cert_cn", rows[i].cert_cn);
      cJSON_AddStringToObject(r, "mgmt_cert_cn", rows[i].mgmt_cert_cn);
      cJSON_AddStringToObject(r, "endpoint", rows[i].endpoint);
      cJSON_AddStringToObject(r, "status", rows[i].status);
      cJSON_AddStringToObject(r, "health", rows[i].health);
      cJSON_AddStringToObject(r, "version", rows[i].version);
      cJSON_AddItemToArray(servers, r);
   }
   char *json = cJSON_PrintUnformatted(root);
   int rc = (json && strlen(json) < (size_t)cap) ? 200 : 413;
   if (rc == 200)
      snprintf(out, cap, "%s", json);
   else
      snprintf(out, cap, "{\"error\":\"response too large\"}");
   free(json);
   cJSON_Delete(root);
   return rc;
}

int kb_http_servers_route(const char *m, const char *p, const char *qs, char *out, int cap)
{
   return kb_http_servers_route_ex(m, p, qs, NULL, 0, out, cap);
}
