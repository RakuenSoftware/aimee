package main

import (
	"io"
	"net/http"
	"strings"
)

// maxProxyBodyBytes caps a proxied request body (defence against a session
// streaming an arbitrarily large payload through the console to the kb).
const maxProxyBodyBytes = 4 << 20 // 4 MiB

// proxyAPI forwards an authenticated /api/* request to the kb /v1 surface using
// the console-admin credential. DENY-BY-DEFAULT: the path is remapped to a /v1
// route that must pass the console-admin ACL mirror (acl.go); anything else is a
// 403 that never reaches the kb. The browser's own token is never forwarded —
// only the console-admin bearer, server-side.
func (s *server) proxyAPI(w http.ResponseWriter, r *http.Request, sess *session) {
	// /api/v1/<...>  ->  /v1/<...>. Only the /v1 surface is proxyable.
	// r.URL.Path is path-only (the stdlib strips the query into RawQuery), so the
	// ACL check below cannot be widened via a crafted query string.
	kbPath := strings.TrimPrefix(r.URL.Path, "/api")
	if !strings.HasPrefix(kbPath, "/v1/") || !consoleAdminAllows(r.Method, kbPath) {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "forbidden: not in console allowlist"})
		return
	}

	// CSRF (double-submit) on every mutating method.
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		if tok := r.Header.Get("X-CSRF-Token"); tok == "" || !constEq(tok, sess.csrf) {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "csrf token mismatch"})
			return
		}
	}

	target := strings.TrimRight(s.cfg.kbBaseURL, "/") + kbPath
	if q := r.URL.RawQuery; q != "" {
		target += "?" + q
	}

	var body io.Reader
	if r.Body != nil {
		body = http.MaxBytesReader(w, r.Body, maxProxyBodyBytes) // cap upload size
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, target, body)
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "proxy build failed"})
		return
	}
	req.Header.Set("Authorization", "Bearer "+s.kbBearer)
	if ct := r.Header.Get("Content-Type"); ct != "" {
		req.Header.Set("Content-Type", ct)
	}

	resp, err := s.kbClient.Do(req)
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "kb unreachable"})
		return
	}
	defer resp.Body.Close()
	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
	w.WriteHeader(resp.StatusCode)
	_, _ = io.Copy(w, resp.Body)
}
