package main

import (
	"context"
	"net/http"
)

// Server-orchestrated container deploy. The setup wizard, after recording the
// page-2 backend config, asks aimee-server to bring up the managed sibling
// services (postgres + aimee-kb + aimee-llm) via `docker compose up -d` against
// the mounted Docker socket. These routes forward the webuser identity to the
// server's /v1/deploy/* handlers (which gate on AIMEE_DEPLOY_ENABLED + tool:execute)
// and relay the JSON response verbatim.

// POST /api/deploy/apply — kick off the deploy (runs on a server background thread).
func (s *server) handleDeployApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost,
		"/v1/deploy/apply", []byte(`{}`))
	s.deployRelay(w, st, data, err)
}

// GET /api/deploy/status — deploy progress + `docker compose ps`.
func (s *server) handleDeployStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodGet,
		"/v1/deploy/status", nil)
	s.deployRelay(w, st, data, err)
}

// deployRelay passes the aimee-server JSON response through verbatim (the deploy
// payload is server-authored, not vault material).
func (s *server) deployRelay(w http.ResponseWriter, st int, data []byte, err error) {
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "deploy: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, st, vaultSafeErrorMessage(data))
		return
	}
	w.Write(data)
}
