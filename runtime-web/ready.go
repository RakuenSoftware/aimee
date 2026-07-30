package main

import (
	"context"
	"encoding/json"
	"net/http"
)

// GET /api/ready — relay the server's dependency readiness to the browser.
//
// The web UI needs this to tell the user when aimee is not fully functional.
// Without it a degraded instance looks identical to a healthy one: search
// silently returns nothing, freshly cloned repos never appear, and the only
// symptom is the user concluding their code "isn't in there". That is exactly
// how a real deployment ran for hours with eighteen cloned repos and an empty
// index while the wizard reported success.
//
// Deliberately unauthenticated-shaped in its OUTPUT: this returns only the
// three dependency states and nothing about topology, versions, or errors, so
// it is safe to render before a session is established. It still goes through
// requireAuth at registration, matching every other /api route.
func (s *server) handleReady(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()

	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodGet, "/v1/ready", nil)
	w.Header().Set("Content-Type", "application/json")

	// A server we cannot reach at all IS the degraded case the banner exists to
	// report — surface it as such rather than as an opaque error, so the UI has
	// something true to show instead of failing closed into silence.
	if err != nil || st != http.StatusOK {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ready":  false,
			"status": "degraded",
			"dependencies": map[string]string{
				"kb": "fail", "db1": "unknown", "retrieval": "unknown",
			},
		})
		return
	}

	var up struct {
		Ready        bool              `json:"ready"`
		Status       string            `json:"status"`
		Dependencies map[string]string `json:"dependencies"`
	}
	if json.Unmarshal(data, &up) != nil {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ready": false, "status": "unknown",
			"dependencies": map[string]string{},
		})
		return
	}
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ready":        up.Ready,
		"status":       up.Status,
		"dependencies": up.Dependencies,
	})
}
