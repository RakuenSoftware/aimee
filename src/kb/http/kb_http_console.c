/* kb_http_console.c: /v1/console routes for the aimee-kb web console.
 * See kb_http_console.h. Reached only with a console-admin credential that the
 * route ACL (kb_route_acl.c) has already authorized. */
#include "kb_http_console.h"

#include "aimee.h" /* now_utc */
#include "cJSON.h"
#include "config.h"                 /* config_load / config_save (§8 tune) */
#include "kb_service.h"             /* kb_service_workers_json, kb_service_ctx_t */
#include "kb_service_kb.h"          /* kb_service_health_json */
#include "db2/kb_service_backend.h" /* async queue status */
#include "db2/ontology_evolution.h" /* db2_ontology_* (§8 observe + act) */
#include "rel_types.h"              /* REL_TYPE_NAME_MAX */

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

/* GET /v1/console/typed_facts — the Typed Facts panel's observe surface (§8):
 * the KB-owned config knobs plus the provisional-relation promotion review queue
 * (what the §7.2 auto-promote sweep will act on, and what an operator can act on
 * by hand). Read-only. */
static int console_typed_facts(char *out_buf, int out_cap)
{
   config_t cfg;
   config_load(&cfg);
   int thr =
       cfg.kb_typed_facts_promote_threshold > 0 ? cfg.kb_typed_facts_promote_threshold : 3;

   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"typed_facts alloc failed\"}");
      return 500;
   }
   cJSON_AddStringToObject(root, "schema", "console.typed_facts.v1");
   char ts[32];
   now_utc(ts, sizeof(ts));
   cJSON_AddStringToObject(root, "generated_at", ts);

   cJSON *c = cJSON_AddObjectToObject(root, "config");
   cJSON_AddBoolToObject(c, "typed_facts_enabled", cfg.typed_facts_enabled ? 1 : 0);
   cJSON_AddBoolToObject(c, "auto_promote", cfg.kb_typed_facts_auto_promote_enabled ? 1 : 0);
   cJSON_AddNumberToObject(c, "promote_threshold", thr);

   /* Promotion review queue: every pending provisional relation (threshold 1 lists
    * the whole queue), with its observation count and whether it has cleared the
    * auto-promote bar. */
   cJSON *cands = cJSON_AddArrayToObject(root, "promotion_candidates");
   char names[32][REL_TYPE_NAME_MAX];
   int nc = db2_ontology_eval_candidates(1, names, 32);
   for (int i = 0; i < nc && cands; i++)
   {
      cJSON *o = cJSON_CreateObject();
      if (!o)
         continue;
      cJSON_AddStringToObject(o, "relation", names[i]);
      long cnt = db2_ontology_eval_count(names[i]);
      cJSON_AddNumberToObject(o, "observations", cnt < 0 ? 0 : (double)cnt);
      cJSON_AddBoolToObject(o, "ready", (cnt >= thr) ? 1 : 0);
      char st[32] = "";
      db2_ontology_eval_status(names[i], st, sizeof(st));
      cJSON_AddStringToObject(o, "status", st);
      cJSON_AddItemToArray(cands, o);
   }
   cJSON_AddNumberToObject(root, "candidate_count", nc < 0 ? 0 : nc);

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"typed_facts render failed\"}");
      return 500;
   }
   if (strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"typed_facts too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return 200;
}

/* POST /v1/console/typed_facts/config — fine-tune / alter behaviour (§8):
 * {auto_promote?: bool, promote_threshold?: int}. Persists to KB config so the
 * drain picks it up on its next poll. */
static int console_typed_facts_config(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   if (!req || !cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid request body\"}");
      return 400;
   }
   config_t cfg;
   config_load(&cfg);
   /* KB-owned master enable/disable for the whole typed-facts layer. */
   const cJSON *en = cJSON_GetObjectItemCaseSensitive(req, "enabled");
   if (en && cJSON_IsBool(en))
      cfg.typed_facts_enabled = cJSON_IsTrue(en) ? 1 : 0;
   const cJSON *ap = cJSON_GetObjectItemCaseSensitive(req, "auto_promote");
   if (ap && cJSON_IsBool(ap))
      cfg.kb_typed_facts_auto_promote_enabled = cJSON_IsTrue(ap) ? 1 : 0;
   const cJSON *pt = cJSON_GetObjectItemCaseSensitive(req, "promote_threshold");
   if (pt && cJSON_IsNumber(pt) && pt->valueint > 0)
      cfg.kb_typed_facts_promote_threshold = pt->valueint;
   cJSON_Delete(req);
   if (config_save(&cfg) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"config save failed\"}");
      return 500;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddBoolToObject(resp, "enabled", cfg.typed_facts_enabled ? 1 : 0);
   cJSON_AddBoolToObject(resp, "auto_promote", cfg.kb_typed_facts_auto_promote_enabled ? 1 : 0);
   cJSON_AddNumberToObject(resp, "promote_threshold", cfg.kb_typed_facts_promote_threshold);
   char *s = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "{\"ok\":true}");
      return 200;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return 200;
}

/* A relation name is safe iff it is a non-empty lower snake_case token within
 * REL_TYPE_NAME_MAX (the ontology's canonical form). Rejects oversized/malformed
 * input at the route boundary before it reaches the ontology helpers. */
static int tf_relation_name_ok(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t n = strlen(s);
   if (n >= REL_TYPE_NAME_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
         return 0;
   }
   return 1;
}

/* POST /v1/console/typed_facts/relation — operator action on a provisional
 * relation (§8): {action: "approve"|"map"|"reject", relation, target?}. Wires to
 * the shipped ontology-evolution verbs. */
static int console_typed_facts_relation(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   const char *rel =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "relation")) : NULL;
   const char *target =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "target")) : NULL;
   if (!action || !rel || !rel[0])
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action and relation are required\"}");
      return 400;
   }
   if (!tf_relation_name_ok(rel) || (target && target[0] && !tf_relation_name_ok(target)))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"relation/target must be lower snake_case within REL_TYPE_NAME_MAX\"}");
      return 400;
   }
   int rc;
   const char *did;
   if (strcmp(action, "approve") == 0)
   {
      rc = db2_ontology_approve(rel);
      did = "approved";
   }
   else if (strcmp(action, "reject") == 0)
   {
      rc = db2_ontology_reject(rel);
      did = "rejected";
   }
   else if (strcmp(action, "map") == 0)
   {
      if (!target || !target[0])
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"map action requires a target\"}");
         return 400;
      }
      rc = db2_ontology_map(rel, target);
      did = "mapped";
   }
   else
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action must be approve, map, or reject\"}");
      return 400;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", rc == 0 ? 1 : 0);
   cJSON_AddStringToObject(resp, "action", did);
   cJSON_AddStringToObject(resp, "relation", rel);
   cJSON_Delete(req);
   char *s = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   int status = rc == 0 ? 200 : 500;
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
      return status;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return status;
}

/* Reject a non-matching method for a matched route with a 405. */
static int console_method_not_allowed(char *out_buf, int out_cap)
{
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
   return 405;
}

int kb_http_console_route(const char *method, const char *path, const char *body, char *out_buf,
                          int out_cap)
{
   if (route_is(path, "/v1/console/overview"))
      return strcmp(method, "GET") == 0 ? console_overview(out_buf, out_cap)
                                        : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts"))
      return strcmp(method, "GET") == 0 ? console_typed_facts(out_buf, out_cap)
                                        : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/config"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_config(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/relation"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_relation(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   return -1; /* not a console route — caller continues dispatch */
}
