/* kb_http_bootstrap.h — kb's PRE-AUTH surface: every route reachable without a
 * credential, in one place.
 *
 * Split out of kb_http.c when that file hit its 2500-line limit, but the
 * grouping is the point rather than the line count. These routes run BEFORE the
 * bearer gate in kb_http_route_ex, which makes them the ones an unauthenticated
 * stranger can reach — so having them scattered through a 2500-line router meant
 * nobody could see the whole attack surface at once. Now it is one file.
 *
 * What is here, and why each one cannot require a credential:
 *
 *   GET  /v1/identity/auth-mode    — a client must know which login flow to
 *     start before it can log in at all (proposal §3).
 *   POST /v1/identity/login/start  — this is how a per-user write token is
 *     obtained; requiring one would be circular.
 *   POST /v1/enroll/redeem         — the single-use enrollment token IS the
 *     credential. The caller supplies a CSR; its private key never leaves it.
 *
 * Anything added here is unauthenticated by construction. That is the bar for
 * belonging in this file.
 */
#ifndef KB_HTTP_BOOTSTRAP_H
#define KB_HTTP_BOOTSTRAP_H

#include <stdint.h>

/* Handle a pre-auth route. Returns the HTTP status if (method, path) is one of
 * them, or -1 if it is not — the same convention as the other kb route
 * satellites, and the reason a miss falls through to the bearer gate.
 *
 * `now` is passed in rather than read so the pending-login lifetime is testable
 * and there is no hidden clock in the request path. */
int kb_http_bootstrap_route(const char *method, const char *path, const char *query_string,
                            const char *body, int64_t now, char *out_buf, int out_cap);

#endif /* KB_HTTP_BOOTSTRAP_H */
