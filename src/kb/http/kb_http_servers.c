#include "kb_http_servers.h"
#include "../../db2/server_registry.h"
#include "kb_mgmt_endpoint.h"
#include "kb_reqctx.h"
#include "cJSON.h"
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

/* Preserve the pre-B3b list-route behavior: the first value wins, duplicate
 * values are ignored, and the copied value is truncated to the legacy buffer. */
static int q_list_legacy(const char *s, const char *k, char *o, size_t n)
{
   if (!s || !k || !o || n == 0)
      return 0;
   size_t key_len = strlen(k);
   while (*s)
   {
      if (*s == '&')
         s++;
      const char *end = strchr(s, '&');
      size_t segment_len = end ? (size_t)(end - s) : strlen(s);
      if (segment_len > key_len && !memcmp(s, k, key_len) && s[key_len] == '=')
      {
         snprintf(o, n, "%s", s + key_len + 1);
         char *amp = strchr(o, '&');
         if (amp)
            *amp = 0;
         return 1;
      }
      if (!end)
         break;
      s = end + 1;
   }
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

int kb_http_servers_route(const char *m, const char *p, const char *qs, char *out, int cap)
{
   if (!m || !p || !out || cap <= 0)
      return -1;
   if (!strcmp(p, "/v1/servers"))
   {
      /* list route below */
   }
   else if (!strncmp(p, "/v1/servers/", 12))
   {
      const char *id = p + 12, *slash = strstr(id, "/health");
      char sid[128];
      if (!slash || slash[7] || slash == id || (size_t)(slash - id) >= sizeof(sid))
         return -1;
      if (strcmp(m, "GET"))
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
      int team_query = q_unique(qs, "team", t, sizeof(t));
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
      return health_dispatch(actor, team, sid, out, cap);
   }
   else
      return -1;
   if (strcmp(m, "GET"))
      return 405;
   char t[32];
   if (!q_list_legacy(qs, "team", t, sizeof(t)))
   {
      snprintf(out, cap, "{\"error\":\"team is required\"}");
      return 400;
   }
   char *e;
   long long team = strtoll(t, &e, 10);
   if (!*t || *e || team <= 0)
   {
      snprintf(out, cap, "{\"error\":\"invalid team\"}");
      return 400;
   }
   db2_server_row_t rows[64];
   int n = db2_server_registry_list(team, rows, 64);
   if (n < 0)
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
