/* kb_http_budget.c: /v1/budget routes (P4a budget reservation core).
 *
 * The authenticated actor comes from kb_reqctx (set by the router after verification);
 * every op runs inside a tenant scope (db2_tenant_scope_begin sets aimee.principal), so
 * the admin gate for POST /v1/budget/set and the admin-OR-team-lead gate for GET
 * /v1/budget/show are enforced at the DB layer inside the SECURITY DEFINER functions —
 * a non-authorized caller surfaces here as 403. Money (limit/soft/spend/reserved) is a
 * NUMERIC string end-to-end (never a float), so a hard cap is exact. BUDGET ONLY: the
 * rate limiter is deferred to P4b; the reserve/settle egress wiring is P2b. */

#include "kb_http_budget.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_reqctx.h"
#include "org_budget.h"

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

/* Validate a non-negative decimal money string: 1..40 chars, digits + at most one '.',
 * at least one digit, no sign/exponent/whitespace. NUMERIC(20,10) is the DB CHECK; this
 * is the boundary shape check (the DB is authoritative on scale/precision). */
static int money_valid(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t len = strlen(s);
   if (len > 40)
      return 0;
   int dots = 0, digits = 0;
   for (const char *p = s; *p; ++p)
   {
      if (*p == '.')
      {
         if (++dots > 1)
            return 0;
      }
      else if (*p >= '0' && *p <= '9')
         digits++;
      else
         return 0;
   }
   return digits > 0;
}

/* Read one query param (no percent-decoding: our ids need none). Returns 1 if present. */
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

/* Parse a positive int64 from a decimal string (no sign, no junk). */
static int parse_pos_int64(const char *s, int64_t *out)
{
   if (!s || !s[0])
      return 0;
   int64_t v = 0;
   for (const char *p = s; *p; ++p)
   {
      if (*p < '0' || *p > '9')
         return 0;
      if (v > (INT64_MAX - (*p - '0')) / 10)
         return 0;
      v = v * 10 + (*p - '0');
   }
   if (v <= 0)
      return 0;
   *out = v;
   return 1;
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

/* POST /v1/budget/set {team, period, limit_usd[, project, soft_limit_usd]} -> upsert a
 * cap (admin-gated at the DB layer, WORM-audited). */
static int handle_set(const char *method, const char *body, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jteam = b ? cJSON_GetObjectItemCaseSensitive(b, "team") : NULL;
   cJSON *jperiod = b ? cJSON_GetObjectItemCaseSensitive(b, "period") : NULL;
   cJSON *jlimit = b ? cJSON_GetObjectItemCaseSensitive(b, "limit_usd") : NULL;
   cJSON *jproject = b ? cJSON_GetObjectItemCaseSensitive(b, "project") : NULL;
   cJSON *jsoft = b ? cJSON_GetObjectItemCaseSensitive(b, "soft_limit_usd") : NULL;

   if (!cJSON_IsNumber(jteam) || !cJSON_IsString(jperiod) || !cJSON_IsString(jlimit))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "team (number), period (day|month), limit_usd (string) required");
   }
   double team_d = jteam->valuedouble;
   if (!(team_d >= 1.0) || team_d >= 9223372036854775808.0 || team_d != (double)(int64_t)team_d)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "team must be a positive integer");
   }
   int64_t team = (int64_t)team_d;
   char period[8];
   snprintf(period, sizeof(period), "%s", jperiod->valuestring);
   if (strcmp(period, "day") != 0 && strcmp(period, "month") != 0)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "period must be day or month");
   }
   char limit_usd[48];
   snprintf(limit_usd, sizeof(limit_usd), "%s", jlimit->valuestring);
   if (!money_valid(limit_usd))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "limit_usd must be a non-negative decimal string");
   }
   int has_project = 0;
   int64_t project = 0;
   if (jproject && cJSON_IsNumber(jproject))
   {
      double p_d = jproject->valuedouble;
      if (!(p_d >= 1.0) || p_d >= 9223372036854775808.0 || p_d != (double)(int64_t)p_d)
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "project must be a positive integer");
      }
      has_project = 1;
      project = (int64_t)p_d;
   }
   char soft_usd[48] = "";
   if (jsoft && cJSON_IsString(jsoft) && jsoft->valuestring[0])
   {
      snprintf(soft_usd, sizeof(soft_usd), "%s", jsoft->valuestring);
      if (!money_valid(soft_usd))
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "soft_limit_usd must be a non-negative decimal string");
      }
   }
   cJSON_Delete(b);

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int64_t id = 0;
   int rc = db2_org_budget_set(team, has_project, project, period, limit_usd,
                               soft_usd[0] ? soft_usd : NULL, &id);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_BUDGET_ERR_DENIED)
         return err(out, cap, 403, "not authorized to set budgets (org-admin required)");
      if (rc == DB2_BUDGET_ERR_RETRO)
         return err(
             out, cap, 409,
             "retroactive reduction: limit below the current period's committed spend+reserved");
      return err(out, cap, 500, "budget set failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)id);
   cJSON_AddNumberToObject(o, "team", (double)team);
   if (has_project)
      cJSON_AddNumberToObject(o, "project", (double)project);
   cJSON_AddStringToObject(o, "period", period);
   return emit(o, out, cap, 200);
}

