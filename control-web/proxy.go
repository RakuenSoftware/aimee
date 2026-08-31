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
// route that must pass the shared control-web module policy (acl.go); anything
// else is a 403 that never reaches the kb. The KB independently requests its
// authoritative console-admin decision over the event bus. Administrative routes
// use only the server-side console-admin bearer; fleet routes use only the
// verified OIDC credential bound to the current session.
func (s *server) proxyAPI(w http.ResponseWriter, r *http.Request, sess *session) {
	if r.URL.RawPath != "" {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "encoded proxy paths are forbidden"})
		return
	}
	// /api/v1/<...>  ->  /v1/<...>. Only the /v1 surface is proxyable.
	// r.URL.Path is path-only (the stdlib strips the query into RawQuery), so the
	// ACL check below cannot be widened via a crafted query string.
	kbPath := strings.TrimPrefix(r.URL.Path, "/api")
	fleetRoute := fleetAllows(r.Method, kbPath)
	fleetMutation := fleetRoute && r.Method == http.MethodPost
	fleetAckToken := ""
	if !strings.HasPrefix(kbPath, "/v1/") {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "forbidden: not in console allowlist"})
		return
	}
	credential := ""
	switch {
	case consoleAdminAllows(r.Method, kbPath):
		credential = s.kbBearer
	case fleetRoute:
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
	if fleetMutation {
		var err error
		fleetAckToken, err = randToken()
		if err != nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "fleet mutation token unavailable"})
			return
		}
		claimed, err := s.sessions.claimFleetMutation(sess.id, fleetAckToken)
		if err != nil {
			s.sessions.del(sess.id)
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "fleet mutation latch unavailable; sign in again"})
			return
		}
		if !claimed {
			writeJSON(w, http.StatusConflict, map[string]string{"error": "prior fleet action result unresolved; sign in again only after operator resolution"})
			return
		}
		sess.fleetIndeterminate = true
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
		if fleetMutation {
			s.transitionFleetMutationOrDelete(sess, fleetAckToken, 1, 0)
		}
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
	keepFleetLatch := fleetMutation && (resp.StatusCode == http.StatusConflict || resp.StatusCode == http.StatusBadGateway)
	if resp.StatusCode == http.StatusUnauthorized && fleetRoute {
		s.sessions.del(sess.id)
	}
	if fleetMutation && !keepFleetLatch && resp.StatusCode != http.StatusUnauthorized {
		if !s.transitionFleetMutationOrDelete(sess, fleetAckToken, 1, 2) {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "fleet result latch unavailable; sign in again"})
			return
		}
		w.Header().Set("X-Aimee-Fleet-Ack", fleetAckToken)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
	w.WriteHeader(resp.StatusCode)
	_, _ = w.Write(payload)
}

func (s *server) transitionFleetMutationOrDelete(sess *session, ackToken string, from, to int) bool {
	if err := s.sessions.transitionFleetMutation(sess.id, ackToken, from, to); err != nil {
		s.sessions.del(sess.id)
		return false
	}
	sess.fleetIndeterminate = to != 0
	return true
}
