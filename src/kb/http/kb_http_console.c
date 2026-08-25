/* kb_http_console.c: /v1/console routes for the aimee-kb web console.
 * See kb_http_console.h. Reached only with a console-admin credential that the
 * control-web module, reached through the event bus, has already authorized. */
#include "kb_http_console.h"

#include "aimee.h" /* now_utc */
#include "cJSON.h"
#include "config.h"
#include "config_client.h"
#include "kb_curator_drain.h"                 /* kb_curator_stages_json / _presets_json */
#include "kb_service.h"                       /* kb_service_workers_json, kb_service_ctx_t */
#include "kb_reqctx.h"                        /* verifier-derived trace scope */
#include "kb_service_kb.h"                    /* kb_service_health_json */
#include "modules/db2/c/kb_service_backend.h" /* async queue status */
#include "modules/db2/c/ontology_evolution.h" /* db2_ontology_* (§8 observe + act) */
#include "modules/db2/c/fact_mutation.h"      /* assertion review/rollback/removal */
#include "modules/db2/c/memory_query.h"       /* human memory review/restore */
#include "modules/db2/c/memory_scope_query.h" /* operator all-scope review */
#include "modules/db2/c/evidence_lifecycle.h" /* P1-P9 operator evidence surface */
#include "modules/db2/c/entity_registry.h"    /* entity merge/unmerge review */
#include "rel_types.h"                        /* REL_TYPE_NAME_MAX */
#include "runtime_secret.h"
#include <openssl/crypto.h> /* wipe transient credential request copies */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern kb_service_ctx_t *g_kb_ctx;
static int console_send(cJSON *resp, int status, const char *fallback, char *out_buf, int out_cap);

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
   int thr = config_kb_typed_facts_promote_threshold() > 0
                 ? config_kb_typed_facts_promote_threshold()
                 : 3;

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
   cJSON_AddBoolToObject(c, "typed_facts_enabled", 1); /* unconditional; no master gate */
   cJSON_AddBoolToObject(c, "auto_promote", config_kb_typed_facts_auto_promote_enabled() ? 1 : 0);
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

   /* Assertion candidates are quarantined from recall until this queue approves
    * them.  Evidence count is independent of the graph weight. */
   cJSON *assertions = cJSON_AddArrayToObject(root, "assertion_candidates");
   fact_candidate_t fc[64];
   int nfc = db2_fact_candidates(fc, 64);
   for (int i = 0; i < nfc && assertions; i++)
   {
      cJSON *o = cJSON_CreateObject();
      if (!o)
         continue;
      cJSON_AddNumberToObject(o, "id", (double)fc[i].id);
      cJSON_AddStringToObject(o, "subject", fc[i].source);
      cJSON_AddStringToObject(o, "relation", fc[i].relation);
      cJSON_AddStringToObject(o, "object", fc[i].target);
      cJSON_AddStringToObject(o, "assertion_kind", fc[i].assertion_kind);
      cJSON_AddStringToObject(o, "lifecycle", fc[i].lifecycle);
      cJSON_AddNumberToObject(o, "authority_rank", fc[i].authority_rank);
      cJSON_AddNumberToObject(o, "evidence_count", fc[i].evidence_count);
      cJSON_AddStringToObject(o, "commit_id", fc[i].commit_id);
      cJSON_AddItemToArray(assertions, o);
   }
   cJSON_AddNumberToObject(root, "assertion_candidate_count", nfc < 0 ? 0 : nfc);

   /* Canonical entities and merge history share the typed-fact operator surface:
    * merge is a graph mutation with the same commit/rollback/audit contract. */
   cJSON *entities = cJSON_AddArrayToObject(root, "entities");
   entity_summary_t es[128];
   int nes = db2_entity_summaries(es, 128);
   for (int i = 0; i < nes && entities; i++)
   {
      cJSON *o = cJSON_CreateObject();
      if (!o)
         continue;
      cJSON_AddNumberToObject(o, "canonical_id", (double)es[i].canonical_id);
      cJSON_AddNumberToObject(o, "kind", es[i].kind);
      cJSON_AddStringToObject(o, "status", es[i].status);
      cJSON_AddNumberToObject(o, "merged_into", (double)es[i].merged_into);
      cJSON_AddStringToObject(o, "name", es[i].name);
      cJSON_AddItemToArray(entities, o);
   }
   cJSON_AddNumberToObject(root, "entity_count", nes < 0 ? 0 : nes);

   cJSON *merges = cJSON_AddArrayToObject(root, "entity_merges");
   entity_merge_summary_t ms[64];
   int nms = db2_entity_merge_summaries(ms, 64);
   for (int i = 0; i < nms && merges; i++)
   {
      cJSON *o = cJSON_CreateObject();
      if (!o)
         continue;
      cJSON_AddNumberToObject(o, "merge_id", (double)ms[i].merge_id);
      cJSON_AddNumberToObject(o, "from_id", (double)ms[i].from_id);
      cJSON_AddNumberToObject(o, "into_id", (double)ms[i].into_id);
      cJSON_AddBoolToObject(o, "undone", ms[i].undone ? 1 : 0);
      cJSON_AddStringToObject(o, "from_name", ms[i].from_name);
      cJSON_AddStringToObject(o, "into_name", ms[i].into_name);
      cJSON_AddStringToObject(o, "commit_id", ms[i].commit_id);
      cJSON_AddItemToArray(merges, o);
   }
   cJSON_AddNumberToObject(root, "entity_merge_count", nms < 0 ? 0 : nms);

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

