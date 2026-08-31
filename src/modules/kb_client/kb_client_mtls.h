/* kb_client_mtls.h: aimee-server's distributed-mode mTLS transport to a remote
 * aimee-kb. Implemented in src/modules/kb_client/kb_client_mtls.c; selected by the kb_client
 * v1 transports when AIMEE_KB_CONN names a remote kb. A kb_client_*.h bridge
 * header (server-includable). */
#ifndef DEC_KB_CLIENT_MTLS_H
#define DEC_KB_CLIENT_MTLS_H 1

#include <stddef.h>

#define KB_CLIENT_ERR_POOL_EXHAUSTED (-2)
#define KB_CLIENT_ERR_AUTH_REQUIRED  (-3)

typedef int (*kb_client_mtls_renew_fn)(const char *host, int port, const char *ca_cert_pem,
                                       const char *cur_cert_pem, const char *cur_key_pem,
                                       const char *authorization, char *cert_out, size_t cert_cap,
                                       char *key_out, size_t key_cap);

/* 1 when Vault holds an AIMEE_KB_CONN aimee:// connection string (a remote kb), else 0.
 * The string supplies the stable endpoint + CA pin after its one-time token has
 * established an owner-only identity under AIMEE_HOME. */
int kb_client_mtls_configured(void);

/* Read the stable server/team binding from a wizard-installed version-2
 * identity. Explicit AIMEE_SERVER_ID / AIMEE_SERVER_TEAM_ID settings remain
 * authoritative at their call sites; these accessors are the managed fallback.
 * Returns 1 when a complete ready identity was loaded, 0 when none is present. */
int kb_client_mtls_managed_metadata(char *server_id_out, size_t server_id_cap,
                                    long long *team_id_out);

/* Perform a /v1 request to the configured remote kb over mTLS. Enrolls once on
 * first use (TOFU-pin the CA, redeem the token for a client cert). Returns the
 * heap response body (caller frees) with *status_out set, or NULL on failure.
 * method is "GET"/"POST"; body is NULL for GET. */
char *kb_client_mtls_request(const char *method, const char *path, const char *body,
                             int *status_out);
/* As above, but propagate the caller's operation timeout to the mTLS socket.
 * This prevents the transport's 30-second default from truncating builds whose
 * public operation contract allows several minutes. */
char *kb_client_mtls_request_timeout(const char *method, const char *path, const char *body,
                                     int timeout_ms, int *status_out);
char *kb_client_mtls_request_timeout_with_type(const char *method, const char *path,
                                               const char *body, const char *content_type,
                                               int timeout_ms, int *status_out);

/* Fetch the exact bounded P5-C2c signed envelope.  Only an authenticated 200
 * response on the fixed route is accepted; output is cleared on every error. */
int kb_client_mtls_management_jwks(char *envelope_out, size_t envelope_cap, size_t *envelope_len);
int kb_client_mtls_management_jwks_fetch(void *ctx, char *envelope_out, size_t envelope_cap,
                                         size_t *envelope_len);

/* Publish this server's bounded health record over its enrolled mTLS identity.
 * The kb authorizes immutable issuer/serial/fingerprint metadata from TLS; the
 * caller-supplied server_id is only a selector and cannot refresh another row. */
int kb_client_mtls_heartbeat(const char *server_id, const char *health, const char *version);

/* Pool observability and lifecycle. reset closes idle entries immediately and
 * makes borrowed entries drain on return. Output pointers may be NULL. */
void kb_client_mtls_pool_stats(int *total_out, int *idle_out, int *busy_out, int *waiters_out,
                               unsigned long *borrow_exhausted_total_out);
void kb_client_mtls_pool_reset(void);
/* Bind transport.kb_pool_enabled to the server's live config snapshot. */
void kb_client_mtls_pool_register_reload(void);
void kb_client_mtls_tls_stats(unsigned long *handshakes_total_out,
                              unsigned long *resumed_total_out);

/* Test seam: discard process-memory identity and pool state while deliberately
 * retaining the durable identity file, simulating a server process restart. */
void kb_client_mtls_reset_for_test(void);
void kb_client_mtls_set_identity_path_for_test(const char *absolute_path);
void kb_client_mtls_set_server_identity_path_for_test(const char *absolute_path);
void kb_client_mtls_set_renew_window_for_test(long seconds);
void kb_client_mtls_set_renew_for_test(kb_client_mtls_renew_fn renew);

#endif /* DEC_KB_CLIENT_MTLS_H */
