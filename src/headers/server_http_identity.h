#ifndef DEC_SERVER_HTTP_IDENTITY_H
#define DEC_SERVER_HTTP_IDENTITY_H 1

#include "server.h" /* server_conn_t, attested_transport_t */

/* server_http_identity: WP-C.0 attested-identity threading for the /v1 front-end.
 *
 * Every /v1 request reaches server_dispatch through loopback_rpc's synthesized
 * (memset-zeroed) server_conn_t, so the caller's kernel-attested identity would
 * otherwise be lost before the delegate worker resolves credentials. These three
 * functions carry it across that boundary, all on the one worker thread that
 * handles the connection synchronously up to compute_ctx creation:
 *   1. capture  — handle_conn records the attested transport + vault principal
 *      (SO_PEERCRED peer uid, or a server.token-gated `webuser:` assertion) into
 *      thread-locals;
 *   2. apply    — loopback_rpc copies them onto the synthesized conn;
 *   3. clear    — handle_conn wipes them after the route so a reused worker
 *      thread cannot leak one request's identity into the next.
 * create_compute_ctx then copies the conn's identity into compute_ctx_t for the
 * detached worker. The principal is the ONLY identity key the vault trusts —
 * never a client-supplied session_id. */

/* Capture this request's attested identity into the per-thread state.
 *   fd      - the connection socket (SO_PEERCRED source for UDS).
 *   is_tcp  - 1 for the network listener, 0 for the local UDS socket.
 *   buf     - the raw HTTP request (read for the X-Aimee-Webuser / Authorization headers).
 *   bearer  - the configured server.token bearer (empty when none), required to
 *             honor a webuser assertion. */
void server_http_identity_capture(int fd, int is_tcp, const char *buf, const char *bearer);

/* Copy the captured identity onto a (synthesized) connection — loopback_rpc's
 * hop 2. Safe to call with no prior capture: writes the un-attested defaults. */
void server_http_identity_apply(server_conn_t *conn);

/* Reset the per-thread captured identity to the un-attested, no-vault defaults. */
void server_http_identity_clear(void);

#endif /* DEC_SERVER_HTTP_IDENTITY_H */
