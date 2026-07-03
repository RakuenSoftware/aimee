package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
)

// handleConfigAll proxies GET /v1/config (config.show) — every typed config
// field and its current value — for the full Settings page. Degrades to an
// empty object so the page renders when aimee-server is unreachable.
func (s *server) handleConfigAll(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/config", nil)
	w.Header().Set("Content-Type", "application/json")
	if err != nil || st != http.StatusOK {
		fmt.Fprintf(w, `{"config":{}}`)
		return
	}
	w.Write(data)
}

// handleConfigSet proxies POST /api/config/set {key,value} -> /v1/config/set.
// The server validates the key against config_fields and persists aimee.yaml;
// the response may include a "notice" (e.g. the claude-CLI-delegate warning).
func (s *server) handleConfigSet(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	const maxBody = 1 << 16
	body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if len(body) > maxBody {
		writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/config/set", body)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "config service unreachable")
		return
	}
	w.WriteHeader(st)
	w.Write(data)
}
