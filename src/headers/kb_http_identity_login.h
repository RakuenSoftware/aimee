/* kb_http_identity_login.h — the kb-side login surface for per-user /v1 write
 * authorization (proposal per-user-remote-writes-authz.md §3).
 *
 * These routes are UNAUTHENTICATED by necessity: they are how somebody who has
 * no credential yet obtains one. That makes them the one kb surface where the
 * usual "the ACL already authorized this" assumption does not hold, so each one
 * states what it protects instead.
 *
 * The declaration route exists because the wizard's flow FORKS on it: §3 has kb
 * declare its auth mode and the server behave differently for OIDC and PAM. A
 * client that guessed would either send a password to a kb that has no PAM
 * mediator or wait for a redirect that never comes.
 *
 * There is deliberately no 302 here. kb's route convention returns a status and
 * a JSON body with no header control, and the proposal's cited precedent is the
 * server's own `git_oauth_github_web_start -> {authorize_url, redirect_uri}`
 * pair — the caller navigates. Adding redirect support to the shared kb response
 * emitter for one route would be a wider change than this surface justifies.
 */
#ifndef KB_HTTP_IDENTITY_LOGIN_H
#define KB_HTTP_IDENTITY_LOGIN_H

#include <stdint.h>

/* Handle an identity-login route. Returns the HTTP status if (method, path) is
 * one of them, or -1 if it is not — the same convention as the other kb route
 * satellites.
 *
 * `now` is the current unix time, passed in rather than read, so the pending
 * login's lifetime is testable and there is no hidden clock in the request path.
 *
 * Routes:
 *   GET  /v1/identity/auth-mode      -> {"mode":"oidc"|"pam"|"none"}
 *   POST /v1/identity/login/start    -> {"authorize_url":...,"redirect_uri":...}
 *                                       body: {"server_id":"..."}
 *   GET  /v1/identity/login/callback -> {"subject":...,"server_id":...}
 *                                       query: code=&state= (or error=)
 *   POST /v1/identity/login/pam      -> {"subject":...,"server_id":...}
 *                                       body: {"username","password","server_id"}
 *                                       409 when this kb is in OIDC mode
 *
 * `query_string` is the request's raw query (may be NULL). The callback route
 * needs it; the others ignore it. It is passed SEPARATELY from the path because
 * the dispatcher matches on the path alone, and folding the query into it would
 * make every route's strcmp depend on parameters it does not read.
 */
int kb_http_identity_login_route(const char *method, const char *path, const char *query_string,
                                 const char *body, int64_t now, char *out_buf, int out_cap);

#endif /* KB_HTTP_IDENTITY_LOGIN_H */
