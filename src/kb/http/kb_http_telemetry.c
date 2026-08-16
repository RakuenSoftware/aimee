/* kb_http_telemetry.c: /v1/metrics + /v1/telemetry routes (P9a telemetry export).
 *
 * See kb_http_telemetry.h. Mirrors kb_http_rate.c: the authenticated actor comes
 * from kb_reqctx; every op runs inside a tenant scope (db2_tenant_scope_begin sets
 * aimee.principal), so the admin gate is enforced at the DB layer inside the
 * SECURITY DEFINER functions (a non-authorized caller surfaces here as 403). The
 * scrape/ingest TOKEN path (kb_http_telemetry_token_route) sits BEFORE the bearer
 * gate: a valid token (constant-time SHA-256 compare) opens an OWNER scope — the
 * same "a validated org secret maps to the owner principal" mapping the unscoped
 * kb bearer already uses — so the admin-gated definers accept it. The token is
 * NEVER written to the body or logs (a mismatch just falls through to a 401). */

#include "kb_http_telemetry.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_identity.h"
#include "kb_reqctx.h"
#include "kb_verifier.h"
#include "org_telemetry.h"
#include "org_telemetry_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The scrape/ingest token as a SHA-256 hex (config telemetry.metrics_token).
 * Empty = no token configured. Set once at startup by kb_http_set_telemetry_token;
 * compared constant-time against the presented bearer's SHA-256. */
static char g_telemetry_token_hash[65];

void kb_http_set_telemetry_token(const char *hash)
{
   g_telemetry_token_hash[0] = '\0';
   if (hash && hash[0])
      snprintf(g_telemetry_token_hash, sizeof(g_telemetry_token_hash), "%s", hash);
}

static int err(char *out, int cap, int status, const char *msg)
{
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", msg);
   return status;
}

static int emit(cJSON *root, char *out, int cap, int status)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s || (int)strlen(s) >= cap)
   {
      free(s);
      snprintf(out, (size_t)cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out, (size_t)cap, "%s", s);
   free(s);
   return status;
}

/* Map a tenant-scope/db2 return into an HTTP status. */
static int tenant_http_status(int rc)
{
   if (rc == DB2_ERR_TENANT_REQUIRES_PG)
      return 503;
   if (rc == DB2_ERR_TENANT_UNAUTHENTICATED)
      return 401;
   if (rc == DB2_ERR_TENANT_DENIED)
      return 403;
   return 500;
}

/* Open the authenticated actor's tenant scope (team 0 = principal only). Returns
 * 0, or writes an HTTP status (>0) into *http_out. Mirrors kb_http_rate.c. */
static int begin_actor_scope(char *out, int cap, int *http_out)
{
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
   {
      *http_out = err(out, cap, 401, "authentication required");
      return -1;
   }
   int rc = db2_tenant_scope_begin(actor, 0);
   if (rc != 0)
   {
      *http_out = err(out, cap, tenant_http_status(rc), "tenant scope failed");
      return -1;
   }
   return 0;
}

/* Open an OWNER tenant scope for the validated-token scrape/ingest path. The
 * owner principal is the same one an unscoped kb bearer maps to; the only
 * operation the token reaches is the read-only snapshot / content-free ingest. */
static int begin_owner_scope(char *out, int cap, int *http_out)
{
   kb_verify_result_t ovr;
   memset(&ovr, 0, sizeof(ovr));
   kb_principal_t owner;
   if (kb_principal_from_verify(&ovr, "", &owner) != 0)
   {
      *http_out = err(out, cap, 500, "owner scope init failed");
      return -1;
   }
   int rc = db2_tenant_scope_begin(&owner, 0);
   if (rc != 0)
   {
      *http_out = err(out, cap, tenant_http_status(rc), "tenant scope failed");
      return -1;
   }
   return 0;
}

/* Render the authoritative-state snapshot as Prometheus text. Assumes an open
 * scope. Returns an HTTP status and writes the body; does NOT commit. */
