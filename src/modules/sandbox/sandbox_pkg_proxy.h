#ifndef DEC_SANDBOX_PKG_PROXY_H
#define DEC_SANDBOX_PKG_PROXY_H

/* Package-access forward proxy for a --network none delegate sandbox.
 *
 * aimee-server serves a narrow forward proxy on its bound UDS (the delegate reaches
 * it via the in-container aimee-forwarder, 127.0.0.1:3129 -> UDS). It handles:
 *   - CONNECT host:port   -> byte-tunnel to an allowed, non-SSRF registry (HTTPS)
 *   - absolute-form HTTP  -> forward to an allowed http:// mirror (apt)
 * The delegate never holds an outside socket; aimee performs and logs every fetch.
 *
 * The security-critical request, allowlist and SSRF decisions live in the Go
 * sandbox module. This header exposes only the C connectivity seam. */

/* Serve one accepted proxy connection on `client_fd`. `is_uds` MUST be nonzero — the
 * proxy is only ever legitimate on the delegate-facing UNIX socket, never the public
 * TCP/TLS surface; the function refuses (returns 0 without touching the network) when
 * is_uds is 0, a second independent guard beside the caller's listener check. `head`
 * is the already-read HTTP request head (NUL-terminated, up to the blank line).
 * Asks the sandbox module to enforce port + host-allowlist + SSRF (re-checked against
 * every dialed address), then CONNECT-tunnels (HTTPS) or absolute-form-forwards
 * (plain HTTP) to the registry. `allowlist` NULL asks the module for its curated
 * default; `tag` (may be NULL) labels the per-request audit log. The caller owns and
 * closes `client_fd`. Returns 0 once handled. */
int sandbox_pkg_proxy_serve(int client_fd, int is_uds, const char *head, const char *allowlist,
                            const char *tag);

#endif /* DEC_SANDBOX_PKG_PROXY_H */