/* Memory rows are reviewable independently of recall.  This history surface is
 * deliberately operator-all-scope; the surrounding console route has already
 * authenticated a console administrator, and ordinary user recall remains
 * constrained by row RLS and the canonical scope filter. */
static int console_memories(char *out_buf, int out_cap)
{
   db2_memory_review_row_t rows[32];
   db2_memory_scope_context_set("", "", 1);
   int n = db2_memory_review_list("", 32, rows, 32);
   db2_memory_scope_context_clear();
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"memory review unavailable\"}");
      return 500;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *items = root ? cJSON_AddArrayToObject(root, "memories") : NULL;
   if (!root || !items)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"memory review alloc failed\"}");
      return 500;
   }
   cJSON_AddStringToObject(root, "schema", "console.memories.v1");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", (double)rows[i].id);
      cJSON_AddStringToObject(o, "tier", rows[i].tier);
      cJSON_AddStringToObject(o, "kind", rows[i].kind);
      cJSON_AddStringToObject(o, "key", rows[i].key);
      cJSON_AddStringToObject(o, "content", rows[i].content);
      cJSON_AddNumberToObject(o, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(o, "lifecycle", rows[i].lifecycle_state);
      cJSON_AddStringToObject(o, "review_reason", rows[i].review_reason);
      cJSON_AddStringToObject(o, "scope_type", rows[i].scope_type);
      cJSON_AddStringToObject(o, "scope_value", rows[i].scope_value);
      cJSON_AddStringToObject(o, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(o, "updated_at", rows[i].updated_at);
      cJSON_AddItemToArray(items, o);
   }
   cJSON_AddNumberToObject(root, "count", n);
   return console_send(root, 200, "{\"schema\":\"console.memories.v1\",\"memories\":[]}", out_buf,
                       out_cap);
}

static int console_memory_review(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   const char *reason =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "reason")) : NULL;
   cJSON *idj = req ? cJSON_GetObjectItemCaseSensitive(req, "memory_id") : NULL;
   int64_t id = cJSON_IsNumber(idj) && idj->valuedouble > 0 ? (int64_t)idj->valuedouble : 0;
   if (!id || !action || (strcmp(action, "reject") != 0 && strcmp(action, "restore") != 0))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"positive memory_id and reject/restore action required\"}");
      return 400;
   }
   fact_actor_t actor;
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   char action_copy[16];
   snprintf(action_copy, sizeof(action_copy), "%s", action);
   db2_memory_scope_context_set("", "", 1);
   int rc = strcmp(action, "reject") == 0 ? db2_memory_reject(id, reason)
                                          : db2_memory_restore(id, actor.principal);
   db2_memory_scope_context_clear();
   cJSON_Delete(req);
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"memory review transition failed\"}");
      return 409;
   }
   snprintf(out_buf, (size_t)out_cap, "{\"ok\":true,\"memory_id\":%lld,\"action\":\"%s\"}",
            (long long)id, action_copy);
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
   /* `enabled` IS NO LONGER ACCEPTED. The typed-fact layer has no master gate:
    * it used to default OFF, which turned retraction, recall and class keying
    * into silent no-ops on a stock install. An endpoint that can still switch it
    * off would reintroduce exactly that, so a request carrying `enabled` is
    * refused rather than quietly ignored -- a caller trying to disable the layer
    * must be told it did not happen.
    *
    * The two real knobs stay: -1 means "not in this request", which
    * config_set_typed_facts leaves unchanged. */
   const cJSON *en = cJSON_GetObjectItemCaseSensitive(req, "enabled");
   if (en)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"the typed-fact layer is always enabled; `enabled` is not a "
               "settable option\"}");
      return 400;
   }
   const cJSON *ap = cJSON_GetObjectItemCaseSensitive(req, "auto_promote");
   const cJSON *pt = cJSON_GetObjectItemCaseSensitive(req, "promote_threshold");
   int want_auto = (ap && cJSON_IsBool(ap)) ? (cJSON_IsTrue(ap) ? 1 : 0) : -1;
   int want_threshold = (pt && cJSON_IsNumber(pt) && pt->valueint > 0) ? pt->valueint : -1;
   cJSON_Delete(req);
   if (config_set_typed_facts(want_auto, want_threshold) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"config save failed\"}");
      return 500;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddBoolToObject(resp, "enabled", 1); /* unconditional; no master gate */
   cJSON_AddBoolToObject(resp, "auto_promote",
                         config_kb_typed_facts_auto_promote_enabled() ? 1 : 0);
   cJSON_AddNumberToObject(resp, "promote_threshold", config_kb_typed_facts_promote_threshold());
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

/* POST /v1/console/typed_facts/assertion
 * {action:"approve"|"reject"|"undo", assertion_id:N}.  Authority is resolved
 * exclusively from the verified request context by the mutation seam. */
