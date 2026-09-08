package main

import (
	"context"
	"encoding/json"
	"net/http"
)

// Pairing remains available after setup and on manually managed deployments.
// The authenticated browser principal is forwarded over the trusted local
// socket; the application store checks ownership for every operation.
func (s *server) handleClients(w http.ResponseWriter, r *http.Request) {
	path := "/v1/clients"
	if r.URL.Path == "/api/clients/revoke" {
		if r.Method != http.MethodPost {
			writeJSONError(w, 405, "method not allowed")
			return
		}
		path += "/revoke"
	} else if r.Method != http.MethodGet && r.Method != http.MethodPost {
		writeJSONError(w, 405, "method not allowed")
		return
	}
	var body []byte
	if r.Method == http.MethodPost {
		var request map[string]json.RawMessage
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&request); err != nil {
			writeJSONError(w, 400, "invalid client request")
			return
		}
		body, _ = json.Marshal(request)
	}
	if path == "/v1/clients" && r.Method == http.MethodPost {
		if _, replaced := readReplacementUsername(s.setupAccountMarker()); !replaced {
			if _, pending := s.pendingBootstrapUsername(); pending {
				writeJSONError(w, 409, "Complete the account step in setup before pairing clients")
				return
			}
		}
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	status, data, err := s.v1RequestWebuser(ctx, currentUser(r), r.Method, path, body)
	s.deployRelay(w, status, data, err)
}
