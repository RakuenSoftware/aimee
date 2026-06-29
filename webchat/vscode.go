package main

// vscode.go: the /vscode reverse-proxy (webchat-git WP-J). The browser opens an
// in-app VSCode (code-server) running on aimee-server. aimee-server owns the
// editor process (WP-I) — it has the workspace files and the sealed-vault git
// env — and exposes it on a per-webuser loopback port via POST /v1/workspace/
// editor. webchat ensures that editor exists, then reverse-proxies /vscode/* to
// 127.0.0.1:<port> (HTTP + WebSocket). The loopback port never reaches the
// browser; the user only ever talks to webchat's own origin (so the iframe is
// same-origin and code-server's git credentials stay server-side, vault-only).
//
// Deployment note: this assumes webchat and aimee-server share a network
// namespace (the combined image / aimee-server image), so 127.0.0.1:<port> is
// reachable. The editor feature is scoped to those images by design.

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// editorPortTTL bounds how long a resolved per-user editor port is reused before
// re-confirming with aimee-server (which also keeps the editor's idle timer warm).
const editorPortTTL = 30 * time.Second

type editorPortEntry struct {
	port   int
	expiry time.Time
}

// editorPorts caches username -> {port, expiry} so we do not call ensure on every
// asset/WebSocket request. On a dial failure the entry is dropped and re-ensured.
var editorPorts sync.Map

// ensureEditorPort asks aimee-server to (idempotently) start the user's editor
// and returns its loopback port. Cached for editorPortTTL. force bypasses the
// cache (used after a dial failure, in case the editor was reaped/respawned).
func (s *server) ensureEditorPort(ctx context.Context, username string, force bool) (int, int, error) {
	if !force {
		if v, ok := editorPorts.Load(username); ok {
			e := v.(editorPortEntry)
			if time.Now().Before(e.expiry) {
				return e.port, http.StatusOK, nil
			}
		}
	}
	st, data, err := s.v1RequestWebuser(ctx, username, http.MethodPost, "/v1/workspace/editor", []byte(`{}`))
	if err != nil {
		return 0, http.StatusServiceUnavailable, err
	}
	if st != http.StatusOK {
		editorPorts.Delete(username)
		return 0, st, nil
	}
	var up struct {
		OK   bool `json:"ok"`
		Port int  `json:"port"`
	}
	if json.Unmarshal(data, &up) != nil || up.Port <= 0 {
		return 0, http.StatusBadGateway, nil
	}
	editorPorts.Store(username, editorPortEntry{port: up.Port, expiry: time.Now().Add(editorPortTTL)})
	return up.Port, http.StatusOK, nil
}

// handleVSCode reverse-proxies /vscode/* to the calling user's code-server. The
// path is forwarded UNCHANGED (code-server supports being hosted under a
// sub-path and derives its asset base from the request path); only the upstream
// host and scheme are rewritten. The webchat session cookie and bearer are
// stripped so code-server never sees webchat's auth material.
func (s *server) handleVSCode(w http.ResponseWriter, r *http.Request) {
	username := currentUser(r)
	if username == "" {
		http.Error(w, "authentication required", http.StatusUnauthorized)
		return
	}

	// requireAuth gates this route on the session cookie, but a cookie alone is
	// CSRF-able: a cross-origin page in the victim's browser can open
	// ws://<webchat>/vscode/... (or POST to code-server's HTTP API) carrying the
	// victim's cookie and drive the integrated terminal. Cookie SameSite=Strict
	// helps but is not an explicit gate, so reject any cross-origin request here.
	// The editor's own assets/XHR/WebSocket all originate from our same-origin
	// iframe, so legitimate traffic always matches; only foreign origins are cut.
	if !sameOriginVSCode(r) {
		http.Error(w, "cross-origin request rejected", http.StatusForbidden)
		return
	}

	// Bare /vscode must become /vscode/ at OUR origin — otherwise code-server
	// would answer the slash-less path with a redirect to /vscode/ built from the
	// upstream Host, leaking the loopback port and bouncing the browser off our
	// origin. Redirecting here keeps everything same-origin.
	if r.URL.Path == "/vscode" {
		http.Redirect(w, r, "/vscode/", http.StatusMovedPermanently)
		return
	}

	// A bounded context just for the ensure RPC (the proxied stream itself uses
	// the request context, which may be long-lived for WebSocket sessions).
	ectx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	port, st, err := s.ensureEditorPort(ectx, username, false)
	cancel()
	if err != nil {
		http.Error(w, "editor unavailable", http.StatusServiceUnavailable)
		return
	}
	if st == http.StatusServiceUnavailable || st == 503 {
		http.Error(w, "editor not enabled", http.StatusServiceUnavailable)
		return
	}
	if port <= 0 {
		http.Error(w, "editor unavailable", http.StatusBadGateway)
		return
	}

	s.proxyToEditor(w, r, username, port)
}

