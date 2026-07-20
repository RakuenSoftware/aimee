#include "kb_http_servers.h"
#include "../../db2/server_registry.h"
#include "../kb_mgmt_endpoint.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int q(const char *s, const char *k, char *o, size_t n)
{
   if (!s)
      return 0;
   size_t l = strlen(k);
   for (; *s; s = strchr(s, '&'))
   {
      if (*s == '&')
         s++;
      if (!strncmp(s, k, l) && s[l] == '=')
      {
         snprintf(o, n, "%s", s + l + 1);
         char *x = strchr(o, '&');
         if (x)
            *x = 0;
         return 1;
      }
   }
   return 0;
}

int kb_http_servers_route(const char *m, const char *p, const char *qs, char *out, int cap)
{
   if (!strcmp(p, "/v1/servers")) {
      /* list route below */
   } else if (!strncmp(p, "/v1/servers/", 12) && !strcmp(m, "GET")) {
      const char *id = p + 12, *slash = strstr(id, "/health");
      char sid[128];
      if (!slash || slash[7] || slash == id || (size_t)(slash-id) >= sizeof(sid)) return -1;
      memcpy(sid, id, (size_t)(slash-id)); sid[slash-id] = 0;
      char t[32], *e; long long team;
      if (!q(qs, "team", t, sizeof(t))) { snprintf(out,cap,"{\"error\":\"team is required\"}"); return 400; }
      team = strtoll(t,&e,10); if (!*t || *e || team <= 0) { snprintf(out,cap,"{\"error\":\"invalid team\"}"); return 400; }
      db2_server_row_t row;
      if (db2_server_registry_get(team,sid,&row) != 0) { snprintf(out,cap,"{\"error\":\"server not found\"}"); return 404; }
      snprintf(out,cap,"{\"server_id\":\"%s\",\"status\":\"%s\",\"health\":\"%s\",\"version\":\"%s\"}",row.server_id,row.status,row.health,row.version);
      return 200;
   } else return -1;
   if (strcmp(m, "GET"))
      return 405;
   char t[32];
   if (!q(qs, "team", t, sizeof(t)))
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
