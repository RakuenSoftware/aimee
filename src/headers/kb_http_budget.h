/* kb_http_budget.h: /v1/budget routes (P4a budget reservation core).
 *
 * POST /v1/budget/set                 — admin: set a team[/project] period cap (WORM-audited).
 * GET  /v1/budget/show?team=&project=  — admin OR team-lead: caps + current-period counters.
 *
 * The authenticated actor comes from kb_reqctx; every op runs inside a tenant scope
 * (aimee.principal) so authorization is enforced at the DB layer inside the SECURITY
 * DEFINER org_budget_set / org_budget_show — a non-admin set surfaces here as 403, a
 * non-admin/non-lead show as 403. BUDGET ONLY (the rate limiter is deferred to P4b);
 * the reserve/settle egress wiring is P2b. Money is a NUMERIC string (never a float). */
#ifndef DEC_KB_HTTP_BUDGET_H
#define DEC_KB_HTTP_BUDGET_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle /v1/budget* routes. Returns an HTTP status (>=0) when it handled the path,
    * or -1 when the path is not one of ours (router falls through). Writes the JSON
    * response into out_buf[out_cap]. */
   int kb_http_budget_route(const char *method, const char *path, const char *query_string,
                            const char *body, char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_BUDGET_H */