static int console_typed_facts_assertion(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   cJSON *idj = req ? cJSON_GetObjectItemCaseSensitive(req, "assertion_id") : NULL;
   int64_t id = cJSON_IsNumber(idj) && idj->valuedouble > 0 ? (int64_t)idj->valuedouble : 0;
   fact_review_action_t review;
   if (action && strcmp(action, "approve") == 0)
      review = FACT_REVIEW_APPROVE;
   else if (action && strcmp(action, "reject") == 0)
      review = FACT_REVIEW_REJECT;
   else if (action && strcmp(action, "undo") == 0)
      review = FACT_REVIEW_UNDO;
   else
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action must be approve, reject, or undo\"}");
      return 400;
   }
   if (!id)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"positive assertion_id required\"}");
      return 400;
   }
   fact_actor_t actor;
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   fact_mutation_result_t result;
   int rc = db2_fact_mutation_review(&actor, id, review, &result);
   cJSON_Delete(req);
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"fact review transition failed\"}");
      return 409;
   }
   snprintf(out_buf, (size_t)out_cap,
            "{\"ok\":true,\"assertion_id\":%lld,\"lifecycle\":\"%s\","
            "\"commit_id\":\"%s\"}",
            (long long)result.assertion_id, result.lifecycle, result.commit_id);
   return 200;
}

/* POST /v1/console/typed_facts/entity
 * {action:"merge",from_id:N,into_id:N} or {action:"unmerge",merge_id:N}.
 * IDs select existing canonical rows only; actor identity and authority come
 * exclusively from the verified console request context. */
static int console_typed_facts_entity(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   fact_actor_t actor;
   if (!action || (strcmp(action, "merge") != 0 && strcmp(action, "unmerge") != 0))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action must be merge or unmerge\"}");
      return 400;
   }
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   char cid[FACT_COMMIT_ID_MAX];
   if (strcmp(action, "merge") == 0)
   {
      cJSON *fj = cJSON_GetObjectItemCaseSensitive(req, "from_id");
      cJSON *tj = cJSON_GetObjectItemCaseSensitive(req, "into_id");
      int64_t from = cJSON_IsNumber(fj) && fj->valuedouble > 0 ? (int64_t)fj->valuedouble : 0;
      int64_t into = cJSON_IsNumber(tj) && tj->valuedouble > 0 ? (int64_t)tj->valuedouble : 0;
      if (!from || !into || from == into || fj->valuedouble != (double)from ||
          tj->valuedouble != (double)into)
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"distinct positive integer from_id and into_id required\"}");
         return 400;
      }
      int64_t mid = db2_entity_merge_as(&actor, from, into, cid);
      cJSON_Delete(req);
      if (mid <= 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"entities are not both active or merge would be invalid\"}");
         return 409;
      }
      snprintf(out_buf, (size_t)out_cap, "{\"ok\":true,\"merge_id\":%lld,\"commit_id\":\"%s\"}",
               (long long)mid, cid);
      return 200;
   }
   cJSON *mj = cJSON_GetObjectItemCaseSensitive(req, "merge_id");
   int64_t mid = cJSON_IsNumber(mj) && mj->valuedouble > 0 ? (int64_t)mj->valuedouble : 0;
   if (!mid || mj->valuedouble != (double)mid)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"positive integer merge_id required\"}");
      return 400;
   }
   int rc = db2_entity_unmerge_as(&actor, mid, cid);
   cJSON_Delete(req);
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"merge is unknown, already undone, or no longer current\"}");
      return 409;
   }
   snprintf(out_buf, (size_t)out_cap, "{\"ok\":true,\"merge_id\":%lld,\"commit_id\":\"%s\"}",
            (long long)mid, cid);
   return 200;
}

/* POST /v1/console/typed_facts/commit
 * preview/rollback accepts either one commit_id or one atomic ingest_run_id. */
static int console_typed_facts_commit(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   const char *cid =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "commit_id")) : NULL;
   const char *run =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "ingest_run_id")) : NULL;
   int by_run = run && run[0];
   if (!action || ((!cid || !cid[0]) && !by_run) || (cid && cid[0] && by_run))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"action and exactly one of commit_id or ingest_run_id required\"}");
      return 400;
   }
   char selector[128];
   const char *selected = by_run ? run : cid;
   if (strlen(selected) >= sizeof(selector))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid commit_id\"}");
      return 400;
   }
   snprintf(selector, sizeof(selector), "%s", selected);
   fact_actor_t actor;
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   if (strcmp(action, "rollback") == 0)
   {
      char rollback_id[FACT_COMMIT_ID_MAX];
      int n = by_run ? db2_fact_ingest_run_rollback(&actor, selector, rollback_id)
                     : db2_fact_commit_rollback(&actor, selector, rollback_id);
      cJSON_Delete(req);
      if (n < 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"commit or ingest run is not reversible\"}");
         return 409;
      }
      snprintf(out_buf, (size_t)out_cap,
               "{\"ok\":true,\"changes\":%d,\"rollback_commit_id\":\"%s\"}", n, rollback_id);
      return 200;
   }
   if (strcmp(action, "preview") != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action must be preview or rollback\"}");
      return 400;
   }
   fact_commit_change_t changes[64];
   int n = by_run ? db2_fact_ingest_run_preview(selector, changes, 64)
                  : db2_fact_commit_preview(selector, changes, 64);
   cJSON_Delete(req);
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"commit preview failed\"}");
      return 404;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, by_run ? "ingest_run_id" : "commit_id", selector);
   cJSON *arr = cJSON_AddArrayToObject(root, "changes");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "assertion_id", (double)changes[i].assertion_id);
      cJSON_AddStringToObject(o, "object_kind", changes[i].object_kind);
      cJSON_AddStringToObject(o, "object_key", changes[i].object_key);
      cJSON_AddStringToObject(o, "action", changes[i].action);
      cJSON_AddStringToObject(o, "before", changes[i].before_lifecycle);
      cJSON_AddStringToObject(o, "after", changes[i].after_lifecycle);
      cJSON_AddNumberToObject(o, "before_authority", changes[i].before_authority_rank);
      cJSON_AddNumberToObject(o, "after_authority", changes[i].after_authority_rank);
      cJSON_AddBoolToObject(o, "existed_before", changes[i].existed_before);
      cJSON_AddBoolToObject(o, "existed_after", changes[i].existed_after);
      cJSON_AddStringToObject(o, "detail", changes[i].detail);
      cJSON_AddItemToArray(arr, o);
   }
   char *rendered = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!rendered || strlen(rendered) >= (size_t)out_cap)
   {
      free(rendered);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"commit preview too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", rendered);
   free(rendered);
   return 200;
}

