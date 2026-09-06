/* aimee_client.h: transport-agnostic client to aimee-server's /v1 HTTP API.
 *
 * The thin client (`aimee`) reaches the server through exactly one of two
 * transports, resolved per request:
 *
 *   - Remote TCP : when a remote server is configured — either programmatically
 *                  via aimee_client_set_remote() (e.g. from a `--server` flag or
 *                  config) or through the AIMEE_SERVER_URL environment variable
 *                  (http[s]://host[:port]). A bearer token (aimee_client_set_remote
 *                  token arg, or AIMEE_SERVER_TOKEN) is sent as
 *                  "Authorization: Bearer <token>" when non-empty.
 *
 *   - Local UDS  : the default on POSIX — HTTP/1.1 over <aimee_home>/aimee-http.sock,
 *                  authenticated by filesystem permissions, no token. This is the
 *                  legacy behaviour and is byte-for-byte unchanged when no remote
 *                  is configured.
 *
 * Windows has no UDS path: a remote target (set_remote or AIMEE_SERVER_URL) is
 * required for connectivity there.
 */
#ifndef DEC_AIMEE_CLIENT_H
#define DEC_AIMEE_CLIENT_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Configure a remote aimee-server target for this process. url is
    * "http[s]://host[:port]"; token may be NULL/empty. Pass url = NULL or "" to
    * clear a previously set remote (reverting to the env/UDS defaults). Intended
    * to be called once at startup (e.g. after parsing `--server`). Thread-safe. */
   void aimee_client_set_remote(const char *url, const char *token);

   /* True when a remote target is configured (set_remote or AIMEE_SERVER_URL).
    * The command dispatcher uses this to skip the local-socket preflight — on
    * Windows there is no UDS, so the remote path is the only one. */
   int aimee_client_has_remote(void);

   /* Where this process actually sends requests, as a short credential-free
    * string (any userinfo in the URL is stripped). Returned storage is static
    * and overwritten by the next call. Error paths print it so a command that
    * silently used the local socket is not mistaken for one that reached the
    * configured server. */
   const char *aimee_client_transport_label(void);

   /* Parse a single remote-server CLI flag (argv[*i]): --server=URL,
    * --server URL (advances *i), or --server-token=TOK. On a match, sets *url
    * and/or *token to point into argv and returns 1; returns 0 otherwise. */
   int aimee_client_parse_flag(char **argv, int *i, int argc, const char **url, const char **token);

   /* If either url or token is non-NULL, apply a remote override: fill the
    * missing one from AIMEE_SERVER_URL / AIMEE_SERVER_TOKEN and call
    * aimee_client_set_remote. A no-op when both are NULL. */
   void aimee_client_apply_override(const char *url, const char *token);

   /* Returns 1 if a remote target is in effect for this process (either set via
    * aimee_client_set_remote or AIMEE_SERVER_URL), else 0. When non-NULL,
    * *desc_out is filled with a short human-readable target ("host:port") for
    * diagnostics; desc_sz is its buffer size. */
   int aimee_client_remote_active(char *desc_out, unsigned long desc_sz);

   /* Like aimee_client_remote_active, but also reports via *is_https_out whether
    * the resolved target is an https:// URL (so callers can select the tls:
    * vs tcp: transport scheme). is_https_out may be NULL. */
   int aimee_client_remote_active_scheme(char *desc_out, unsigned long desc_sz, int *is_https_out);

   /* Returns 1 if a remote target is in effect and copies its bearer token (the
    * second remote.conf line / --server-token / AIMEE_SERVER_TOKEN) into *tok_out
    * (empty string when the target has no token), else 0. Dependency-free (no
    * networking) so callers that only need the resolved token don't pull the
    * transport closure. tok_sz is the buffer size. */
   int aimee_client_remote_token(char *tok_out, unsigned long tok_sz);

   /* Send an HTTP request to aimee-server's /v1 API over the resolved transport.
    * method: "GET"/"POST"; path: e.g. "/v1/personas"; body: JSON or NULL.
    * Returns the response body (heap; caller frees) and sets *status_out to the
    * HTTP status. Returns NULL on connect/transport failure (status_out = 0). */
   char *aimee_client_request(const char *method, const char *path, const char *body,
                              int *status_out);

   /* Model-proxy transport: relay the complete HTTP response, including headers,
    * incrementally. Uses the resolved remote bearer and native TLS identity.
    * Caller supplies validated model route/protocol headers, never credentials.
    * on_socket registers the live fd for cancellation; -1 unregisters it before
    * close. Returns -1 on transport/callback failure; never retries a request. */
   int aimee_client_proxy_request(const char *method, const char *path, const char *headers,
                                  const void *body, unsigned long body_len, const char *session_id,
                                  int (*write_response)(const void *, unsigned long, void *),
                                  void (*on_socket)(int, void *), void *context);

   /* Security guard: returns 1 when transmitting bearer |token| to a server at
    * (|is_https|, |host|) would expose the credential in cleartext — a non-empty
    * token over plaintext http:// to a non-loopback host. The remote-TCP request
    * path refuses such a request rather than leak the bearer on the wire.
    * Exposed for unit testing of the invariant. */
   int aimee_client_would_leak_cleartext(int is_https, const char *host, const char *token);

   /* Fetch the leaf certificate of the https:// server named in |url| WITHOUT
    * verifying it (trust-on-first-use), returning it as PEM (caller free()s
    * *pem_out) plus its SHA-256 fingerprint in |fp_out|. Backs `aimee remote
    * set/trust` cert pinning. Returns 0 on success; -1 if |url| is not https, the
    * host is unreachable, or pinning is unsupported in this build/platform. */
   int aimee_client_fetch_cert(const char *url, char **pem_out, char *fp_out, unsigned long fp_n);

   /* Suppress (on != 0) or restore the transport's connection-failure messages on
    * stderr. `aimee remote set` brackets its pre-pin probe with this so an
    * expected, about-to-be-pinned handshake failure does not print. */
   void aimee_client_suppress_conn_errors(int on);

#ifdef __cplusplus
}
#endif

#endif /* DEC_AIMEE_CLIENT_H */
