/* wfe_bind_ingress.h -- S2 binding-creation seam (the live unblock).
 *
 * The dispatch guard + advance_request driver (sub-slices 3/4) key on the aimee
 * session id, but nothing created a session->work-item binding, so enforcement was
 * dormant. This module closes that gap: on a routed ENFORCED primary turn it
 * creates + binds a work-item, so the guard + driver can fire.
 *
 * The session id reaches the gateway router via the per-turn thread-local
 * ingress_preinject_session_id(), which the AUTHORITATIVE in-process primary turn
 * publishes beside its existing session_id override (server_compute /
 * primary_session_adapter). An additional client-stamped auth-token channel for the
 * external-CLI /v1/messages proxy path is deliberately deferred: it needs an
 * AUTHENTICATED per-session identity (an unauthenticated "aimee-sess-<sid>" token
 * would let any caller bind an arbitrary session), so it is a separate follow-on.
 *
 * Default-OFF: with the enforcement dial unset nothing binds. */
#ifndef DEC_WFE_BIND_INGRESS_H
#define DEC_WFE_BIND_INGRESS_H 1

/* On an interactive primary turn, ensure `session_id` is bound to a work-item for
 * the workflow the router picks for `message` -- but ONLY if the routed workflow is
 * enforced AND the enforcement dial is on. Idempotent per session: if already
 * bound, returns 1 without creating anything (the work-item's UNIQUE(repo,
 * proposal_path)=interactive/<sid> is a second backstop). `repo` may be NULL/empty.
 * Returns 1 if the session is now bound (pre-existing or freshly created), 0 if not
 * (unrouted / non-enforced / dial off / empty message / error). Default-OFF. */
int wfe_bind_interactive(const char *session_id, const char *message, const char *repo);

#endif /* DEC_WFE_BIND_INGRESS_H */
