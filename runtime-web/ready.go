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

	// Not having ASKED is not the same as having been told "no". This used to
	// answer every failure with kb:"fail", which renders as "the knowledge
	// service is unreachable" — a specific claim about a specific dependency
	// that this handler has no evidence for. On a fresh instance the common
	// cause is v1RequestWebuser's own 401 (no webchat session established yet),
	// so a healthy deployment accused its KB of being down for the whole of the
	// setup wizard, and the one banner that must stay credible was the one
	// lying.
	//
	// 401 means we could not ask with authority: report nothing rather than
	// guess. healthBanner() treats "unknown" as "no banner", which is right —
	// a user without a session is not reading search results yet.
	if st == http.StatusUnauthorized {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ready":  false,
			"status": "unknown",
			"dependencies": map[string]string{
				"kb": "unknown", "db1": "unknown", "retrieval": "unknown",
			},
		})
		return
	}

	// A degraded aimee-server answers /v1/ready with HTTP 503 AND a complete
	// dependency body. That is a VERDICT, not a failure to answer. Treating any
	// non-200 as unreachable threw that body away and substituted a fabricated
	// kb:"fail" — which is how an instance whose kb was demonstrably healthy got
	// reported as "the knowledge service is unreachable" when the real fault was
	// an embedder that never loaded. Trust any parseable dependency payload,
	// whatever the status code carrying it.
	var up struct {
		Ready        bool              `json:"ready"`
		Status       string            `json:"status"`
		Dependencies map[string]string `json:"dependencies"`
	}
	if err == nil && json.Unmarshal(data, &up) == nil && len(up.Dependencies) > 0 {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ready":        up.Ready,
			"status":       up.Status,
			"dependencies": up.Dependencies,
		})
		return
	}

	// Nothing usable came back, so the user's results really are untrustworthy
	// and the banner must fire. Say only what we can stand behind: retrieval is
	// unavailable. We never reached the kb and cannot vouch for it either way.
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ready":  false,
		"status": "degraded",
		"dependencies": map[string]string{
			"kb": "unknown", "db1": "unknown", "retrieval": "fail",
		},
	})
}
