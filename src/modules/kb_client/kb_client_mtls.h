/* kb_client_mtls.h: aimee-server's distributed-mode mTLS transport to a remote
 * aimee-kb. Implemented in src/modules/kb_client/kb_client_mtls.c; selected by the kb_client
 * v1 transports when AIMEE_KB_CONN names a remote kb. A kb_client_*.h bridge
 * header (server-includable). */
#ifndef DEC_KB_CLIENT_MTLS_H
#define DEC_KB_CLIENT_MTLS_H 1

/* 1 when AIMEE_KB_CONN holds an aimee:// connection string (a remote kb), else 0. */
int kb_client_mtls_configured(void);

/* Perform a /v1 request to the configured remote kb over mTLS. Enrolls once on
 * first use (TOFU-pin the CA, redeem the token for a client cert). Returns the
 * heap response body (caller frees) with *status_out set, or NULL on failure.
 * method is "GET"/"POST"; body is NULL for GET. */
char *kb_client_mtls_request(const char *method, const char *path, const char *body,
                             int *status_out);

#endif /* DEC_KB_CLIENT_MTLS_H */
