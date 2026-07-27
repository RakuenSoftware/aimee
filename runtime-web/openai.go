package main

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httputil"
	"os"
	"path/filepath"
	"strings"
)

func (s *server) openAIBearerOK(r *http.Request) bool {
	if s == nil || s.cfg == nil || s.cfg.socketPath == "" {
		return false
	}
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(s.cfg.socketPath), "server.token"))
	if err != nil {
		return false
	}
	token := strings.TrimSpace(string(raw))
	if token == "" {
		return false
	}
	return r.Header.Get("Authorization") == "Bearer "+token
}

func (s *server) handleOpenAIModels(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	if !s.openAIBearerOK(r) {
		writeJSONError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	w.Header().Set("Content-Type", "application/json")

	// Prefer aimee-server's real /v1/models (the live agent/model list, already
	// OpenAI-shaped) over the legacy hardcoded entry. Fall back to the minimal
	// built-in list if the server is unreachable so the OpenAI-compatible
	// surface keeps answering.
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	if st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/models", nil); err == nil &&
		st == http.StatusOK && len(data) > 0 {
		w.Write(data)
		return
	}

	_ = json.NewEncoder(w).Encode(map[string]any{
		"object": "list",
		"data": []map[string]any{
			{
				"id":       "aimee",
				"object":   "model",
				"created":  0,
				"owned_by": "aimee",
			},
		},
	})
}

// proxyV1 forwards an OpenAI-surface request to aimee-server's real /v1 endpoint
// over the HTTP-UDS socket, streaming the response back so SSE chat completions
// flush incrementally (ReverseProxy disables buffering for text/event-stream).
// The webchat bearer is stripped before forwarding; the UDS is a trusted local
// channel that does not itself require the bearer.
// sessionUsername resolves the TRUSTED webchat identity (the PAM username from a
// server-validated session cookie), or "" when there is no valid session. This is
// the identity webchat already authenticated — not anything the client supplies.
func (s *server) sessionUsername(r *http.Request) string {
	if s == nil || s.sessions == nil {
		return ""
	}
	cookie, err := r.Cookie("session")
	if err != nil {
		return ""
	}
	username, err := s.sessions.ValidateSession(cookie.Value)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(username)
}

func (s *server) proxyV1(w http.ResponseWriter, r *http.Request, upstreamPath string) {
	sock := s.aimeeHTTPSockPath()
	// All stamped identity is server-derived (never client-supplied): the PAM
	// username from a server-validated webchat session gives a TRUSTED per-user
	// principal/session so distinct webchat users do not collapse; an external
	// client with only the shared bearer (no session) attributes to the single
	// trusted "webchat" service account. The client-supplied OpenAI `user` field
	// is deliberately NOT used for attribution — it would let a client choose its
	// own audit identity, which is not trusted.
	webUser := s.sessionUsername(r)
	proxy := &httputil.ReverseProxy{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, "unix", sock)
			},
		},
		Director: func(req *http.Request) {
			req.URL.Scheme = "http"
			req.URL.Host = "aimee"
			req.URL.Path = upstreamPath
			req.Host = "aimee"
			// Strip the webchat bearer (the UDS is a trusted local channel). CRITICAL:
			// also strip any client-supplied X-Aimee-* identity headers BEFORE
			// stamping our own — otherwise an external OpenAI client could send
			// X-Aimee-Principal: victim and, because we add the trusted proxy
			// secret, aimee-server would attribute the request to victim. All stamped
			// identity is server-derived: a validated webchat (PAM) session, or the
			// constant "webchat" account. The Idempotency-Key (not an identity header)
			// is forwarded unchanged.
			req.Header.Del("Authorization")
			req.Header.Del("X-Aimee-Proxy-Authorization")
			req.Header.Del("X-Aimee-Principal")
			req.Header.Del("X-Aimee-Source")
			req.Header.Del("X-Aimee-Session-Key")
			if secret := strings.TrimSpace(os.Getenv("AIMEE_INGRESS_PROXY_SECRET")); secret != "" {
				req.Header.Set("X-Aimee-Proxy-Authorization", secret)
				req.Header.Set("X-Aimee-Source", "webchat")
				if webUser != "" {
					// TRUSTED: server-validated webchat PAM session. Distinct users
					// get distinct trusted principals/sessions.
					req.Header.Set("X-Aimee-Principal", "webchat:"+webUser)
					req.Header.Set("X-Aimee-Session-Key", "webchat:"+webUser)
				} else {
					// External shared-bearer client: a shared credential carries no
					// per-client identity, so these requests are INTENTIONALLY
					// attributed to the single trusted "webchat" service account
					// (the stamped principal; no session key). Per-client distinction
					// requires a webchat session (PAM user) or per-user tokens. See
					// the proposal's webchat shared-bearer note.
					req.Header.Set("X-Aimee-Principal", "webchat")
				}
			}
		},
		ErrorHandler: func(w http.ResponseWriter, _ *http.Request, _ error) {
			writeJSONError(w, http.StatusBadGateway, "aimee-server unavailable")
		},
	}
	proxy.ServeHTTP(w, r)
}

// openAIProxyHandler returns a POST handler that authenticates the webchat
// bearer and proxies the OpenAI-surface request to aimee-server's real /v1
// endpoint, backing the surface with live inference instead of a stub.
func (s *server) openAIProxyHandler(upstreamPath string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		if !s.openAIBearerOK(r) {
			writeJSONError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		s.proxyV1(w, r, upstreamPath)
	}
}

// handleOpenAIChatCompletions backs the OpenAI /v1/chat/completions surface with
// aimee-server's real endpoint (streaming and non-streaming), completing the
// HTTP chat round-trip for external OpenAI-compatible clients.
func (s *server) handleOpenAIChatCompletions(w http.ResponseWriter, r *http.Request) {
	s.openAIProxyHandler("/v1/chat/completions")(w, r)
}
