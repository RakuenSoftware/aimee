/* web_egress.h -- one guarded path for every outbound web fetch.
 *
 * WHY THIS EXISTS
 *
 * The page reader resolves a host once, validates the resolved address against a
 * private/reserved deny-list, pins the connection to that address so a rebinding
 * resolver cannot swap in a private one between check and connect, and refuses
 * redirects. Search had none of that: it called agent_http_get directly.
 *
 * That was survivable only while search returned engine snippets, because its
 * endpoints are compile-time constants. The moment search fetches the pages it
 * finds, the destinations come from a third-party ranking and the gap becomes a
 * straightforward SSRF. This module exists so there is one path, and adding a
 * caller cannot accidentally miss the guard.
 *
 * TWO POLICIES, BECAUSE THERE ARE TWO KINDS OF DESTINATION
 *
 * Applying the page reader's deny-list to everything would break a legitimate
 * self-hosted SearXNG on a LAN address. Applying nothing to configured endpoints
 * would leave a hole: config.set is capability-gated (CAP_SESSION_ADMIN), but an
 * admin-capable session that pointed the search backend at a cloud metadata
 * address would exfiltrate instance credentials through a tool that looks like
 * search. So configured endpoints are validated too, and permitting a private
 * one is a DEPLOY-TIME decision (an environment variable), not something
 * reachable through the config surface an agent can touch.
 *
 * PLATFORM
 *
 * The pinned-connect primitive (agent_http_get_pinned) is posix-only, and the
 * server runs on posix. The Windows thin client keeps the unguarded path for the
 * two compile-time-constant endpoints and REFUSES operator-configured ones,
 * which is the destination that actually carries risk. That is stated rather
 * than silently varying by platform. */
#ifndef DEC_WEB_EGRESS_H
#define DEC_WEB_EGRESS_H 1

#include <stddef.h>

typedef enum
{
   /* A destination chosen by a model, a page, or a third-party ranking. Deny
    * private/reserved addresses, pin the connection, refuse redirects. */
   WEB_EGRESS_UNTRUSTED = 0,

   /* An endpoint the operator configured (self-hosted SearXNG). Same validation,
    * except a private address is permitted when the deployment has explicitly
    * opted in via AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT=1. Default is deny. */
   WEB_EGRESS_CONFIGURED = 1,
} web_egress_policy_t;

/* Fetch `url` under `policy`.
 *
 * Resolves once, validates the resolved address, connects to THAT address (no
 * re-resolution, so there is no window for a rebinding answer), and treats a 3xx
 * as an error rather than following it.
 *
 * extra_headers may be NULL. On success returns a malloc'd body the caller
 * frees and sets *err to NULL. On failure returns NULL and points *err at a
 * static reason string suitable for surfacing to a caller.
 *
 * Never returns a body for a non-200 response. */
char *web_egress_fetch(const char *url, web_egress_policy_t policy, const char *extra_headers,
                       int timeout_ms, size_t max_bytes, const char **err);

/* As web_egress_fetch, but reports the address the connection was pinned to.
 *
 * A cache needs this: storing the validated address alongside the body is what
 * lets a later hit re-check it against current policy without resolving DNS
 * again. `pinned_out` receives the address (at least 64 bytes) and is set to an
 * empty string when the fetch fails. */
char *web_egress_fetch_pinned(const char *url, web_egress_policy_t policy,
                              const char *extra_headers, int timeout_ms, size_t max_bytes,
                              char *pinned_out, size_t pinned_out_len, const char **err);

/* True when a resolved address is private, reserved, loopback, or link-local.
 * Exposed for tests. */
struct sockaddr;
int web_egress_addr_blocked(const struct sockaddr *sa);

/* Whether the deployment permits a configured endpoint on a private address.
 * Reads AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT. Exposed for tests. */
int web_egress_private_endpoint_allowed(void);

#endif /* DEC_WEB_EGRESS_H */
