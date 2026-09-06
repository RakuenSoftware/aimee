package main

import (
	"context"
	"io"
	"net/http"
)

func (s *server) proxyProviderConnection(w http.ResponseWriter, r *http.Request, path string) {
	var body []byte
	if r.Method == http.MethodPost {
		if !sameOriginRequest(r) {
			writeJSONError(w, http.StatusForbidden, "cross-origin provider request rejected")
			return
		}
		var err error
		body, err = io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<16))
		if err != nil {
			writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
			return
		}
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	var status int
	var data []byte
	var err error
	if r.Method == http.MethodPost {
		// Provider credentials use the same attested webuser hop as model.add --key.
		status, data, err = s.v1RequestWebuser(ctx, currentUser(r), r.Method, path, body)
	} else {
		status, data, err = s.v1Request(ctx, r.Method, path, body)
	}
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "provider service unreachable")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(data)
}

func (s *server) handleProviderConnections(w http.ResponseWriter, r *http.Request) {
	s.proxyProviderConnection(w, r, "/v1/provider/connections")
}
func (s *server) handleProviderSaveConnection(w http.ResponseWriter, r *http.Request) {
	s.proxyProviderConnection(w, r, "/v1/provider/save_connection")
}
func (s *server) handleProviderRemoveConnection(w http.ResponseWriter, r *http.Request) {
	s.proxyProviderConnection(w, r, "/v1/provider/remove_connection")
}

func (s *server) handleProviderConnectionModels(w http.ResponseWriter, r *http.Request) {
	s.proxyProviderConnection(w, r, "/v1/provider/connection_models")
}
