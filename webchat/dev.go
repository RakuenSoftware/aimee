package main

import (
	"context"
	"io"
	"net/http"
)

// POST /api/dev/submit {proposal_md, workflow?, repo?} — autonomous-development
// intake. Proxies to /v1/dev/submit under the authenticated webuser identity so
// the run is created in the caller's scope. Body is passed through verbatim.
func (s *server) handleDevSubmit(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	const maxBody = 1 << 20
	body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if len(body) > maxBody {
		writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/dev/submit", body)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "aimee-server unavailable")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(st)
	_, _ = w.Write(data)
}