/* POST /v1/console/typed_facts/erasure
 * {action:"preview"|"erase",subject,relation?,object?}. */
static int console_typed_facts_erasure(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action")) : NULL;
   const char *source =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "subject")) : NULL;
   const char *relation =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "relation")) : NULL;
   const char *target =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "object")) : NULL;
   if (!action || !source || !source[0] ||
       (strcmp(action, "preview") != 0 && strcmp(action, "erase") != 0))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"action preview|erase and subject required\"}");
      return 400;
   }
   char action_copy[16];
   snprintf(action_copy, sizeof(action_copy), "%s", action);
   fact_actor_t actor;
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   fact_erasure_impact_t impact;
   char cid[FACT_COMMIT_ID_MAX] = "";
   int rc = strcmp(action_copy, "preview") == 0
                ? db2_fact_erasure_preview(source, relation, target, &impact)
                : db2_fact_erasure_execute(&actor, source, relation, target, &impact, cid);
   cJSON_Delete(req);
   if (rc < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"erasure operation failed\"}");
      return 409;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "ok", 1);
   cJSON_AddStringToObject(root, "action", action_copy);
   cJSON_AddNumberToObject(root, "assertions", impact.assertion_count);
   cJSON_AddNumberToObject(root, "evidence_mentions", impact.evidence_count);
   cJSON_AddStringToObject(root, "residual_data", impact.residual_data);
   if (cid[0])
      cJSON_AddStringToObject(root, "commit_id", cid);
   char *rendered = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!rendered || strlen(rendered) >= (size_t)out_cap)
   {
      free(rendered);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"erasure report too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", rendered);
   free(rendered);
   return 200;
}

static const char *console_json_string(cJSON *req, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(req, name);
   return cJSON_IsString(v) ? v->valuestring : "";
}

/* POST /v1/console/evidence — one authenticated transport for the named P2-P9
 * operations. SQL selection is enum-bound in evidence_lifecycle.c; request data
 * can never select SQL or nominate actor/authority. */
static int console_evidence(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const char *action = req ? console_json_string(req, "action") : "";
   fact_actor_t actor;
   const char *verified_scope_kind = "global";
   const char *verified_scope_id = "";
   if (!req || !action[0])
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"action is required\"}");
      return 400;
   }
   if (db2_fact_actor_from_request(1, &actor) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"authenticated operator required\"}");
      return 403;
   }
   (void)kb_reqctx_verified_scope(&verified_scope_kind, &verified_scope_id);
   evidence_lifecycle_op_t op = 0;
   const char *args[EL_MAX_ARGS] = {0};
   char numbers[EL_MAX_ARGS][32];
   char *owned[2] = {0};
   int nargs = 0, owned_n = 0;
