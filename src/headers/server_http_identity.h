#ifndef DEC_SERVER_HTTP_IDENTITY_H
#define DEC_SERVER_HTTP_IDENTITY_H 1

#include "server.h" /* server_conn_t, attested_transport_t */
#include "server_tls.h"

/* server_http_identity: WP-C.0 attested-identity threading for the /v1 front-end.
 *
 * Every /v1 request reaches server_dispatch through loopback_rpc's synthesized
 * (memset-zeroed) server_conn_t, so the caller's kernel-attested identity would
 * otherwise be lost before the delegate worker resolves credentials. These three
 * functions carry it across that boundary, all on the one worker thread that
 * handles the connection synchronously up to compute_ctx creation:
 *   1. capture  — handle_conn records the attested transport + vault principal
 *      (SO_PEERCRED peer uid, or a root-UDS-gated `webuser:` assertion) into
 *      thread-locals;
 *   2. apply    — loopback_rpc copies them onto the synthesized conn;
 *   3. clear    — handle_conn wipes them after the route so a reused worker
 *      thread cannot leak one request's identity into the next.
 * create_compute_ctx then copies the conn's identity into compute_ctx_t for the
 * detached worker. The principal is the ONLY identity key the vault trusts —
 * never a client-supplied session_id.
 * Streaming chat/responses/messages handlers are synchronous on the same worker
 * and receive the same capture/clear lifetime before their early return. */

/* Capture this request's attested identity into the per-thread state.
 *   fd      - the connection socket (SO_PEERCRED source for UDS).
 *   is_tcp  - 1 for the network listener, 0 for the local UDS socket.
 *   buf     - the raw HTTP request (read for the X-Aimee-Webuser / Authorization headers).
 * A webuser assertion is honored only for the root-owned webchat peer over the
 * Unix socket. TCP can never assert a webuser this way. */
void server_http_identity_capture(int fd, int is_tcp, const char *buf);

/* The captured vault principal for the in-flight request (empty if un-attested),
 * for buffered route handlers that need the caller identity without synthesizing
 * a conn (e.g. the webchat-git /v1/workspace/clone route). Valid only between
 * capture and clear, i.e. during a route handler on the serving thread. */
const char *server_http_identity_principal(void);

/* Replace the transport-derived principal with a server-authoritative identity
 * already bound to the verified client certificate. */
void server_http_identity_override_principal(const char *principal);

/* The in-flight request's inbound `aimee-session-id` header value and bearer token
 * (both "" when absent), for the economizer gateway-mutation session-key resolver.
 * Valid only during a route handler on the serving thread (same lifetime as the
 * principal); the bearer buffer is zeroed on clear. */
const char *server_http_identity_session_hdr(void);
const char *server_http_identity_bearer(void);

/* Split the generic session-bound gateway credential
 *   <bearer>.aimee-session.<32 lowercase hex chars>
 * into its authorization bearer and launcher-owned session id. Unbound tokens
 * are copied unchanged with an empty session id. The suffix is correlation
 * state inside the already-authenticated bearer principal; it grants no
 * authority on its own. Returns 1 when bound. */
/* Constant-time bearer comparison that tolerates a session-binding suffix. */
int server_http_bearer_matches(const char *presented, const char *bearer_cfg);

int server_http_session_bearer_unbind(const char *presented, char *bearer, size_t bearer_n,
                                      char *session_id, size_t session_n);
const char *server_http_identity_status_staple(void);
const server_tls_peer_cert_t *server_http_identity_peer_cert(void);
const server_tls_peer_cert_t *server_http_identity_local_cert(void);
const char *server_http_identity_local_fingerprint(void);

/* The in-flight request's query string ("k=v&…", no '?'), or "" if none. Set by
 * server_http_identity_set_query around the route call, cleared by _clear. Valid
 * only during a route handler on the serving thread; points into the request
 * buffer. Used by buffered handlers that read query params (e.g. events cursor). */
void server_http_identity_set_query(const char *q);
const char *server_http_identity_query(void);

/* Copy the captured identity onto a (synthesized) connection — loopback_rpc's
 * hop 2. Safe to call with no prior capture: writes the un-attested defaults. */
void server_http_identity_apply(server_conn_t *conn);

/* Reset the per-thread captured identity to the un-attested, no-vault defaults. */
void server_http_identity_clear(void);

/* Write a JSON error body {"error":"<msg>"} into resp (cap bytes), JSON-escaping
 * msg so dynamic content (e.g. git stderr with newlines/quotes, file paths)
 * cannot produce an invalid body that a client then fails to parse. Always
 * NUL-terminates when cap > 0; truncates an over-long message safely. */
void http_error_json(char *resp, size_t cap, const char *msg);

#endif /* DEC_SERVER_HTTP_IDENTITY_H */
