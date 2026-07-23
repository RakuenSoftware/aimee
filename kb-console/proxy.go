package main

import (
	"io"
	"net/http"
	"strings"
)

// maxProxyBodyBytes caps a proxied request body (defence against a session
// streaming an arbitrarily large payload through the console to the kb).
const maxProxyBodyBytes = 4 << 20 // 4 MiB
const maxProxyResponseBytes = 1 << 20

// proxyAPI forwards an authenticated /api/* request to the kb /v1 surface using
// the console-admin credential. DENY-BY-DEFAULT: the path is remapped to a /v1
// route that must pass the console-admin ACL mirror (acl.go); anything else is a
// 403 that never reaches the kb. Administrative routes use only the server-side
// console-admin bearer; fleet routes use only the verified OIDC credential bound
// to the current session.
func (s *server) proxyAPI(w http.ResponseWriter, r *http.Request, sess *session) {
	if r.URL.RawPath != "" {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "encoded proxy paths are forbidden"})
		return
	}
	// /api/v1/<...>  ->  /v1/<...>. Only the /v1 surface is proxyable.
	// r.URL.Path is path-only (the stdlib strips the query into RawQuery), so the
	// ACL check below cannot be widened via a crafted query string.
	kbPath := strings.TrimPrefix(r.URL.Path, "/api")
	if !strings.HasPrefix(kbPath, "/v1/") {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "forbidden: not in console allowlist"})
		return
	}
	credential := ""
	switch {
	case consoleAdminAllows(r.Method, kbPath):
		credential = s.kbBearer
	case fleetAllows(r.Method, kbPath):
		if sess.breakGlass || !s.fleetOIDCEnabled {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "fleet access requires aligned OIDC login"})
			return
		}
		var ok bool
		credential, ok = s.oidcTokens.get(sess)
		if !ok {
			s.sessions.del(sess.id)
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "OIDC session expired; sign in again"})
			return
		}
	default:
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
	req.Header.Set("Authorization", "Bearer "+credential)
	if ct := r.Header.Get("Content-Type"); ct != "" {
		req.Header.Set("Content-Type", ct)
	}

	resp, err := s.kbClient.Do(req)
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "kb unreachable"})
		return
	}
	defer resp.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(resp.Body, maxProxyResponseBytes+1))
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "kb response read failed"})
		return
	}
	if len(payload) > maxProxyResponseBytes {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "kb response too large"})
		return
	}
	if resp.StatusCode == http.StatusUnauthorized && fleetAllows(r.Method, kbPath) {
		s.sessions.del(sess.id)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
	w.WriteHeader(resp.StatusCode)
	_, _ = w.Write(payload)
}
