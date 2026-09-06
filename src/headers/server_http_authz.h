/* server_http_authz.h — the two entry points server_http.c needs from the
 * authorization unit that used to live inside it. The public capability and
 * route-gate functions (server_http_conn_caps, server_http_route_allowed_caps,
 * server_http_global_ignored_count, …) keep their existing declarations in
 * server_http.h: they moved translation unit, not interface. */
#ifndef AIMEE_SERVER_HTTP_AUTHZ_H
#define AIMEE_SERVER_HTTP_AUTHZ_H

#include "pki.h"
#include "server_identity_token.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Resolve the caller's per-user write tier from the Authorization header of
    * the in-flight request (proposal per-user-remote-writes-authz.md §5).
    *
    * Verification only — the token's single-use jti is NOT spent here, so a
    * request later refused by the rate limit or the route gate never burns a
    * token the user would have to replace. The caller spends it with
    * server_write_tier_consume_for_request once the request is known servable,
    * and must OPENSSL_cleanse *claims either way.
    *
    * A no-op returning SERVER_REMOTE_WRITES_OFF when !is_tcp: UDS is structurally
    * exempt (§7) and its capabilities never pass through the tier at all.
    * *identity_present is set only when a token actually verified. */
   int server_http_resolve_write_tier(int is_tcp, const char *buf, const char *method,
                                      const char *path, const char *request_id,
                                      server_identity_token_claims_t *claims,
                                      int *identity_present);

   /* Count one request refused that the retired aimee.api.remote_writes would
    * formerly have allowed; surfaced as remote_writes.global_ignored. */
   void server_http_note_global_ignored(void);

   /* Translate the durable certificate-roster verdict into the HTTP status for
    * the request-time mTLS re-check.  An unreadable authority is a temporary
    * service failure, not evidence that the presented certificate was revoked,
    * expired, or never issued.  Keep this pure so the fail-closed distinction
    * is pinned without needing a live TLS connection or store module. */
   int server_http_mtls_recheck_status(pki_cert_status_t status);

   /* Reconstruct the capability set the retired process-global setting would
    * have supplied, for observability only. */
   int server_http_retired_global_would_allow(int fd, int is_tcp, const char *bearer,
                                              int remote_writes, int mtls_mode,
                                              int mtls_authenticated, const char *method,
                                              const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_HTTP_AUTHZ_H */