static int do_metrics(char *out, int cap)
{
   org_metric_row_t *rows = calloc(DB2_TELEMETRY_MAX_ROWS, sizeof(*rows));
   if (!rows)
      return err(out, cap, 500, "out of memory");
   int n = db2_metrics_snapshot(rows, DB2_TELEMETRY_MAX_ROWS);
   if (n < 0)
   {
      free(rows);
      if (n == DB2_TELEMETRY_ERR_DENIED)
         return err(out, cap, 403, "not authorized (org-admin required)");
      if (n == DB2_TELEMETRY_ERR_TOOBIG)
         return err(out, cap, 500, "metrics snapshot exceeds buffer (too many series)");
      return err(out, cap, 500, "metrics snapshot failed");
   }
   int wrote = org_telemetry_render_prom(rows, n, out, (size_t)cap);
   free(rows);
   if (wrote < 0)
      return err(out, cap, 500, "metrics render overflow");
   return 200;
}

/* Parse + ingest one content-free telemetry metric. origin_cn is SERVER-SET (never
 * body text); team is NULL in P9a (never trusted from the body). Assumes an open
 * scope. Returns an HTTP status and writes {"result":...}; does NOT commit. */
static int do_ingest(const char *body, const char *origin_cn, char *out, int cap)
{
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   if (!b)
      return err(out, cap, 400, "invalid JSON body");
   /* Parse ONLY the known fields — there is no generic passthrough. */
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(b, "source_event_id");
   cJSON *jschema = cJSON_GetObjectItemCaseSensitive(b, "event_schema");
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(b, "metric_name");
   cJSON *jkind = cJSON_GetObjectItemCaseSensitive(b, "metric_kind");
   cJSON *jval = cJSON_GetObjectItemCaseSensitive(b, "value");
   cJSON *jts = cJSON_GetObjectItemCaseSensitive(b, "ts");
   if (!cJSON_IsString(jid) || !cJSON_IsString(jschema) || !cJSON_IsString(jname) ||
       !cJSON_IsString(jkind) || !cJSON_IsNumber(jval) || !cJSON_IsNumber(jts))
   {
      cJSON_Delete(b);
      return err(out, cap, 400,
                 "source_event_id, event_schema, metric_name, metric_kind (strings), "
                 "value, ts (numbers) required");
   }
   /* Cheap client-side shape checks (the definer is authoritative + fail-closed). */
   if (!org_telemetry_metric_name_valid(jname->valuestring))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "metric_name must match [a-zA-Z0-9_:]{1,128}");
   }
   if (strcmp(jkind->valuestring, "counter") != 0 && strcmp(jkind->valuestring, "gauge") != 0)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "metric_kind must be counter|gauge");
   }
   char source_event_id[256], event_schema[160], metric_name[160], metric_kind[16], value_text[64];
   snprintf(source_event_id, sizeof(source_event_id), "%s", jid->valuestring);
   snprintf(event_schema, sizeof(event_schema), "%s", jschema->valuestring);
   snprintf(metric_name, sizeof(metric_name), "%s", jname->valuestring);
   snprintf(metric_kind, sizeof(metric_kind), "%s", jkind->valuestring);
   /* Format value as exact integer when integral, else fixed decimal (never a
    * lossy/scientific rendering the NUMERIC coercion could misread). */
   double v = jval->valuedouble;
   if (v == (double)(long long)v)
      snprintf(value_text, sizeof(value_text), "%lld", (long long)v);
   else
      snprintf(value_text, sizeof(value_text), "%.10f", v);
   int64_t ts = (int64_t)jts->valuedouble;
   cJSON_Delete(b);

   char result[DB2_TELEMETRY_RESULT_MAX] = "";
   int rc = db2_telemetry_ingest(source_event_id, origin_cn, 0 /*has_team*/, 0, event_schema,
                                 metric_name, metric_kind, value_text, ts, result, sizeof(result));
   if (rc != 0)
      return err(out, cap, 500, "telemetry ingest failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "result", result);
   return emit(o, out, cap, 200);
}

