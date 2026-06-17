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

	s.proxyToEditor(w, r, port)
}

// proxyToEditor forwards the request to 127.0.0.1:<port>. httputil.ReverseProxy
// transparently handles WebSocket upgrades (the "Connection: Upgrade" tunnel
// code-server uses for the terminal + editor sync).
func (s *server) proxyToEditor(w http.ResponseWriter, r *http.Request, port int) {
	target := &url.URL{Scheme: "http", Host: "127.0.0.1:" + strconv.Itoa(port)}
	proxy := &httputil.ReverseProxy{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, _ string) (net.Conn, error) {
				return (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, network, target.Host)
			},
		},
		Director: func(req *http.Request) {
			req.URL.Scheme = target.Scheme
			req.URL.Host = target.Host
			req.Host = target.Host
			// code-server runs with --auth none behind our auth gate; it must not
			// receive webchat's session cookie or bearer.
			req.Header.Del("Cookie")
			req.Header.Del("Authorization")
			req.Header.Set("X-Forwarded-Proto", forwardedProto(r))
		},
		ModifyResponse: func(resp *http.Response) error {
			// The editor is embedded in a same-origin iframe; force SAMEORIGIN so
			// it frames regardless of what code-server emits, while still blocking
			// cross-origin clickjacking of the proxy.
			resp.Header.Set("X-Frame-Options", "SAMEORIGIN")
			return nil
		},
		ErrorHandler: func(rw http.ResponseWriter, req *http.Request, e error) {
			// The cached port may be stale (editor reaped). Drop it so the next
			// request re-ensures; report a transient error to the browser.
			editorPorts.Delete(currentUser(req))
			rw.WriteHeader(http.StatusBadGateway)
			_, _ = rw.Write([]byte("editor connection failed"))
		},
	}
	proxy.ServeHTTP(w, r)
}

func forwardedProto(r *http.Request) string {
	if r.TLS != nil {
		return "https"
	}
	return "http"
}
