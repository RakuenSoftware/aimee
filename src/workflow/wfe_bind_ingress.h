/* wfe_bind_ingress.h -- S2 binding-creation seam (the live unblock).
 *
 * The dispatch guard + advance_request driver (sub-slices 3/4) key on the aimee
 * session id, but the live primary's chat turn reaches the /v1/messages gateway
 * with NO session id (gw_request_t carries none). This module closes that gap: the
 * client encodes the session id in the primary provider's auth token
 * ("aimee-sess-<sid>"), the server extracts it at HTTP ingress, and on a routed
 * ENFORCED turn creates + binds a work-item so enforcement can fire.
 *
 * PURE where it can be: the token parser has no deps; the bind path composes the
 * existing router + work-item + binding APIs behind the enforcement dial.
 * Default-OFF: with the dial unset nothing binds. */
#ifndef DEC_WFE_BIND_INGRESS_H
#define DEC_WFE_BIND_INGRESS_H 1

#include <stddef.h>

/* The auth-token identity prefix the client stamps on the primary provider so the
 * gateway can recover the aimee session id from an otherwise-opaque request. */
#define WFE_SESSION_TOKEN_PREFIX "aimee-sess-"

/* Extract the aimee session id from a client auth value of the form
 * "aimee-sess-<sid>" (a leading "Bearer " is tolerated). The recovered sid must be
 * id-charset ([A-Za-z0-9_-], the mint format). Returns 1 and fills `out` on a
 * match, 0 otherwise (not an aimee-session token / bad charset / overflow). Pure. */
int wfe_session_id_from_auth(const char *auth_value, char *out, size_t n);

/* On an interactive primary turn, ensure `session_id` is bound to a work-item for
 * the workflow the router picks for `message` -- but ONLY if the routed workflow is
 * enforced AND the enforcement dial is on. Idempotent per session: if already
 * bound, returns 1 without creating anything (the work-item's UNIQUE(repo,
 * proposal_path)=interactive/<sid> is a second backstop). `repo` may be NULL/empty.
 * Returns 1 if the session is now bound (pre-existing or freshly created), 0 if not
 * (unrouted / non-enforced / dial off / empty message / error). Default-OFF. */
int wfe_bind_interactive(const char *session_id, const char *message, const char *repo);

#endif /* DEC_WFE_BIND_INGRESS_H */