#define EL_ARG(v) args[nargs++] = (v)
#define EL_NUM(name, fallback)                                                                     \
   do                                                                                              \
   {                                                                                               \
      cJSON *nv = cJSON_GetObjectItemCaseSensitive(req, (name));                                   \
      int el_i = nargs;                                                                            \
      snprintf(numbers[el_i], sizeof(numbers[el_i]), "%lld",                                       \
               (long long)(cJSON_IsNumber(nv) ? nv->valuedouble : (fallback)));                    \
      EL_ARG(numbers[el_i]);                                                                       \
   } while (0)
   if (strcmp(action, "changeset.show") == 0 || strcmp(action, "changeset.diff") == 0 ||
       strcmp(action, "changeset.preview_revert") == 0)
   {
      op = strcmp(action, "changeset.show") == 0   ? EL_CHANGESET_SHOW
           : strcmp(action, "changeset.diff") == 0 ? EL_CHANGESET_DIFF
                                                   : EL_CHANGESET_PREVIEW_REVERT;
      EL_ARG(console_json_string(req, "changeset_id"));
   }
   else if (strcmp(action, "changeset.revert") == 0)
   {
      op = EL_CHANGESET_REVERT;
      EL_ARG(console_json_string(req, "changeset_id"));
      EL_ARG(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "force_partial")) ? "true"
                                                                                  : "false");
   }
   else if (strcmp(action, "document.preview_lifecycle") == 0)
   {
      op = EL_DOCUMENT_PREVIEW;
      EL_NUM("doc_id", 0);
      EL_ARG(console_json_string(req, "operation"));
   }
   else if (strcmp(action, "document.apply_lifecycle") == 0)
   {
      op = EL_DOCUMENT_APPLY;
      EL_NUM("doc_id", 0);
      EL_ARG(console_json_string(req, "operation"));
      EL_ARG(console_json_string(req, "preview_token"));
      EL_ARG(console_json_string(req, "reason"));
   }
   else if (strcmp(action, "derived.status") == 0)
   {
      op = EL_DERIVED_STATUS;
      EL_ARG(console_json_string(req, "derived_kind"));
      EL_ARG(console_json_string(req, "derived_memory_id"));
      EL_ARG(console_json_string(req, "input_kind"));
      EL_ARG(console_json_string(req, "input_id"));
   }
   else if (strcmp(action, "review.list") == 0)
   {
      op = EL_REVIEW_LIST;
      EL_NUM("limit", 100);
   }
   else if (strcmp(action, "review.decide") == 0)
   {
      op = EL_REVIEW_DECIDE;
      EL_ARG(console_json_string(req, "item_id"));
      EL_ARG(console_json_string(req, "item_head"));
      EL_ARG(console_json_string(req, "decision"));
      EL_ARG(console_json_string(req, "requested_value"));
      EL_ARG(console_json_string(req, "preview_token"));
   }
   else if (strcmp(action, "ontology.export") == 0)
      op = EL_ONTOLOGY_EXPORT;
   else if (strcmp(action, "ontology.import") == 0)
   {
      op = EL_ONTOLOGY_IMPORT;
      cJSON *package = cJSON_GetObjectItemCaseSensitive(req, "package");
      owned[owned_n] = cJSON_PrintUnformatted(package);
      EL_ARG(owned[owned_n++]);
      EL_ARG(actor.principal); /* provenance is authenticated, never body-supplied */
      EL_ARG(console_json_string(req, "review_record"));
      EL_ARG(console_json_string(req, "signature"));
   }
   else if (strcmp(action, "ontology.dry_run") == 0)
   {
      op = EL_ONTOLOGY_DRY_RUN;
      EL_ARG(console_json_string(req, "package_id"));
      cJSON *ack = cJSON_GetObjectItemCaseSensitive(req, "acknowledge_widening");
      EL_ARG(cJSON_IsTrue(ack) ? "true" : "false");
   }
   else if (strcmp(action, "ontology.migrate") == 0)
   {
      op = EL_ONTOLOGY_MIGRATE;
      EL_ARG(console_json_string(req, "package_id"));
      EL_ARG(console_json_string(req, "preview_token"));
   }
   else if (strcmp(action, "ontology.report") == 0)
   {
      op = EL_ONTOLOGY_REPORT;
      EL_ARG(console_json_string(req, "changeset_id"));
   }
   else if (strcmp(action, "ontology.rollback") == 0)
      op = EL_ONTOLOGY_ROLLBACK;
   else if (strcmp(action, "outcome.record") == 0)
   {
      op = EL_OUTCOME_RECORD;
      EL_ARG(console_json_string(req, "outcome_id"));
      EL_ARG(console_json_string(req, "retrieval_event_id"));
      EL_ARG(console_json_string(req, "subject_kind"));
      EL_ARG(console_json_string(req, "subject_id"));
      EL_ARG(console_json_string(req, "outcome"));
      EL_ARG(console_json_string(req, "task_label"));
      EL_ARG(console_json_string(req, "workflow"));
      EL_ARG(verified_scope_kind);
      EL_ARG(verified_scope_id);
      EL_ARG(console_json_string(req, "resulting_action"));
      EL_ARG(console_json_string(req, "correction_ref"));
      EL_ARG(console_json_string(req, "source_hash"));
      EL_NUM("code_generation", 0);
      EL_ARG(console_json_string(req, "fault_kind"));
      EL_ARG(console_json_string(req, "fault_value"));
   }
   else if (strcmp(action, "recall.trace_record") == 0)
   {
      op = EL_RECALL_TRACE_RECORD;
      EL_ARG(console_json_string(req, "retrieval_event_id"));
      EL_ARG(console_json_string(req, "turn_id"));
      EL_ARG(console_json_string(req, "query_fingerprint"));
      EL_ARG(verified_scope_kind);
      EL_ARG(verified_scope_id);
      EL_ARG(console_json_string(req, "sensitivity"));
      owned[owned_n] = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(req, "results"));
      EL_ARG(owned[owned_n++]);
      EL_ARG(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "persist")) ? "true" : "false");
   }
   else if (strcmp(action, "recall.trace_get") == 0)
   {
      op = EL_RECALL_TRACE_GET;
      EL_ARG(console_json_string(req, "trace_id"));
      EL_ARG(verified_scope_kind);
      EL_ARG(verified_scope_id);
   }
   if (!op)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unknown evidence action\"}");
      return 400;
   }
   int rc = db2_evidence_lifecycle_json(&actor, op, args, nargs, out_buf, out_cap);
   for (int i = 0; i < owned_n; i++)
      free(owned[i]);
   cJSON_Delete(req);
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"evidence operation refused or failed atomically\"}");
      return 409;
   }
   if (strstr(out_buf, "\"stale\": true") || strstr(out_buf, "\"stale\":true"))
      return 409;
   return 200;
#undef EL_ARG
#undef EL_NUM
}

