package main

import (
	"context"
	"net/http"
	"os"
)

// GET /api/setup/appliance — whether this instance is the all-in-one appliance
// (AIMEE_WIZARD_APPLIANCE=1), so the setup wizard hides the baked KB/LLM/store
// steps and only asks for the provider, git connection, and workspaces.
func (s *server) handleSetupAppliance(w http.ResponseWriter, r *http.Request) {
	appliance := os.Getenv("AIMEE_WIZARD_APPLIANCE") == "1"
	w.Header().Set("Content-Type", "application/json")
	if appliance {
		w.Write([]byte(`{"appliance":true}`))
	} else {
		w.Write([]byte(`{"appliance":false}`))
	}
}

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
	// POST /apply may carry the first user's enrollment bearer. Never let a
	// browser, reverse proxy, or intermediary retain that one-time setup result.
	w.Header().Set("Cache-Control", "no-store")
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

// GET /api/embedders — the selectable-embedder catalog the wizard's Deploy-topology
// step builds its picker from, relayed verbatim from the server's /v1/embedders.
//
// Without this route the browser got 404 and fetchEmbedders swallowed it (by design:
// "a deployment whose server predates the endpoint still gets the free-text field").
// The failure was therefore silent and total — the picker listed no bundled model, so
// a new user could not choose bekko-a25m or nomic in the wizard at all, only
// "external", which wants an endpoint and a width they are unlikely to have. The
// catalog was there the whole time; nothing carried it across the webchat boundary.
func (s *server) handleEmbedders(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodGet, "/v1/embedders", nil)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "embedders: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, st, vaultSafeErrorMessage(data))
		return
	}
	w.Write(data)
}