/* GET /v1/budget/show?team=&project= -> the team's caps + current-period counters. */
static int handle_show(const char *method, const char *qs, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return err(out, cap, 405, "method not allowed");
   char team_s[32], project_s[32];
   int has_team = qparam(qs, "team", team_s, sizeof(team_s));
   int has_project = qparam(qs, "project", project_s, sizeof(project_s));
   int64_t team = 0, project = 0;
   if (!has_team || !parse_pos_int64(team_s, &team))
      return err(out, cap, 400, "team (positive integer) is required");
   if (has_project && !parse_pos_int64(project_s, &project))
      return err(out, cap, 400, "project must be a positive integer");

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   db2_org_budget_row_t rows[DB2_BUDGET_MAX_ROWS];
   int n =
       db2_org_budget_show(team, has_project, project, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
   {
      db2_tenant_scope_rollback();
      if (n == DB2_BUDGET_ERR_DENIED)
         return err(out, cap, 403, "not authorized (org-admin or team-lead required)");
      return err(out, cap, 500, "budget show failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");

   cJSON *root = cJSON_CreateObject();
   cJSON_AddNumberToObject(root, "team", (double)team);
   if (has_project)
      cJSON_AddNumberToObject(root, "project", (double)project);
   cJSON *arr = cJSON_AddArrayToObject(root, "budgets");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "team_id", (double)rows[i].team_id);
      if (rows[i].has_project)
         cJSON_AddNumberToObject(o, "project_id", (double)rows[i].project_id);
      else
         cJSON_AddNullToObject(o, "project_id");
      cJSON_AddStringToObject(o, "period", rows[i].period);
      cJSON_AddStringToObject(o, "period_id", rows[i].period_id);
      cJSON_AddStringToObject(o, "limit_usd", rows[i].limit_usd);
      if (rows[i].soft_limit_usd[0])
         cJSON_AddStringToObject(o, "soft_limit_usd", rows[i].soft_limit_usd);
      else
         cJSON_AddNullToObject(o, "soft_limit_usd");
      cJSON_AddStringToObject(o, "spend_usd", rows[i].spend_usd);
      cJSON_AddStringToObject(o, "reserved_usd", rows[i].reserved_usd);
      cJSON_AddStringToObject(o, "remaining_usd", rows[i].remaining_usd);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out, cap, 200);
}

int kb_http_budget_route(const char *method, const char *path, const char *query_string,
                         const char *body, char *out_buf, int out_cap)
{
   if (!path)
      return -1;
   if (strcmp(path, "/v1/budget/set") == 0)
      return handle_set(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/budget/show") == 0)
      return handle_show(method, query_string, out_buf, out_cap);
   return -1; /* not ours */
}
