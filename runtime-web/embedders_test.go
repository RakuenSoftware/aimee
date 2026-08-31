package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

// The wizard's Deploy-topology step builds its embedder picker from
// GET /api/embedders. That route did not exist: the browser got 404, and
// fetchEmbedders swallows a failed catalog fetch on purpose ("a deployment whose
// server predates the endpoint still gets the free-text field"). So the failure was
// silent AND total — the picker listed no bundled model, and a new user could not
// select bekko-a25m or nomic in the wizard at all. Their only remaining option was
// "external", which wants an endpoint and a vector width they are unlikely to have.
//
// The catalog was always there, correct, at the server's /v1/embedders. Nothing
// carried it across the webchat boundary.
//
// This asserts the route is REGISTERED. Whether the relay body is right is the
// server's contract; what broke here was that nothing answered at all.
func TestEmbeddersRouteIsRegistered(t *testing.T) {
	var s server
	mux := http.NewServeMux()
	s.registerRoutes(mux)

	// The mux carries a catch-all for the SPA, so "some handler matched" proves
	// nothing — an unregistered /api path falls through to it and the browser gets
	// the app shell (or 404), which is exactly the bug. Require the EXACT pattern.
	req := httptest.NewRequest(http.MethodGet, "/api/embedders", nil)
	_, pattern := mux.Handler(req)
	if pattern != "/api/embedders" {
		t.Fatalf("GET /api/embedders resolved to %q, want an exact /api/embedders route: "+
			"the wizard's embedder picker is empty and setup cannot be completed "+
			"in a browser", pattern)
	}
}

// A wrong method must not read as "no such endpoint" — that is the symptom the
// missing route produced, and the two should stay distinguishable.
func TestEmbeddersRejectsNonGetWithoutLookingMissing(t *testing.T) {
	var s server
	w := httptest.NewRecorder()
	s.handleEmbedders(w, httptest.NewRequest(http.MethodPost, "/api/embedders", nil))
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST /api/embedders = %d, want %d", w.Code, http.StatusMethodNotAllowed)
	}
}
