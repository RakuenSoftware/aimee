package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// Roundtable preset proxies. The engine owns named roundtable presets as JSON
// files (roundtable_preset.{c,h}); the GUI reaches them over aimee-server's
// admin-gated /v1/roundtables routes on the trusted UDS, mirroring the persona
// proxy (persona.go). Responses are forwarded verbatim.
//
//	GET  /api/roundtables            -> list presets (+ active flag)
//	POST /api/roundtables            -> create a preset (name in body)
//	POST /api/roundtables/active     -> make a preset the active roundtable
//	GET  /api/roundtables/<name>     -> show a preset
//	PUT  /api/roundtables/<name>     -> create or edit a preset
//	DELETE /api/roundtables/<name>   -> remove a preset

// handleRoundtables serves the collection endpoint: GET (list) and POST (create).
func (s *server) handleRoundtables(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	w.Header().Set("Content-Type", "application/json")

	switch r.Method {
	case http.MethodGet:
		st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/roundtables", nil)
		if err != nil || st != http.StatusOK {
			// Degrade to an empty list so the tab renders rather than erroring.
			fmt.Fprintf(w, `{"roundtables":[],"active":""}`)
			return
		}
		w.Write(data)
	case http.MethodPost:
		body := readLimitedBody(w, r)
		if body == nil {
			return
		}
		st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/roundtables", body)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, "roundtable service unreachable")
			return
		}
		w.WriteHeader(st)
		w.Write(data)
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
	}
}

// handleRoundtableItem serves the per-name routes plus the /active selector.
func (s *server) handleRoundtableItem(w http.ResponseWriter, r *http.Request) {
	seg := strings.TrimPrefix(r.URL.Path, "/api/roundtables/")
	// One safe segment only: no separators, traversal, or percent-encoding. The
	// server's roundtable_preset_name_valid is the authoritative guard.
	if seg == "" || strings.Contains(seg, "/") || strings.Contains(seg, "..") || strings.Contains(seg, "%") {
		writeJSONError(w, http.StatusBadRequest, "bad preset name")
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	w.Header().Set("Content-Type", "application/json")

	// The active-preset selector: POST /api/roundtables/active {name}.
	if seg == "active" {
		if r.Method != http.MethodPost {
			http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
			return
		}
		body := readLimitedBody(w, r)
		if body == nil {
			return
		}
		st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/roundtables/active", body)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, "roundtable service unreachable")
			return
		}
		w.WriteHeader(st)
		w.Write(data)
		return
	}

	v1path := "/v1/roundtables/" + seg
	var method string
	var body []byte
	switch r.Method {
	case http.MethodGet:
		method = http.MethodGet
	case http.MethodPut:
		method = http.MethodPut
		body = readLimitedBody(w, r)
		if body == nil {
			return
		}
	case http.MethodDelete:
		method = http.MethodDelete
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	st, data, err := s.v1Request(ctx, method, v1path, body)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "roundtable service unreachable")
		return
	}
	w.WriteHeader(st)
	w.Write(data)
}

// readLimitedBody reads the request body up to a 1 MiB cap, writing an error
// response and returning nil if it is too large.
func readLimitedBody(w http.ResponseWriter, r *http.Request) []byte {
	const maxBody = 1 << 20
	body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if len(body) > maxBody {
		writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
		return nil
	}
	return body
}
