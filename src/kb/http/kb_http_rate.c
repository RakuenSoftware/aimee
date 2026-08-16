/* kb_http_rate.c: /v1/rate routes (P4b keyed fixed-window rate limiter).
 *
 * The authenticated actor comes from kb_reqctx (set by the router after verification);
 * every op runs inside a tenant scope (db2_tenant_scope_begin sets aimee.principal), so
 * the admin gate for POST /v1/rate/policy and the admin-OR-team-lead gate for GET
 * /v1/rate/show are enforced at the DB layer inside the SECURITY DEFINER functions — a
 * non-authorized caller surfaces here as 403. RATE ONLY: org_rate_check (the atomic keyed
 * admission) is the P2b egress-enforcement primitive and is NOT routed in P4b; only the
 * policy set/show admin surface is exposed here. The budget core is P4a. */

#include "kb_http_rate.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_reqctx.h"
#include "org_rate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int err(char *out, int cap, int status, const char *msg)
{
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", msg);
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

/* One of the five limiter dimensions. */
static int dim_valid(const char *s)
{
   return s && (strcmp(s, "team") == 0 || strcmp(s, "project") == 0 || strcmp(s, "cert") == 0 ||
                strcmp(s, "model") == 0 || strcmp(s, "cred_slot") == 0);
}

/* A scope_key is a bounded non-empty token: 1..127 printable non-space chars (the DB is
 * authoritative; '*' is the dim default). */
static int scope_valid(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t len = strlen(s);
   if (len > 127)
      return 0;
   for (const char *p = s; *p; ++p)
      if ((unsigned char)*p <= ' ' || *p == '"' || *p == '\\')
         return 0;
   return 1;
}

/* Read one query param (no percent-decoding: our ids/names need none). Returns 1 if present. */
static int qparam(const char *qs, const char *key, char *out, size_t cap)
{
   if (cap == 0)
      return 0;
   out[0] = '\0';
   if (!qs)
      return 0;
   size_t klen = strlen(key);
   const char *p = qs;
   while (p && *p)
   {
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         const char *v = p + klen + 1;
         const char *amp = strchr(v, '&');
         size_t n = amp ? (size_t)(amp - v) : strlen(v);
         if (n >= cap)
            n = cap - 1;
         memcpy(out, v, n);
         out[n] = '\0';
         return 1;
      }
      p = strchr(p, '&');
      if (p)
         p++;
   }
   return 0;
}

/* Open an admin/bootstrap tenant scope for the current actor (team 0 = principal only).
 * Returns 0 on success, or writes an HTTP status (>0) into *http_out. */
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

/* POST /v1/rate/policy {dim, scope, window_seconds, max_count} -> upsert a policy
 * (admin-gated at the DB layer, WORM-audited). */
static int handle_policy(const char *method, const char *body, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jdim = b ? cJSON_GetObjectItemCaseSensitive(b, "dim") : NULL;
   cJSON *jscope = b ? cJSON_GetObjectItemCaseSensitive(b, "scope") : NULL;
   cJSON *jwin = b ? cJSON_GetObjectItemCaseSensitive(b, "window_seconds") : NULL;
   cJSON *jmax = b ? cJSON_GetObjectItemCaseSensitive(b, "max_count") : NULL;

   if (!cJSON_IsString(jdim) || !cJSON_IsString(jscope) || !cJSON_IsNumber(jwin) ||
       !cJSON_IsNumber(jmax))
   {
      cJSON_Delete(b);
      return err(
          out, cap, 400,
          "dim (string), scope (string), window_seconds (number), max_count (number) required");
   }
   char dim[16];
   snprintf(dim, sizeof(dim), "%s", jdim->valuestring);
   if (!dim_valid(dim))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "dim must be team|project|cert|model|cred_slot");
   }
   char scope[128];
   snprintf(scope, sizeof(scope), "%s", jscope->valuestring);
   if (!scope_valid(scope))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "scope must be a non-empty token (use \"*\" for the dim default)");
   }
   double win_d = jwin->valuedouble;
   if (!(win_d >= 1.0) || win_d >= 9223372036854775808.0 || win_d != (double)(int64_t)win_d)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "window_seconds must be a positive integer");
   }
   double max_d = jmax->valuedouble;
   if (!(max_d >= 0.0) || max_d >= 9223372036854775808.0 || max_d != (double)(int64_t)max_d)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "max_count must be a non-negative integer");
   }
   int64_t window_seconds = (int64_t)win_d;
   int64_t max_count = (int64_t)max_d;
   cJSON_Delete(b);

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int64_t id = 0;
   int rc = db2_org_rate_policy_set(dim, scope, window_seconds, max_count, &id);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_RATE_ERR_DENIED)
         return err(out, cap, 403, "not authorized to set rate policy (org-admin required)");
      return err(out, cap, 500, "rate policy set failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)id);
   cJSON_AddStringToObject(o, "dim", dim);
   cJSON_AddStringToObject(o, "scope", scope);
   cJSON_AddNumberToObject(o, "window_seconds", (double)window_seconds);
   cJSON_AddNumberToObject(o, "max_count", (double)max_count);
   return emit(o, out, cap, 200);
}

/* GET /v1/rate/show?dim=&scope= -> the (dim, scope) policy (0 or 1 row). */
static int handle_show(const char *method, const char *qs, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return err(out, cap, 405, "method not allowed");
   char dim[16], scope[128];
   int has_dim = qparam(qs, "dim", dim, sizeof(dim));
   int has_scope = qparam(qs, "scope", scope, sizeof(scope));
   if (!has_dim || !dim_valid(dim))
      return err(out, cap, 400, "dim (team|project|cert|model|cred_slot) is required");
   if (!has_scope || !scope_valid(scope))
      return err(out, cap, 400, "scope (non-empty token) is required");

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   db2_org_rate_policy_t rows[DB2_RATE_MAX_ROWS];
   int n = db2_org_rate_policy_show(dim, scope, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
   {
      db2_tenant_scope_rollback();
      if (n == DB2_RATE_ERR_DENIED)
         return err(out, cap, 403, "not authorized (org-admin or team-lead required)");
      return err(out, cap, 500, "rate show failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dim", dim);
   cJSON_AddStringToObject(root, "scope", scope);
   cJSON *arr = cJSON_AddArrayToObject(root, "policies");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", (double)rows[i].id);
      cJSON_AddStringToObject(o, "dim", rows[i].dim);
      cJSON_AddStringToObject(o, "scope", rows[i].scope_key);
      cJSON_AddNumberToObject(o, "window_seconds", (double)rows[i].window_seconds);
      cJSON_AddNumberToObject(o, "max_count", (double)rows[i].max_count);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out, cap, 200);
}

int kb_http_rate_route(const char *method, const char *path, const char *query_string,
                       const char *body, char *out_buf, int out_cap)
{
   if (!path)
      return -1;
   if (strcmp(path, "/v1/rate/policy") == 0)
      return handle_policy(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/rate/show") == 0)
      return handle_show(method, query_string, out_buf, out_cap);
   return -1; /* not ours */
}