/* The curator-pipeline config keys the console may write, beyond the per-stage
 * enable flags (which are validated against the live registry, below): the
 * persisted stage order, the user presets, the composed custom stages, the tier
 * preset, and the two extract-stage worker counts.
 *
 * kb_curator_tier is a PRESET OVER THE STAGE TOGGLES — config_kb_curator.c's
 * kb_curator_apply_tier rewrites every kb_curator_*_enabled flag from it — so a
 * write here changes what the stage list shows; the page refetches after a save
 * for exactly that reason. The worker counts are the per-stage concurrency the
 * drain reads (kb_curator_drain.c), which is why they belong with the pipeline
 * rather than on a general settings page. */
static const char *const PIPELINE_CONFIG_KEYS[] = {
    "kb_curator_stage_order", "kb_curator_user_presets",         "kb_curator_custom_stages",
    "kb_curator_tier",        "kb_curator_extract_docs_workers", "kb_curator_extract_code_workers",
};

/* True iff `key` is a per-stage enable flag advertised by the live registry.
 * Deriving the allowlist from kb_curator_stages_json() rather than a hand-kept
 * list keeps it correct as stages are added or renamed (Option B, single source
 * of truth) — a stage with a null config_key is embedder-gated and stays
 * read-only. */
static int pipeline_stage_config_key(const char *key)
{
   if (!key || !key[0])
      return 0;
   cJSON *stages = kb_curator_stages_json();
   int found = 0;
   const cJSON *st = NULL;
   cJSON_ArrayForEach(st, stages)
   {
      const char *ck = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "config_key"));
      if (ck && strcmp(ck, key) == 0)
      {
         found = 1;
         break;
      }
   }
   cJSON_Delete(stages);
   return found;
}

static int pipeline_config_key_allowed(const char *key)
{
   for (size_t i = 0; i < sizeof(PIPELINE_CONFIG_KEYS) / sizeof(PIPELINE_CONFIG_KEYS[0]); i++)
      if (strcmp(PIPELINE_CONFIG_KEYS[i], key) == 0)
         return 1;
   return pipeline_stage_config_key(key);
}

/* Current value of every pipeline-relevant config key, so the GUI renders the
 * toggles, order, presets, and custom stages from one round trip. */
static cJSON *pipeline_config_json(void)
{
   cJSON *out = cJSON_CreateObject();
   for (size_t i = 0; i < sizeof(PIPELINE_CONFIG_KEYS) / sizeof(PIPELINE_CONFIG_KEYS[0]); i++)
   {
      cJSON *value = config_client_value_copy(PIPELINE_CONFIG_KEYS[i]);
      if (value)
         cJSON_AddItemToObject(out, PIPELINE_CONFIG_KEYS[i], value);
   }
   cJSON *stages = kb_curator_stages_json();
   const cJSON *st = NULL;
   cJSON_ArrayForEach(st, stages)
   {
      const char *ck = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "config_key"));
      if (!ck || cJSON_GetObjectItemCaseSensitive(out, ck))
         continue; /* embedder-gated, or a key two stages share */
      cJSON *value = config_client_value_copy(ck);
      if (value)
         cJSON_AddItemToObject(out, ck, value);
   }
   cJSON_Delete(stages);
   return out;
}

/* Write a JSON response, falling back to a minimal body if it would not fit. */
static int console_send(cJSON *resp, int status, const char *fallback, char *out_buf, int out_cap)
{
   char *s = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "%s", fallback);
      return status;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return status;
}

/* GET /v1/console/pipeline — the curator pipeline as data for the console's
 * Pipeline page: the live stage registry (Option B), the built-in presets, and
 * the current value of every config key the page toggles. The KB owns the
 * curator, so this is served in-process rather than proxied through
 * aimee-server. */
static int console_pipeline(char *out_buf, int out_cap)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddItemToObject(resp, "stages", kb_curator_stages_json());
   cJSON_AddItemToObject(resp, "presets", kb_curator_presets_json());
   cJSON_AddItemToObject(resp, "config", pipeline_config_json());
   return console_send(resp, 200, "{\"error\":\"pipeline too large\"}", out_buf, out_cap);
}

/* Render a JSON value as the text module mutation contract parses. Returns 0 and
 * fills `buf` on success, -1 for a type this config surface does not accept. */
static int pipeline_value_text(const cJSON *v, char *buf, size_t cap)
{
   if (cJSON_IsBool(v))
   {
      snprintf(buf, cap, "%s", cJSON_IsTrue(v) ? "true" : "false");
      return 0;
   }
   if (cJSON_IsNumber(v))
   {
      snprintf(buf, cap, "%d", v->valueint);
      return 0;
   }
   if (cJSON_IsString(v) && v->valuestring)
   {
      if (strlen(v->valuestring) >= cap)
         return -1;
      snprintf(buf, cap, "%s", v->valuestring);
      return 0;
   }
   return -1;
}

/* POST /v1/console/pipeline/config — set ONE pipeline config key: {key, value}.
 * The key must be a stage enable flag the live registry advertises or one of
 * PIPELINE_CONFIG_KEYS, so the console cannot reach arbitrary config through
 * this route. Persists to aimee.yaml; the curator picks it up on its next
 * config load. */
