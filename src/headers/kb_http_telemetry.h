/* kb_http_telemetry.h: /v1/metrics + /v1/telemetry routes (P9a telemetry export).
 *
 * GET  /v1/metrics                 — Prometheus text of the org aggregates (auth:
 *                                    org-admin OR the scrape token). No PII.
 * POST /v1/telemetry/metrics       — content-free allowlist-gated ingest (auth:
 *                                    org-admin OR the ingest token). Drop-on-unknown.
 * POST /v1/telemetry/allow         — admin: upsert an allowlist entry (WORM-audited).
 * GET  /v1/telemetry/allow         — admin: show the allowlist.
 *
 * TWO auth boundaries. The scrape/ingest TOKEN (config telemetry.metrics_token,
 * stored as a SHA-256 hex, compared constant-time) authorizes /v1/metrics +
 * /v1/telemetry/metrics WITHOUT the normal kb bearer, so it is checked BEFORE the
 * bearer gate (kb_http_telemetry_token_route); a valid token opens an owner scope.
 * Everything else (and the token-less path) is org-admin, enforced at the DB layer
 * inside the SECURITY DEFINER functions (non-admin -> 403). Admin-managed allowlist
 * mutation only. The token is never echoed to the body or logs. */
#ifndef DEC_KB_HTTP_TELEMETRY_H
#define DEC_KB_HTTP_TELEMETRY_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Set the /v1/metrics + /v1/telemetry/metrics scrape/ingest token, stored as a
    * SHA-256 hex (config telemetry.metrics_token). Empty/NULL disables the token
    * path (those routes are then org-admin only). Call once before the listener. */
   void kb_http_set_telemetry_token(const char *hash);

   /* Gate scrape/export and metric-ingest routes. Disabled is the process
    * default; the KB entry point enables them only for an explicit listener. */
   void kb_http_set_telemetry_enabled(int enabled);

   /* Render one scrape for the dedicated observability listener. trusted_transport
    * means an owner-only Unix socket, a loopback bind, or verified mTLS. When
    * require_bearer is set, the configured bearer is required in addition to that
    * transport. Returns an HTTP status and writes a Prometheus body or JSON error. */
   int kb_http_telemetry_scrape(const char *presented, int trusted_transport, int require_bearer,
                                char *out_buf, int out_cap);

   /* Pre-bearer-gate scrape/ingest TOKEN path for GET /v1/metrics + POST
    * /v1/telemetry/metrics. presented is the raw bearer the caller sent (may be
    * NULL/empty); the expected hash is the module's configured token (set via
    * kb_http_set_telemetry_token). Returns an HTTP status (>=0) when the token
    * matched and it served the request, or -1 to FALL THROUGH to the normal
    * bearer/admin path (token absent, mismatched, or a non-token-eligible path). */
   int kb_http_telemetry_token_route(const char *method, const char *path, const char *query_string,
                                     const char *body, const char *presented, char *out_buf,
                                     int out_cap);

   /* Post-bearer-gate ORG-ADMIN path for all four telemetry routes (the actor set
    * by the router is the authenticated principal; the DB definer enforces the
    * admin gate). Returns an HTTP status (>=0) when it handled the path, or -1
    * when the path is not one of ours (router falls through). */
   int kb_http_telemetry_route(const char *method, const char *path, const char *query_string,
                               const char *body, char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_TELEMETRY_H */
