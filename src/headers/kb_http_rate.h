/* kb_http_rate.h: /v1/rate routes (P4b keyed fixed-window rate limiter).
 *
 * POST /v1/rate/policy               — admin: set a (dim, scope) window_seconds/max_count.
 * GET  /v1/rate/show?dim=&scope=      — admin OR team-lead: the (dim, scope) policy.
 *
 * The authenticated actor comes from kb_reqctx; every op runs inside a tenant scope
 * (aimee.principal) so authorization is enforced at the DB layer inside the SECURITY
 * DEFINER org_rate_policy_set / org_rate_policy_show — a non-admin set surfaces here as
 * 403, a non-admin/non-lead show as 403. RATE ONLY (org_rate_check is the P2b egress
 * primitive, not routed here); the budget core is P4a. */
#ifndef DEC_KB_HTTP_RATE_H
#define DEC_KB_HTTP_RATE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle /v1/rate* routes. Returns an HTTP status (>=0) when it handled the path,
    * or -1 when the path is not one of ours (router falls through). Writes the JSON
    * response into out_buf[out_cap]. */
   int kb_http_rate_route(const char *method, const char *path, const char *query_string,
                          const char *body, char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_RATE_H */
