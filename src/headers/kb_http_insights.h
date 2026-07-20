/* kb_http_insights.h: /v1/insights routes (P3b org spend reporting).
 *
 * GET /v1/insights/spend?team=&project=&since=&until=  — authorized org spend, grouped
 * per model + per project, with a reconciling total. The authenticated actor comes from
 * kb_reqctx; authorization (org-admin OR team-lead) is enforced at the DB layer inside
 * the SECURITY DEFINER org_spend_query() — a caller who is neither admin nor a lead of
 * the requested team receives 403. team absent -> the org-wide (admin-only) branch.
 * Read-only: no writes, no ledger, cost_usd is a NUMERIC string (never a float). */
#ifndef DEC_KB_HTTP_INSIGHTS_H
#define DEC_KB_HTTP_INSIGHTS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle /v1/insights* routes. Returns an HTTP status (>=0) when it handled the path,
    * or -1 when the path is not one of ours (router falls through). Writes the JSON
    * response into out_buf[out_cap]. */
   int kb_http_insights_route(const char *method, const char *path, const char *query_string,
                              char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_INSIGHTS_H */
