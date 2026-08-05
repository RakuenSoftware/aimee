/* kb_http_console.c: /v1/console routes for the aimee-kb web console.
 * See kb_http_console.h. Reached only with a console-admin credential that the
 * control-web module, reached through the event bus, has already authorized. */
#include "kb_http_console.h"

#include "aimee.h" /* now_utc */
#include "cJSON.h"
#include "config.h"                 /* config_load / config_save (§8 tune) */
#include "config_fields.h"          /* typed get/set of the pipeline config keys */
#include "kb_curator_drain.h"       /* kb_curator_stages_json / _presets_json */
#include "kb_service.h"             /* kb_service_workers_json, kb_service_ctx_t */
#include "kb_service_kb.h"          /* kb_service_health_json */
#include "db2/kb_service_backend.h" /* async queue status */
#include "db2/ontology_evolution.h" /* db2_ontology_* (§8 observe + act) */
#include "rel_types.h"              /* REL_TYPE_NAME_MAX */
#include <openssl/crypto.h>         /* wipe transient credential request copies */

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
   cJSON_AddBoolToObject(c, "typed_facts_enabled", config_typed_facts_enabled() ? 1 : 0);
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
   /* KB-owned master enable/disable for the whole typed-facts layer. -1 means
    * "not in this request", which config_set_typed_facts leaves unchanged. */
   const cJSON *en = cJSON_GetObjectItemCaseSensitive(req, "enabled");
   const cJSON *ap = cJSON_GetObjectItemCaseSensitive(req, "auto_promote");
   const cJSON *pt = cJSON_GetObjectItemCaseSensitive(req, "promote_threshold");
   int want_enabled = (en && cJSON_IsBool(en)) ? (cJSON_IsTrue(en) ? 1 : 0) : -1;
   int want_auto = (ap && cJSON_IsBool(ap)) ? (cJSON_IsTrue(ap) ? 1 : 0) : -1;
   int want_threshold = (pt && cJSON_IsNumber(pt) && pt->valueint > 0) ? pt->valueint : -1;
   cJSON_Delete(req);
   if (config_set_typed_facts(want_enabled, want_auto, want_threshold) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"config save failed\"}");
      return 500;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddBoolToObject(resp, "enabled", config_typed_facts_enabled() ? 1 : 0);
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
      const config_field_t *f = config_field_lookup(PIPELINE_CONFIG_KEYS[i]);
      if (f)
         cJSON_AddItemToObject(out, PIPELINE_CONFIG_KEYS[i],
                               config_field_public_value_json_current(f));
   }
   cJSON *stages = kb_curator_stages_json();
   const cJSON *st = NULL;
   cJSON_ArrayForEach(st, stages)
   {
      const char *ck = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "config_key"));
      if (!ck || cJSON_GetObjectItemCaseSensitive(out, ck))
         continue; /* embedder-gated, or a key two stages share */
      const config_field_t *f = config_field_lookup(ck);
      if (f)
         cJSON_AddItemToObject(out, ck, config_field_public_value_json_current(f));
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

/* Render a JSON value as the text config_field_set_value parses. Returns 0 and
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
   const config_field_t *f = config_field_lookup(key);
   if (!f)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unknown config key\"}");
      return 400;
   }
   char key_copy[128];
   snprintf(key_copy, sizeof(key_copy), "%s", key);
   const char *secret_name = config_field_secret_name(f);
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
    * the snapshot -- the same three steps this did by hand through a config_t. */
   if (config_set(key_copy, text) != 0)
   {
      OPENSSL_cleanse(text, sizeof(text));
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid value for this key\"}");
      return 400;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddStringToObject(resp, "key", key_copy);
   cJSON_AddItemToObject(resp, "value", config_field_public_value_json_current(f));
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
 * their RELOAD_RESTART class in config_fields.c. */
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
    /* typed_facts_enabled is deliberately absent: the console's Typed Facts page
     * owns it, next to the promotion queue it gates and the two knobs
     * (auto-promote, threshold) that are not in config_fields at all and can only
     * be set through /v1/console/typed_facts/config. One owner per option. */
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
      const config_field_t *f = config_field_lookup(KB_SETTINGS[i].key);
      if (!f)
         continue;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "key", KB_SETTINGS[i].key);
      cJSON_AddStringToObject(o, "section", KB_SETTINGS[i].section);
      cJSON_AddBoolToObject(o, "restart", KB_SETTINGS[i].restart);
      cJSON_AddItemToObject(o, "value", config_field_public_value_json_current(f));
      cJSON_AddBoolToObject(o, "secret", config_field_secret_name(f) ? 1 : 0);
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
   const config_field_t *f = config_field_lookup(key);
   if (!f)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unknown config key\"}");
      return 400;
   }
   char key_copy[128];
   snprintf(key_copy, sizeof(key_copy), "%s", key);
   const char *secret_name = config_field_secret_name(f);
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
    * the snapshot -- the same three steps this did by hand through a config_t. */
   if (config_set(key_copy, text) != 0)
   {
      OPENSSL_cleanse(text, sizeof(text));
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid value for this key\"}");
      return 400;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddStringToObject(resp, "key", key_copy);
   cJSON_AddItemToObject(resp, "value", config_field_public_value_json_current(f));
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
   return -1; /* not a console route — caller continues dispatch */
}