// proxyToEditor forwards the request to 127.0.0.1:<port>. httputil.ReverseProxy
// transparently handles WebSocket upgrades (the "Connection: Upgrade" tunnel
// code-server uses for the terminal + editor sync).
func (s *server) proxyToEditor(w http.ResponseWriter, r *http.Request, username string, port int) {
	target := &url.URL{Scheme: "http", Host: "127.0.0.1:" + strconv.Itoa(port)}
	origHost := r.Host
	proxy := &httputil.ReverseProxy{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, _ string) (net.Conn, error) {
				return (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, network, target.Host)
			},
		},
		Director: func(req *http.Request) {
			req.URL.Scheme = target.Scheme
			req.URL.Host = target.Host
			// Preserve the externally-visible host/proto via X-Forwarded-* so
			// code-server builds correct absolute URLs (asset/WebSocket) for the
			// proxied sub-path, then point the upstream Host at the loopback peer.
			req.Header.Set("X-Forwarded-Host", origHost)
			req.Header.Set("X-Forwarded-Proto", forwardedProto(r))
			if ip, _, e := net.SplitHostPort(r.RemoteAddr); e == nil {
				req.Header.Set("X-Forwarded-For", ip)
			}
			req.Host = target.Host
			// code-server runs with --auth none behind our auth gate; it must not
			// receive webchat's session cookie or bearer.
			req.Header.Del("Cookie")
			req.Header.Del("Authorization")
		},
		ModifyResponse: func(resp *http.Response) error {
			// The editor is embedded in a same-origin iframe; force SAMEORIGIN so
			// it frames regardless of what code-server emits, while still blocking
			// cross-origin clickjacking of the proxy.
			resp.Header.Set("X-Frame-Options", "SAMEORIGIN")
			// Belt-and-braces: if code-server emits a redirect to its own loopback
			// host, strip it to a relative path so the browser stays on our origin
			// and the port is never exposed.
			if loc := resp.Header.Get("Location"); loc != "" {
				if u, e := url.Parse(loc); e == nil && u.Host == target.Host {
					u.Scheme, u.Host = "", ""
					resp.Header.Set("Location", u.String())
				}
			}
			return nil
		},
		ErrorHandler: func(rw http.ResponseWriter, req *http.Request, e error) {
			// The cached port may be stale (editor reaped). Evict ONLY if the cache
			// still holds the failed port, so we never clobber a fresh port another
			// goroutine just stored, then report a transient error.
			if v, ok := editorPorts.Load(username); ok && v.(editorPortEntry).port == port {
				editorPorts.Delete(username)
			}
			rw.WriteHeader(http.StatusBadGateway)
			_, _ = rw.Write([]byte("editor connection failed"))
		},
	}

	// code-server holds a single long-lived WebSocket open for the editor/terminal
	// and makes no further /vscode HTTP requests during a session, so the
	// server-side idle timer (refreshed only by ensureEditorPort) would otherwise
	// expire and reap an actively-open editor mid-use. While this upgrade is being
	// proxied, periodically force an ensure to keep that timer warm; the ticker
	// stops as soon as the connection ends (tab closed), letting a truly idle
	// editor reap normally.
	if isWebSocketUpgrade(r) {
		stop := make(chan struct{})
		defer close(stop)
		go func() {
			t := time.NewTicker(editorPortTTL)
			defer t.Stop()
			for {
				select {
				case <-stop:
					return
				case <-t.C:
					kctx, cancel := context.WithTimeout(context.Background(), socketCallTimeout)
					_, _, _ = s.ensureEditorPort(kctx, username, true)
					cancel()
				}
			}
		}()
	}

	proxy.ServeHTTP(w, r)
}

