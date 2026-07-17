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
 * This header exposes the PURE, security-critical decision functions for unit
 * testing; the socket I/O lives in the .c. */

#include <stddef.h>
#include <sys/socket.h>

typedef enum
{
   SBX_REQ_INVALID = 0,
   SBX_REQ_API,      /* origin-form ("/...") — belongs to the existing /v1 stack */
   SBX_REQ_CONNECT,  /* CONNECT host:port HTTP/1.1 — HTTPS tunnel */
   SBX_REQ_ABSOLUTE, /* METHOD http://host[:port]/path HTTP/1.1 — plain-HTTP forward */
} sbx_req_kind_t;

/* Classify an HTTP request line. For CONNECT/ABSOLUTE, writes the target host into
 * host[hostcap] (lowercased) and the port into *port (443 default for CONNECT with no
 * port, 80 for absolute http). Returns SBX_REQ_INVALID on anything malformed. */
sbx_req_kind_t sandbox_pkg_classify_request_line(const char *line, char *host, size_t hostcap,
                                                 int *port);

/* SSRF guard: 1 if `sa` is NOT a public address a sandboxed delegate may reach —
 * loopback, unspecified, link-local (incl. 169.254.169.254 cloud metadata), RFC1918,
 * CGNAT (100.64/10), multicast/reserved, IPv6 ::1/fe80::/fc00::/ff00::, and the
 * v4-mapped IPv6 form of any blocked v4. Must be applied to the RESOLVED IP, and
 * re-applied to every address actually dialed (DNS rebinding). */
int sandbox_pkg_ip_is_blocked(const struct sockaddr *sa);

/* 1 if `port` is a package-registry port the proxy permits (80 or 443). */
int sandbox_pkg_port_allowed(int port);

/* 1 if `host` (case-insensitive) matches `allowlist` — a comma/whitespace-separated
 * list whose entries are a bare host (exact) or a `*.suffix` wildcard (matches the
 * suffix itself and any label beneath it). NULL/empty allowlist matches nothing. */
int sandbox_pkg_host_allowed(const char *host, const char *allowlist);

/* Curated default registry allowlist (deb/ubuntu mirrors, npm, PyPI). */
const char *sandbox_pkg_default_allowlist(void);

/* Serve one accepted proxy connection on `client_fd`. `head` is the already-read HTTP
 * request head (NUL-terminated, up to and including the blank line — the caller's
 * listener consumes it before deciding this is proxy traffic). Enforces port +
 * host-allowlist + SSRF (re-checked against every address actually dialed), then
 * CONNECT-tunnels (HTTPS, tunnel bytes still on the socket) or absolute-form-forwards
 * (plain HTTP) to the registry. `allowlist` NULL uses the curated default; `tag` (may
 * be NULL) labels the per-request audit log. The caller owns and closes `client_fd`.
 * Returns 0 once a request has been handled — including a 4xx/5xx refusal. */
int sandbox_pkg_proxy_serve(int client_fd, const char *head, const char *allowlist,
                            const char *tag);

#endif /* DEC_SANDBOX_PKG_PROXY_H */