int kb_http_telemetry_token_route(const char *method, const char *path, const char *query_string,
                                  const char *body, const char *presented, char *out_buf,
                                  int out_cap)
{
   (void)query_string;
   if (!path)
      return -1;
   int is_metrics = (strcmp(path, "/v1/metrics") == 0);
   int is_ingest = (strcmp(path, "/v1/telemetry/metrics") == 0);
   if (!is_metrics && !is_ingest)
      return -1; /* not a token-eligible route */
   /* No token configured (a valid hash is exactly 64 hex chars), or none
    * presented -> fall through to the admin path. */
   if (strnlen(g_telemetry_token_hash, 65) != 64)
      return -1;
   if (!presented || !presented[0])
      return -1;
   /* Constant-time SHA-256 compare. A mismatch falls through (the normal bearer
    * gate then 401s); the token is never echoed or logged. */
   char phash[65];
   org_telemetry_sha256_hex(presented, phash);
   if (!org_telemetry_token_hash_eq(phash, g_telemetry_token_hash))
      return -1;

   if (is_metrics && strcmp(method, "GET") != 0)
      return err(out_buf, out_cap, 405, "method not allowed");
   if (is_ingest && strcmp(method, "POST") != 0)
      return err(out_buf, out_cap, 405, "method not allowed");

   int http = 0;
   if (begin_owner_scope(out_buf, out_cap, &http) != 0)
      return http;
   int status;
   if (is_metrics)
      status = do_metrics(out_buf, out_cap);
   else
      status = do_ingest(body, "telemetry-token", out_buf, out_cap);
   if (status >= 200 && status < 300)
   {
      if (db2_tenant_scope_commit() != 0)
         return err(out_buf, out_cap, 500, "commit failed");
   }
   else
      db2_tenant_scope_rollback();
   return status;
}

/* GET /v1/telemetry/allow -> the allowlist (admin only, DB-enforced). */
static int handle_allow_show(char *out, int cap)
{
   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   db2_telemetry_allow_row_t *rows = calloc(DB2_TELEMETRY_ALLOW_MAX_ROWS, sizeof(*rows));
   if (!rows)
   {
      db2_tenant_scope_rollback();
      return err(out, cap, 500, "out of memory");
   }
   int n = db2_telemetry_allow_show(rows, DB2_TELEMETRY_ALLOW_MAX_ROWS);
   if (n < 0)
   {
      free(rows);
      db2_tenant_scope_rollback();
      if (n == DB2_TELEMETRY_ERR_DENIED)
         return err(out, cap, 403, "not authorized (org-admin required)");
      return err(out, cap, 500, "telemetry allow show failed");
   }
   if (db2_tenant_scope_commit() != 0)
   {
      free(rows);
      return err(out, cap, 500, "commit failed");
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "allowlist");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "event_schema", rows[i].event_schema);
      cJSON_AddStringToObject(o, "metric_names", rows[i].metric_names);
      cJSON_AddBoolToObject(o, "enabled", rows[i].enabled ? 1 : 0);
      cJSON_AddStringToObject(o, "updated_at", rows[i].updated_at);
      cJSON_AddItemToArray(arr, o);
   }
   free(rows);
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out, cap, 200);
}

/* POST /v1/telemetry/allow {event_schema, metric_names:[...], enabled} -> upsert an
 * allowlist entry (admin-gated at the DB layer, WORM-audited). */