// forwardedProto is the scheme the browser used to reach webchat. webchat
// usually terminates TLS itself (r.TLS set), but behind an external TLS
// terminator the hop to us is plaintext and the terminator sets
// X-Forwarded-Proto — honor it (first value of a possible comma list) so the
// same-origin gate and code-server's asset URLs use the real external scheme
// rather than wrongly seeing "http" and 403'ing/ mis-building every request.
func forwardedProto(r *http.Request) string {
	if xf := r.Header.Get("X-Forwarded-Proto"); xf != "" {
		if i := strings.IndexByte(xf, ','); i >= 0 {
			xf = xf[:i]
		}
		if xf = strings.ToLower(strings.TrimSpace(xf)); xf != "" {
			return xf
		}
	}
	if r.TLS != nil {
		return "https"
	}
	return "http"
}

// isWebSocketUpgrade reports whether r is a WebSocket upgrade handshake.
func isWebSocketUpgrade(r *http.Request) bool {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		return false
	}
	for _, tok := range strings.Split(r.Header.Get("Connection"), ",") {
		if strings.EqualFold(strings.TrimSpace(tok), "upgrade") {
			return true
		}
	}
	return false
}

// hostNoDefaultPort lower-cases host and drops the port when it is the default
// for scheme, so "host:443"/"https" and a bare "host" compare equal (the common
// :443/:80 deployment, where the browser omits the default port from Origin but
// the Host header keeps it).
func hostNoDefaultPort(host, scheme string) string {
	host = strings.ToLower(host)
	h, p, err := net.SplitHostPort(host)
	if err != nil {
		return host // no explicit port
	}
	if (scheme == "https" && p == "443") || (scheme == "http" && p == "80") {
		return h
	}
	return net.JoinHostPort(h, p)
}

// sameOriginVSCode is the cross-origin gate for the /vscode proxy. The browser
// reaches webchat at forwardedProto://r.Host; a request whose Origin resolves to
// a different scheme+host+port is cross-origin and rejected. Scheme and host are
// compared normalized (case-insensitive, default port stripped) so a same-origin
// request on :443/:80 — where the browser omits the default port from Origin —
// is not wrongly refused.
//
// A WebSocket upgrade MUST carry an Origin (browsers always send one on an
// upgrade), so a missing Origin on an upgrade is cross-origin and refused — this
// closes the cookie-only CSRF path to the editor terminal. A missing Origin on a
// non-upgrade request is allowed only for safe methods (top-level iframe
// navigations, asset GETs) or when the browser explicitly marks it same-origin
// via Sec-Fetch-Site, so a non-safe cross-origin call that omits Origin cannot
// slip through.
func sameOriginVSCode(r *http.Request) bool {
	origin := strings.TrimSpace(r.Header.Get("Origin"))
	if origin == "" {
		if isWebSocketUpgrade(r) {
			return false
		}
		switch r.Method {
		case http.MethodGet, http.MethodHead, http.MethodOptions:
			return true
		}
		sfs := strings.ToLower(strings.TrimSpace(r.Header.Get("Sec-Fetch-Site")))
		return sfs == "same-origin" || sfs == "none"
	}
	ou, err := url.Parse(origin)
	if err != nil || ou.Host == "" {
		return false
	}
	exp := forwardedProto(r)
	return strings.EqualFold(ou.Scheme, exp) &&
		hostNoDefaultPort(ou.Host, exp) == hostNoDefaultPort(r.Host, exp)
}
