/* kb_http.h: aimee-kb public HTTP/1.1 API server (Phase 1+5).
 *
 * Starts a background thread that accepts TCP connections on kb_api_http_port
 * and serves /v1/health, /v1/version, /v1/capabilities, and the Phase 5
 * retrieval endpoints (search, artifacts, code, entities).
 * Port 0 or unconfigured disables the server (no-op start/stop).
 *
 * Auth: when kb_api_bearer_token is non-empty every request must carry
 * Authorization: Bearer <token>; unauthenticated requests get 401. */
#pragma once

/* Start the HTTP listener thread on the given port.
 * bearer_token may be NULL or empty to disable auth.
 * Returns 0 on success, -1 on error. */
int kb_http_start(int port, const char *bearer_token);

/* Signal the listener thread to stop and wait for it to exit. */
void kb_http_stop(void);

/* Route a single HTTP request and write the response into out_buf (null-
 * terminated). Returns the HTTP status code.
 * method, path, auth_header are null-terminated strings (auth_header may
 * be NULL). out_buf must be at least out_cap bytes. */
int kb_http_route(const char *method, const char *path, const char *auth_header,
                  const char *bearer_token, char *out_buf, int out_cap);

/* Extended routing for Phase 5 endpoints: adds query_string and body for
 * POST endpoints and path-parameter routes.
 * query_string and body may be NULL. out_buf must be at least out_cap bytes. */
int kb_http_route_ex(const char *method, const char *path, const char *query_string,
                     const char *auth_header, const char *bearer_token, const char *body,
                     int body_len, char *out_buf, int out_cap);
