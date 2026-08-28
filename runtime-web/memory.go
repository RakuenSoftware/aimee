package main

import (
	"context"
	"net/http"
)

// memoryProxyHandler relays one fixed Memory Center mutation/query to the
// canonical server /v1 route. The path comes only from registerRoutes, never
// from request input, and the authenticated actor is asserted over the trusted
// Unix-socket boundary so project scoping and rejection audit remain intact.
func (s *server) memoryProxyHandler(path string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		body, ok := readBoundedBody(w, r, 1<<20)
		if !ok {
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
		defer cancel()
		status, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, path, body)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, "memory service unavailable")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		_, _ = w.Write(data)
	}
}
