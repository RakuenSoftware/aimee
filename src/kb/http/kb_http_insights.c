/* kb_http_insights.c: /v1/insights/spend route (P3b org spend reporting).
 *
 * The authenticated actor comes from kb_reqctx (set by the router after verification);
 * the read runs inside a tenant scope (db2_tenant_scope_begin sets aimee.principal), so
 * the SECURITY DEFINER org_spend_query() evaluates its admin/lead predicate against the
 * verified actor — a caller can never read a team they don't lead, and the org-wide
 * (team-absent) branch is admin-only, all enforced at the DB layer. The boundary
 * pre-validates team/project/date (400 on malformed) BEFORE the definer call; the
 * definer re-validates (defense in depth). Read-only; cost_usd is a NUMERIC string. */

#include "kb_http_insights.h"

#include "modules/db2/c/db2_tenant.h"
#include "kb_insights_util.h"
#include "kb_reqctx.h"
#include "org_spend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Read one query param (no percent-decoding: our ids/ISO dates need none). Returns 1 if
 * present (value copied into out), 0 if absent (out set to ""). */
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

/* Parse a positive int64 from a decimal string (no sign, no junk). Returns 1 on success
 * with *out set; 0 on empty/negative/overflow/non-digit. */
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
         return 0; /* overflow */
      v = v * 10 + (*p - '0');
   }
   if (v <= 0)
      return 0;
   *out = v;
   return 1;
}

/* GET /v1/insights/spend?team=&project=&since=&until= */
static int handle_spend(const char *method, const char *qs, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return err(out, cap, 405, "method not allowed");

   char team_s[32], project_s[32], since[32], until[32];
   int has_team = qparam(qs, "team", team_s, sizeof(team_s));
   int has_project = qparam(qs, "project", project_s, sizeof(project_s));
   int has_since = qparam(qs, "since", since, sizeof(since));
   int has_until = qparam(qs, "until", until, sizeof(until));

   /* since/until are required and must be well-formed ISO calendar dates. Reject at the
    * boundary (400) before the definer call; the definer re-validates too. */
   if (!has_since || !has_until)
      return err(out, cap, 400, "since and until (YYYY-MM-DD) are required");
   if (!kb_insights_date_valid(since) || !kb_insights_date_valid(until))
      return err(out, cap, 400, "since/until must be valid YYYY-MM-DD dates");
   if (strcmp(since, until) > 0)
      return err(out, cap, 400, "since must be <= until");

   int64_t team = 0, project = 0;
   if (has_team && !parse_pos_int64(team_s, &team))
      return err(out, cap, 400, "team must be a positive integer");
   if (has_project && !parse_pos_int64(project_s, &project))
      return err(out, cap, 400, "project must be a positive integer");

   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
      return err(out, cap, 401, "authentication required");
   int rc = db2_tenant_scope_begin(actor, 0);
   if (rc != 0)
      return err(out, cap, tenant_http_status(rc), "tenant scope failed");

   db2_org_spend_row_t rows[DB2_SPEND_MAX_ROWS];
   int n = db2_org_spend_query(has_team, team, has_project, project, since, until, rows,
                               (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
   {
      /* A denied/bad-date definer RAISE aborts the txn; a TOOBIG is detected client-side
       * (no RAISE) but the read is complete either way — roll back rather than commit. */
      db2_tenant_scope_rollback();
      if (n == DB2_SPEND_ERR_DENIED)
         return err(out, cap, 403, "not authorized (org-admin or team-lead required)");
      if (n == DB2_SPEND_ERR_BADDATE)
         return err(out, cap, 400, "invalid date range");
      if (n == DB2_SPEND_ERR_TOOBIG)
         return err(out, cap, 413, "report too large; narrow the team/project/date range");
      return err(out, cap, 500, "spend query failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");

   char *json = kb_insights_spend_json(has_team, (long long)team, has_project, (long long)project,
                                       since, until, rows, n);
   if (!json)
      return err(out, cap, 500, "response build failed");
   if ((int)strlen(json) >= cap)
   {
      free(json);
      return err(out, cap, 500, "response too large");
   }
   snprintf(out, (size_t)cap, "%s", json);
   free(json);
   return 200;
}

int kb_http_insights_route(const char *method, const char *path, const char *query_string,
                           char *out_buf, int out_cap)
{
   if (!path)
      return -1;
   if (strcmp(path, "/v1/insights/spend") == 0)
      return handle_spend(method, query_string, out_buf, out_cap);
   return -1; /* not ours */
}