static int handle_allow_set(const char *body, char *out, int cap)
{
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jschema = b ? cJSON_GetObjectItemCaseSensitive(b, "event_schema") : NULL;
   cJSON *jnames = b ? cJSON_GetObjectItemCaseSensitive(b, "metric_names") : NULL;
   cJSON *jenabled = b ? cJSON_GetObjectItemCaseSensitive(b, "enabled") : NULL;
   if (!cJSON_IsString(jschema) || !cJSON_IsArray(jnames))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "event_schema (string) and metric_names (array) required");
   }
   int enabled = jenabled ? cJSON_IsTrue(jenabled) : 1;
   char event_schema[160];
   snprintf(event_schema, sizeof(event_schema), "%s", jschema->valuestring);
   /* Build a Postgres array literal '{a,b,c}' from the JSON string array. Each
    * name must be a bounded [a-zA-Z0-9_:] identifier — so the literal needs no
    * quoting/escaping and no free text can enter the allowlist. */
   char arr_lit[1024];
   size_t o = 0;
   arr_lit[o++] = '{';
   arr_lit[o] = '\0';
   int first = 1;
   cJSON *el = NULL;
   cJSON_ArrayForEach(el, jnames)
   {
      if (!cJSON_IsString(el) || !org_telemetry_metric_name_valid(el->valuestring))
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "each metric_name must match [a-zA-Z0-9_:]{1,128}");
      }
      size_t nl = strlen(el->valuestring);
      if (o + nl + 2 >= sizeof(arr_lit))
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "metric_names too large");
      }
      if (!first)
         arr_lit[o++] = ',';
      memcpy(arr_lit + o, el->valuestring, nl);
      o += nl;
      arr_lit[o] = '\0';
      first = 0;
   }
   if (o + 2 >= sizeof(arr_lit))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "metric_names too large");
   }
   arr_lit[o++] = '}';
   arr_lit[o] = '\0';
   cJSON_Delete(b);

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int rc = db2_telemetry_allow(event_schema, arr_lit, enabled);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_TELEMETRY_ERR_DENIED)
         return err(out, cap, 403, "not authorized to set allowlist (org-admin required)");
      return err(out, cap, 500, "telemetry allow set failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o2 = cJSON_CreateObject();
   cJSON_AddStringToObject(o2, "event_schema", event_schema);
   cJSON_AddStringToObject(o2, "metric_names", arr_lit);
   cJSON_AddBoolToObject(o2, "enabled", enabled ? 1 : 0);
   return emit(o2, out, cap, 200);
}

/* Admin (post-bearer-gate) path. GET /v1/metrics + POST /v1/telemetry/metrics run
 * as the authenticated ADMIN actor (the DB definer's admin gate is the authz). */
static int handle_admin_metrics(const char *method, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return err(out, cap, 405, "method not allowed");
   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int status = do_metrics(out, cap);
   if (status >= 200 && status < 300)
   {
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
   }
   else
      db2_tenant_scope_rollback();
   return status;
}

static int handle_admin_ingest(const char *method, const char *body, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   const kb_principal_t *actor = kb_reqctx_actor();
   char origin[160] = "telemetry-admin";
   if (actor && actor->label[0])
      snprintf(origin, sizeof(origin), "%s", actor->label);
   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int status = do_ingest(body, origin, out, cap);
   if (status >= 200 && status < 300)
   {
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
   }
   else
      db2_tenant_scope_rollback();
   return status;
}

int kb_http_telemetry_route(const char *method, const char *path, const char *query_string,
                            const char *body, char *out_buf, int out_cap)
{
   (void)query_string;
   if (!path)
      return -1;
   if (strcmp(path, "/v1/metrics") == 0)
      return handle_admin_metrics(method, out_buf, out_cap);
   if (strcmp(path, "/v1/telemetry/metrics") == 0)
      return handle_admin_ingest(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/telemetry/allow") == 0)
   {
      if (strcmp(method, "GET") == 0)
         return handle_allow_show(out_buf, out_cap);
      if (strcmp(method, "POST") == 0)
         return handle_allow_set(body, out_buf, out_cap);
      return err(out_buf, out_cap, 405, "method not allowed");
   }
   return -1; /* not ours */
}
