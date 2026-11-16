/* kb_http_insights.c: /v1/insights/spend route (P3b org spend reporting).
 *
 * The authenticated actor comes from kb_reqctx (set by the router after verification);
 * the read runs inside a tenant scope (kb_store_tenant_scope_begin sets aimee.principal), so
 * the SECURITY DEFINER org_spend_query() evaluates its admin/lead predicate against the
 * verified actor — a caller can never read a team they don't lead, and the org-wide
 * (team-absent) branch is admin-only, all enforced at the DB layer. The boundary
 * pre-validates team/project/date (400 on malformed) BEFORE the definer call; the
 * definer re-validates (defense in depth). Read-only; cost_usd is a NUMERIC string. */

#include "kb_http_insights.h"
#include "kb_http_common.h"

#include "modules/kb/c/kb_store_tenant.h"
#include "kb_insights_util.h"
#include "kb_reqctx.h"
#include "org_spend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GET /v1/insights/spend?team=&project=&since=&until= */
static int handle_spend(const char *method, const char *qs, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return kb_http_error(out, cap, 405, "method not allowed");

   char team_s[32], project_s[32], since[32], until[32];
   int has_team = kb_http_query_param(qs, "team", team_s, sizeof(team_s));
   int has_project = kb_http_query_param(qs, "project", project_s, sizeof(project_s));
   int has_since = kb_http_query_param(qs, "since", since, sizeof(since));
   int has_until = kb_http_query_param(qs, "until", until, sizeof(until));

   /* since/until are required and must be well-formed ISO calendar dates. Reject at the
    * boundary (400) before the definer call; the definer re-validates too. */
   if (!has_since || !has_until)
      return kb_http_error(out, cap, 400, "since and until (YYYY-MM-DD) are required");
   if (!kb_insights_date_valid(since) || !kb_insights_date_valid(until))
      return kb_http_error(out, cap, 400, "since/until must be valid YYYY-MM-DD dates");
   if (strcmp(since, until) > 0)
      return kb_http_error(out, cap, 400, "since must be <= until");

   int64_t team = 0, project = 0;
   if (has_team && !kb_http_parse_positive_int64(team_s, &team))
      return kb_http_error(out, cap, 400, "team must be a positive integer");
   if (has_project && !kb_http_parse_positive_int64(project_s, &project))
      return kb_http_error(out, cap, 400, "project must be a positive integer");

   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
      return kb_http_error(out, cap, 401, "authentication required");
   int rc = kb_store_tenant_scope_begin(actor, 0);
   if (rc != 0)
      return kb_http_error(out, cap, kb_http_tenant_status(rc), "tenant scope failed");

   kb_store_org_spend_row_t rows[KB_STORE_SPEND_MAX_ROWS];
   int n = kb_store_org_spend_query(has_team, team, has_project, project, since, until, rows,
                                    (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
   {
      /* A denied/bad-date definer RAISE aborts the txn; a TOOBIG is detected client-side
       * (no RAISE) but the read is complete either way — roll back rather than commit. */
      kb_store_tenant_scope_rollback();
      if (n == KB_STORE_SPEND_ERR_DENIED)
         return kb_http_error(out, cap, 403, "not authorized (org-admin or team-lead required)");
      if (n == KB_STORE_SPEND_ERR_BADDATE)
         return kb_http_error(out, cap, 400, "invalid date range");
      if (n == KB_STORE_SPEND_ERR_TOOBIG)
         return kb_http_error(out, cap, 413,
                              "report too large; narrow the team/project/date range");
      return kb_http_error(out, cap, 500, "spend query failed");
   }
   if (kb_store_tenant_scope_commit() != 0)
      return kb_http_error(out, cap, 500, "commit failed");

   char *json = kb_insights_spend_json(has_team, (long long)team, has_project, (long long)project,
                                       since, until, rows, n);
   if (!json)
      return kb_http_error(out, cap, 500, "response build failed");
   if ((int)strlen(json) >= cap)
   {
      free(json);
      return kb_http_error(out, cap, 500, "response too large");
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
