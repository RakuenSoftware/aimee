/* kb_http_console.c: /v1/console routes for the aimee-kb web console.
 * See kb_http_console.h. Reached only with a console-admin credential that the
 * route ACL (kb_route_acl.c) has already authorized. */
#include "kb_http_console.h"

#include "aimee.h" /* now_utc */
#include "cJSON.h"
#include "kb_service.h"             /* kb_service_workers_json, kb_service_ctx_t */
#include "kb_service_kb.h"          /* kb_service_health_json */
#include "db2/kb_service_backend.h" /* async queue status */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern kb_service_ctx_t *g_kb_ctx;

/* Compare path to route, tolerating a single trailing slash (matching the route
 * ACL's normalization so the ACL and the handler agree on which path is which). */
static int route_is(const char *path, const char *route)
{
   size_t n = strlen(path);
   if (n > 1 && path[n - 1] == '/')
      n--;
   return strlen(route) == n && strncmp(path, route, n) == 0;
}

/* Attach a component {name, ok, data|error} to the overview array. `json` is an
 * owned JSON string (parsed + freed here) or NULL for an error component. */
static void add_component(cJSON *arr, const char *name, char *json, const char *err)
{
   cJSON *c = cJSON_CreateObject();
   cJSON_AddStringToObject(c, "name", name);
   if (json)
   {
      cJSON *data = cJSON_Parse(json);
      free(json);
      if (data)
      {
         cJSON_AddBoolToObject(c, "ok", 1);
         cJSON_AddItemToObject(c, "data", data);
      }
      else
      {
         cJSON_AddBoolToObject(c, "ok", 0);
         cJSON_AddStringToObject(c, "error", "malformed component json");
      }
   }
   else
   {
      cJSON_AddBoolToObject(c, "ok", 0);
      cJSON_AddStringToObject(c, "error", err ? err : "unavailable");
   }
   cJSON_AddItemToArray(arr, c);
}

/* Attach a component whose data is an already-built cJSON object (ownership
 * transferred). Used for components assembled directly, so values are JSON-
 * escaped and never round-trip through a fixed-size printf buffer. */
static void add_component_obj(cJSON *arr, const char *name, cJSON *data)
{
   cJSON *c = cJSON_CreateObject();
   cJSON_AddStringToObject(c, "name", name);
   cJSON_AddBoolToObject(c, "ok", 1);
   cJSON_AddItemToObject(c, "data", data);
   cJSON_AddItemToArray(arr, c);
}

/* GET /v1/console/overview — the dashboard aggregate. Fans in the kb telemetry
 * read models IN-PROCESS (direct backend calls, not HTTP — so the console-admin
 * ACL, which does not allow the underlying telemetry routes, is not self-denied).
 * Each component carries {name, ok, data|error}; the envelope is versioned +
 * timestamped and marks degraded when any component failed. */
static int console_overview(char *out_buf, int out_cap)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"overview alloc failed\"}");
      return 500;
   }
   cJSON_AddStringToObject(root, "schema", "console.overview.v1");
   char ts[32];
   now_utc(ts, sizeof(ts));
   cJSON_AddStringToObject(root, "generated_at", ts);
   cJSON *comps = cJSON_AddArrayToObject(root, "components");

   /* Pipeline (async queue depth) — built directly as cJSON (no printf buffer). */
   db2_kb_service_async_queue_stats_t qs;
   if (db2_kb_service_async_queue_status(&qs) == 0)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddNumberToObject(d, "pending", qs.pending);
      cJSON_AddNumberToObject(d, "running", qs.running);
      cJSON_AddNumberToObject(d, "done", qs.done);
      cJSON_AddNumberToObject(d, "failed", qs.failed);
      cJSON_AddNumberToObject(d, "total", qs.total);
      add_component_obj(comps, "pipeline", d);
   }
   else
      add_component(comps, "pipeline", NULL, "queue unavailable");

   /* Workers — the backend hands back its own JSON string, parsed as-is. */
   if (g_kb_ctx)
      add_component(comps, "workers", kb_service_workers_json(g_kb_ctx), NULL);
   else
      add_component(comps, "workers", NULL, "workers unavailable");

   /* Health. */
   add_component(comps, "health", kb_service_health_json(), NULL);

   /* Version — cJSON escapes AIMEE_VERSION, so an odd version string stays valid. */
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "version", AIMEE_VERSION);
      cJSON_AddStringToObject(d, "service", "aimee-kb");
      add_component_obj(comps, "version", d);
   }

   /* degraded = any component not ok (ok is a real JSON boolean, so cJSON_IsTrue
    * matches — verified against cJSON_AddBoolToObject/cJSON_CreateBool). */
   int degraded = 0;
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, comps)
   {
      if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "ok")))
      {
         degraded = 1;
         break;
      }
   }
   cJSON_AddBoolToObject(root, "degraded", degraded);

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"overview render failed\"}");
      return 500;
   }
   /* Never emit a truncated (invalid-JSON) body with a 200. */
   if (strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"overview too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return 200;
}

int kb_http_console_route(const char *method, const char *path, char *out_buf, int out_cap)
{
   if (!route_is(path, "/v1/console/overview"))
      return -1; /* not a console route — caller continues dispatch */

   if (strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   return console_overview(out_buf, out_cap);
}