static int console_pipeline_config(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   const char *key =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "key")) : NULL;
   const cJSON *val = req ? cJSON_GetObjectItemCaseSensitive(req, "value") : NULL;
   if (!key || !key[0] || !val)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"key and value are required\"}");
      return 400;
   }
   if (!pipeline_config_key_allowed(key))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not a pipeline config key\"}");
      return 403;
   }
   const char *secret_name = config_client_secret_name(key);
   cJSON *current = secret_name ? NULL : config_client_value_copy(key);
   if (!secret_name && !current)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unknown config key\"}");
      return 400;
   }
   char key_copy[128];
   snprintf(key_copy, sizeof(key_copy), "%s", key);
   cJSON_Delete(current);
   /* Sized for the largest pipeline value: the custom-stages / user-presets JSON
    * blobs, which config_field_set_value truncates to the field width anyway. */
   char text[8192];
   int vrc = pipeline_value_text(val, text, sizeof(text));
   if (secret_name && cJSON_IsString(val) && val->valuestring)
      OPENSSL_cleanse(val->valuestring, strlen(val->valuestring));
   cJSON_Delete(req);
   if (vrc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unsupported value type or too long\"}");
      return 400;
   }
   if (secret_name)
   {
      int configured = text[0] ? 1 : 0;
      int src = config_secret_store(secret_name, text);
      OPENSSL_cleanse(text, sizeof(text));
      if (src != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"credential Vault write failed\"}");
         return 500;
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddBoolToObject(resp, "ok", 1);
      cJSON_AddStringToObject(resp, "key", key_copy);
      cJSON_AddBoolToObject(resp, "value", configured);
      cJSON_AddBoolToObject(resp, "secret", 1);
      return console_send(resp, 200, "{\"ok\":true}", out_buf, out_cap);
   }
   /* config_set is the surgical single-field write: it validates against the
    * field descriptor, patches just that key in the document, and republishes
    * the snapshot -- the same three steps this did by hand through a legacy_config_record. */
   if (config_set(key_copy, text) != 0)
   {
      OPENSSL_cleanse(text, sizeof(text));
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid value for this key\"}");
      return 400;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddStringToObject(resp, "key", key_copy);
   cJSON *updated = config_client_value_copy(key_copy);
   cJSON_AddItemToObject(resp, "value", updated ? updated : cJSON_CreateNull());
   cJSON_AddBoolToObject(resp, "secret", 0);
   OPENSSL_cleanse(text, sizeof(text));
   return console_send(resp, 200, "{\"ok\":true}", out_buf, out_cap);
}

/* ── KB-owned settings ────────────────────────────────────────────────────────
 * The config the KB OWNS, edited here rather than on aimee-server's Settings
 * page. The split is by which binary the option actually governs, not by key
 * prefix: the kb runs the embedder and the synth tier, so their
 * model/endpoint/topology keys belong to the kb console. Deliberately NOT here:
 * kb_mode / kb_client_url / kb_client_bearer_token (they configure how
 * AIMEE-SERVER reaches a kb — server-side client config, read in
 * server/server_main.c) and kb_evidence_emit_enabled (read by
 * server/ingress_preinject.c).
 *
 * `section` groups the fields for the console page; `restart` marks the ones
 * bound at startup (the kb API listener and the deploy topology), mirroring
 * their restart behavior. */
typedef struct
{
   const char *key;
   const char *section;
   int restart;
} kb_setting_t;

static const kb_setting_t KB_SETTINGS[] = {
    /* Embedder — the kb embeds and searches; aimee-server only reads the value. */
    {"embedder_command", "Embedder", 0},
    {"embedder_model", "Embedder", 0},
    {"embedder_dims", "Embedder", 0},
    {"embedder_url", "Embedder", 0},
    /* Reranker. */
    /* Synth tier. */
    {"synthesis_endpoint", "Synth", 1},
    {"synthesis_model", "Synth", 1},
    /* Knowledge base proper (all read inside the kb binary). */
    {"kb_search_max_results", "Knowledge base", 0},
    {"kb_fusion_mode", "Knowledge base", 0},
    {"kb_mining_enabled", "Knowledge base", 0},
    /* typed_facts_enabled is deliberately absent, and now has no owner at all:
     * the master gate is retired and the layer is unconditional. The Typed Facts
     * page keeps the two real knobs (auto-promote, threshold), which are not in
     * config_fields and are set through /v1/console/typed_facts/config. */
    {"kb_api_http_port", "Knowledge base", 1},
    {"kb_api_bearer_token", "Knowledge base", 1},
    /* Document ingest sidecars. */
    {"kb_pdf_tier", "Document ingest", 0},
    {"ocr_command", "Document ingest", 0},
    {"tsr_command", "Document ingest", 0},
    {"css_style_graph_enabled", "Document ingest", 0},
    {"css_render_command", "Document ingest", 0},
};

static const kb_setting_t *kb_setting_lookup(const char *key)
{
   if (!key || !key[0])
      return NULL;
   for (size_t i = 0; i < sizeof(KB_SETTINGS) / sizeof(KB_SETTINGS[0]); i++)
      if (strcmp(KB_SETTINGS[i].key, key) == 0)
         return &KB_SETTINGS[i];
   return NULL;
}

/* GET /v1/console/settings — every KB-owned option with its current value, so
 * the console renders the page from one call. A key the config allowlist does
 * not know is skipped rather than reported with a null value: that only happens
 * if KB_SETTINGS drifts from config_fields.c, and a silently-missing row is
 * better than an uneditable one. */
