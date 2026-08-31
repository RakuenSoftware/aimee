/* kb_tls.h: mutual-TLS context builders for aimee-kb's distributed-mode
 * transport. (distributed-mode-auth proposal, mTLS phase.)
 *
 * Both peers authenticate with certificates issued by the kb's internal CA
 * (kb_pki.h): the kb presents a server cert and REQUIRES a client cert; the
 * client presents its enrollment-issued cert and verifies the server against
 * the same CA (which it pinned by fingerprint via the aimee:// connection
 * string). After the handshake, the kb derives the caller's scope from the
 * verified client certificate's subject CN (verify-then-trust). */
#ifndef DEC_KB_TLS_H
#define DEC_KB_TLS_H

#include "kb_pki.h"
#include <openssl/ssl.h>
#include <stddef.h>

#define KB_TLS_PEER_ISSUER_MAX KB_PKI_ISSUER_MAX
#define KB_TLS_PEER_SERIAL_MAX KB_PKI_SERIAL_MAX
/* Maximum token text stored in the 4096-byte process-secret slot. */
#define KB_TLS_BEARER_TOKEN_MAX 4095

#ifdef __cplusplus
extern "C"
{
#endif

   /* True only when the PEM leaf carries exactly one EKU and it is the
    * requested role (serverAuth when server_role=1, clientAuth otherwise). */
   int kb_tls_cert_has_exact_role(const char *cert_pem, int server_role);

   /* Build an mTLS SERVER context: presents (server_cert_pem, server_key_pem),
    * trusts ca_cert_pem to verify CLIENT certs, and REQUIRES a valid client cert
    * (the handshake fails if the peer presents none or an untrusted one). TLS
    * 1.2+. Returns a new SSL_CTX (caller SSL_CTX_free) or NULL on error. */
   SSL_CTX *kb_tls_server_ctx(const char *ca_cert_pem, const char *server_cert_pem,
                              const char *server_key_pem);

   /* Build an mTLS CLIENT context: presents (client_cert_pem, client_key_pem),
    * trusts ca_cert_pem to verify the SERVER cert. TLS 1.2+. Returns a new
    * SSL_CTX (caller SSL_CTX_free) or NULL on error. */
   SSL_CTX *kb_tls_client_ctx(const char *ca_cert_pem, const char *client_cert_pem,
                              const char *client_key_pem);

   /* After a successful handshake, copy the peer certificate's subject CN (the
    * enrollment scope, for a client cert) into out[cap]. Returns 0 on success,
    * -1 if there is no peer certificate or it does not fit. */
   int kb_tls_peer_cn(SSL *ssl, char *out, size_t cap);

   /* Lowercase-hex sha256 fingerprint of the peer (client) certificate DER, into
    * hex_out (needs >=65 bytes). Matches kb_pki_ca_fingerprint. 0 on success. */
   int kb_tls_peer_fingerprint(SSL *ssl, char *hex_out, size_t cap);

   /* Peer cert issuer DN + serial (uppercase hex) — the (issuer, serial) immutable
    * revocation-key inputs for a transport (cert:CN) principal (P1 I5). Return 0 on
    * success, -1 if no peer cert, extraction fails, or the value does not fit. */
   int kb_tls_peer_issuer(SSL *ssl, char *out, size_t cap);
   int kb_tls_peer_serial(SSL *ssl, char *out, size_t cap);

   /* Serve ONE mTLS connection on `fd` using `ctx`: complete the handshake
    * (which REQUIRES + verifies a client cert), read the HTTP request, route it
    * with the scope taken from the client certificate's CN (verify-then-trust —
    * a "<kind>:<id>" CN authorizes only that scope; "global"/owner gets full
    * access), and write the response. The caller owns `fd` (not closed here).
    * The plaintext listener path is unaffected. Exposed for unit testing the
    * full mTLS request path over a socketpair. */
   void kb_tls_serve_conn(int fd, SSL_CTX *ctx);

   /* Test-only seam for the host PAM verifier used by the service-identity
    * layer. Passing NULL restores the production pam_check_credentials path. */
   void kb_tls_set_pam_check_for_test(int (*check)(const char *, const char *));

   /* --- the kb mTLS listener (distributed mode) --- */

   /* Start the mTLS listener on `port` (0 = OS-assigned; see kb_mtls_bound_port).
    * Loads-or-creates the CA under <data_dir>/kb-ca, issues a server cert for
    * `host` signed by it, and serves /v1 over mutual TLS — every request must
    * present a CA-issued client cert, and its scope is taken from the cert. Binds
    * all interfaces (distributed mode is for remote peers). Returns 0 on success,
    * -1 on error. Call kb_mtls_stop() before re-starting. */
   int kb_mtls_start(int port, const char *data_dir, const char *host);

   /* The port the listener actually bound (useful when started with port 0), or
    * 0 if not running. */
   int kb_mtls_bound_port(void);

   /* Snapshot bounded-listener occupancy for transport observability. Any
    * output pointer may be NULL. */
   void kb_mtls_connection_stats(int *limit_out, int *live_out, int *queued_out);

   /* Stop the mTLS listener and free its context. Safe when not running. */
   void kb_mtls_stop(void);

   /* --- the mTLS client dialer (distributed mode, post-enrollment) --- */

   /* Make an HTTP request to a kb over mutual TLS. Connects to host:port,
    * verifies the server certificate against ca_cert_pem, presents the
    * enrollment-issued (client_cert_pem, client_key_pem), sends `method path`
    * with optional `body` (NULL/"" for none), and writes the response body into
    * resp_out[resp_cap] with *status_out set to the HTTP status. Returns 0 on
    * success (a response was received + parsed), -1 on resolve / connect / TLS /
    * I/O failure. This is what an enrolled aimee client uses to reach a remote
    * kb; the CA + client cert come from redeeming the connection string. */
   int kb_tls_client_request(const char *host, int port, const char *ca_cert_pem,
                             const char *client_cert_pem, const char *client_key_pem,
                             const char *method, const char *path, const char *body, char *resp_out,
                             size_t resp_cap, int *status_out);
   int kb_tls_client_request_auth(const char *host, int port, const char *ca_cert_pem,
                                  const char *client_cert_pem, const char *client_key_pem,
                                  const char *method, const char *path, const char *body,
                                  const char *authorization, char *resp_out, size_t resp_cap,
                                  int *status_out);

   typedef struct kb_tls_client_conn kb_tls_client_conn_t;

   /* Reusable one-in-flight HTTP/1.1 client primitive. request() reads one
    * strict Content-Length-framed response exactly and reports whether the
    * connection remains reusable. */
   kb_tls_client_conn_t *kb_tls_client_conn_open(const char *host, int port,
                                                 const char *ca_cert_pem,
                                                 const char *client_cert_pem,
                                                 const char *client_key_pem);
   kb_tls_client_conn_t *kb_tls_client_conn_open_ctx(const char *host, int port, SSL_CTX *ctx);
   kb_tls_client_conn_t *kb_tls_client_conn_open_session(const char *host, int port, SSL_CTX *ctx,
                                                         SSL_SESSION *session);
   /* Set the per-request socket I/O timeout on an open client connection. The
    * default remains 30 seconds for callers that do not opt in; long-running
    * relays (for example kb.build) must propagate their operation timeout. */
   int kb_tls_client_conn_set_timeout(kb_tls_client_conn_t *conn, int timeout_ms);
   int kb_tls_client_conn_session_reused(const kb_tls_client_conn_t *conn);
   SSL_SESSION *kb_tls_client_conn_get1_session(const kb_tls_client_conn_t *conn);
   int kb_tls_client_conn_request(kb_tls_client_conn_t *conn, const char *method, const char *path,
                                  const char *body, const char *authorization, int close_after,
                                  char *resp_out, size_t resp_cap, int *status_out,
                                  int *reusable_out);
   /* As above, but preserve an explicit caller-owned Content-Type header for
    * non-JSON bodies such as documentation multipart uploads. NULL selects the
    * existing application/json default. */
   int kb_tls_client_conn_request_with_type(kb_tls_client_conn_t *conn, const char *method,
                                            const char *path, const char *body,
                                            const char *authorization, const char *content_type,
                                            int close_after, char *resp_out, size_t resp_cap,
                                            int *status_out, int *reusable_out);
   void kb_tls_client_conn_close(kb_tls_client_conn_t *conn);

   /* TOFU bootstrap: fetch the kb's CA certificate from GET /v1/enroll/ca and
    * trust it ONLY if its sha256 fingerprint equals `expected_fp_hex` (the value
    * pinned in the aimee:// connection string). The first connection is made
    * without server verification (the client has no CA yet) — security comes
    * from the out-of-band fingerprint match, which defeats a MITM presenting a
    * different CA. On success writes the CA cert PEM into ca_out[ca_cap] and
    * returns 0; returns -1 on connect / I/O failure or a fingerprint mismatch. */
   int kb_tls_fetch_ca(const char *host, int port, const char *expected_fp_hex, char *ca_out,
                       size_t ca_cap);

   /* Consume an `aimee://` connection string end to end on the client: parse it,
    * TOFU-fetch + pin the CA by fingerprint, generate a fresh keypair + CSR,
    * redeem the enrollment token for a client certificate, and output the mTLS
    * identity — the CA cert into ca_out, the issued client cert into cert_out,
    * and the freshly-generated private key into key_out (which never leaves the
    * client). After this the client dials the kb with kb_tls_client_request using
    * (ca_out, cert_out, key_out). Returns 0 on success, -1 on a malformed string
    * / pin mismatch / redeem failure. */
   int kb_tls_enroll(const char *conn_string, char *ca_out, size_t ca_cap, char *cert_out,
                     size_t cert_cap, char *key_out, size_t key_cap);

   /* Returns 1 if the PEM certificate expires within `seconds` from now, 0 if it
    * is valid longer, -1 on a parse error. Drives pre-expiry rotation. */
   int kb_tls_cert_expires_within(const char *cert_pem, long seconds);

   /* Rotate a client certificate over mTLS: present (cur_cert, cur_key) to the kb,
    * generate a FRESH keypair + CSR, POST it to /v1/enroll/renew, and output the
    * new cert + new private key (the kb binds it to the same verified scope from
    * the presented cert). The new key never leaves the client. Returns 0 on
    * success, -1 on connect / TLS / renew failure. */
   int kb_tls_renew(const char *host, int port, const char *ca_cert_pem, const char *cur_cert_pem,
                    const char *cur_key_pem, const char *authorization, char *cert_out,
                    size_t cert_cap, char *key_out, size_t key_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_TLS_H */