static int console_settings(char *out_buf, int out_cap)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(resp, "fields");
   for (size_t i = 0; i < sizeof(KB_SETTINGS) / sizeof(KB_SETTINGS[0]); i++)
   {
      const char *secret_name = config_client_secret_name(KB_SETTINGS[i].key);
      cJSON *value = secret_name ? cJSON_CreateBool(runtime_secret_has(secret_name))
                                 : config_client_value_copy(KB_SETTINGS[i].key);
      if (!value)
         continue;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "key", KB_SETTINGS[i].key);
      cJSON_AddStringToObject(o, "section", KB_SETTINGS[i].section);
      cJSON_AddBoolToObject(o, "restart", KB_SETTINGS[i].restart);
      cJSON_AddItemToObject(o, "value", value);
      cJSON_AddBoolToObject(o, "secret", config_client_key_is_secret(KB_SETTINGS[i].key));
      cJSON_AddItemToArray(arr, o);
   }
   return console_send(resp, 200, "{\"error\":\"settings too large\"}", out_buf, out_cap);
}

/* POST /v1/console/settings/config — set ONE KB-owned option: {key, value}.
 * Same shape and same containment as the pipeline route: the key must be in
 * KB_SETTINGS, so this cannot reach arbitrary config (db2_url, the agent roster,
 * or aimee-server's own keys). Persists to aimee.yaml; the `restart` fields take
 * effect when the kb restarts. */
static int console_settings_config(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   const char *key =
       req ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "key")) : NULL;
   const cJSON *val = req ? cJSON_GetObjectItemCaseSensitive(req, "value") : NULL;
   if (!key || !key[0] || !val)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"key and value are required\"}");
      return 400;
   }
   const kb_setting_t *ks = kb_setting_lookup(key);
   if (!ks)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not a kb-owned setting\"}");
      return 403;
   }
   const char *secret_name = config_client_secret_name(key);
   cJSON *current = secret_name ? NULL : config_client_value_copy(key);
   if (!secret_name && !current)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unknown config key\"}");
      return 400;
   }
   char key_copy[128];
   snprintf(key_copy, sizeof(key_copy), "%s", key);
   cJSON_Delete(current);
   char text[8192];
   int vrc = pipeline_value_text(val, text, sizeof(text));
   if (secret_name && cJSON_IsString(val) && val->valuestring)
      OPENSSL_cleanse(val->valuestring, strlen(val->valuestring));
   cJSON_Delete(req);
   if (vrc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unsupported value type or too long\"}");
      return 400;
   }
   if (secret_name)
   {
      int configured = text[0] ? 1 : 0;
      int src = config_secret_store(secret_name, text);
      OPENSSL_cleanse(text, sizeof(text));
      if (src != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"credential Vault write failed\"}");
         return 500;
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddBoolToObject(resp, "ok", 1);
      cJSON_AddStringToObject(resp, "key", key_copy);
      cJSON_AddBoolToObject(resp, "value", configured);
      cJSON_AddBoolToObject(resp, "secret", 1);
      cJSON_AddBoolToObject(resp, "restart", ks->restart);
      return console_send(resp, 200, "{\"ok\":true}", out_buf, out_cap);
   }
   /* config_set is the surgical single-field write: it validates against the
    * field descriptor, patches just that key in the document, and republishes
    * the snapshot -- the same three steps this did by hand through a legacy_config_record. */
   if (config_set(key_copy, text) != 0)
   {
      OPENSSL_cleanse(text, sizeof(text));
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid value for this key\"}");
      return 400;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddStringToObject(resp, "key", key_copy);
   cJSON *updated = config_client_value_copy(key_copy);
   cJSON_AddItemToObject(resp, "value", updated ? updated : cJSON_CreateNull());
   cJSON_AddBoolToObject(resp, "secret", 0);
   cJSON_AddBoolToObject(resp, "restart", ks->restart);
   OPENSSL_cleanse(text, sizeof(text));
   return console_send(resp, 200, "{\"ok\":true}", out_buf, out_cap);
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
   if (route_is(path, "/v1/console/memories"))
      return strcmp(method, "GET") == 0 ? console_memories(out_buf, out_cap)
                                        : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/memories/review"))
      return strcmp(method, "POST") == 0 ? console_memory_review(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/config"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_config(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/pipeline"))
      return strcmp(method, "GET") == 0 ? console_pipeline(out_buf, out_cap)
                                        : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/pipeline/config"))
      return strcmp(method, "POST") == 0 ? console_pipeline_config(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/settings"))
      return strcmp(method, "GET") == 0 ? console_settings(out_buf, out_cap)
                                        : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/settings/config"))
      return strcmp(method, "POST") == 0 ? console_settings_config(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/relation"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_relation(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/assertion"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_assertion(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/entity"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_entity(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/commit"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_commit(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/typed_facts/erasure"))
      return strcmp(method, "POST") == 0 ? console_typed_facts_erasure(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   if (route_is(path, "/v1/console/evidence"))
      return strcmp(method, "POST") == 0 ? console_evidence(body, out_buf, out_cap)
                                         : console_method_not_allowed(out_buf, out_cap);
   return -1; /* not a console route — caller continues dispatch */
}
