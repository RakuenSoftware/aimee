package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"path/filepath"
	"strings"
)

// aimeeHTTPSockPath returns the path to aimee-server's /v1 HTTP-over-UDS socket,
// which lives alongside the RPC socket in the aimee config dir.
func (s *server) aimeeHTTPSockPath() string {
	return filepath.Join(filepath.Dir(s.cfg.socketPath), "aimee-http.sock")
}

// v1Request performs an HTTP request against aimee-server's /v1 API over its
// Unix socket. Personas (like all new client↔server features) are reached over
// /v1, never the legacy RPC socket. Returns status, body, and a transport error.
func (s *server) v1Request(ctx context.Context, method, path string, body []byte) (int, []byte, error) {
	sock := s.aimeeHTTPSockPath()
	client := &http.Client{
		Timeout: socketCallTimeout,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, "unix", sock)
			},
		},
	}
	var rdr io.Reader
	if body != nil {
		rdr = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, "http://aimee"+path, rdr)
	if err != nil {
		return 0, nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	return resp.StatusCode, data, err
}

// handleChatPersonas proxies GET /v1/personas — the list the UI selector renders.
// Degrades to an empty list so the selector can hide itself when aimee-server is
// unreachable rather than erroring the page.
func (s *server) handleChatPersonas(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/personas", nil)
	w.Header().Set("Content-Type", "application/json")
	if err != nil || st != http.StatusOK {
		fmt.Fprintf(w, `{"personas":[]}`)
		return
	}
	w.Write(data)
}

// handleChatPersonaItem proxies per-persona show/upsert/delete so the Workflows
// tab can manage personas directly (the engine's persona files are the source of
// truth). It forwards to aimee-server's admin-gated /v1/personas/<name> routes
// over the trusted UDS (same path the workflow save uses), returning status +
// body verbatim:
//
//	GET    /api/chat/personas/<name>  -> show (full definition for the edit form)
//	PUT    /api/chat/personas/<name>  -> create or edit (upsert from JSON body)
//	DELETE /api/chat/personas/<name>  -> remove (built-ins reset to the default)
func (s *server) handleChatPersonaItem(w http.ResponseWriter, r *http.Request) {
	seg := strings.TrimPrefix(r.URL.Path, "/api/chat/personas/")
	// one safe segment only: no separators, traversal, or percent-encoding (which
	// could smuggle a '/'). The server's persona_name_valid is the authoritative
	// guard; this fails fast and clearly.
	if seg == "" || strings.Contains(seg, "/") || strings.Contains(seg, "..") || strings.Contains(seg, "%") {
		writeJSONError(w, http.StatusBadRequest, "bad persona name")
		return
	}
	v1path := "/v1/personas/" + seg

	var method string
	var body []byte
	switch r.Method {
	case http.MethodGet:
		method = http.MethodGet
	case http.MethodPut:
		method = http.MethodPut
		const maxBody = 1 << 20
		body, _ = io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if len(body) > maxBody {
			writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
			return
		}
	case http.MethodDelete:
		method = http.MethodDelete
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, method, v1path, body)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		fmt.Fprintf(w, `{"error":"persona service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	w.Write(data)
}

// handleChatPersona reads or sets the active persona for a browser session:
//
//	GET  /api/chat/persona?sid=<id>   → the session's persona (else durable default)
//	POST /api/chat/persona {sid,name} → set it (sticky; both the chat system prompt
//	                                    and delegate-policy enforcement honor it)
//
// Setting goes through aimee-server's per-session store (POST /v1/sessions/<id>/
// persona), so a webchat conversation behaves exactly like a TUI session.
func (s *server) handleChatPersona(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()

	switch r.Method {
	case http.MethodGet:
		sid := r.URL.Query().Get("sid")
		path := "/v1/persona"
		if sid != "" {
			path = "/v1/sessions/" + sid + "/persona"
		}
		st, data, err := s.v1Request(ctx, http.MethodGet, path, nil)
		if err != nil || st != http.StatusOK {
			fmt.Fprintf(w, `{"name":""}`)
			return
		}
		w.Write(data)
	case http.MethodPost:
		var req struct {
			Sid  string `json:"sid"`
			Name string `json:"name"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Sid == "" || req.Name == "" {
			writeJSONError(w, http.StatusBadRequest, "sid and name required")
			return
		}
		body, _ := json.Marshal(map[string]string{"name": req.Name})
		st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/sessions/"+req.Sid+"/persona", body)
		if err != nil {
			writeJSONError(w, http.StatusServiceUnavailable, "aimee-server unavailable")
			return
		}
		if st == http.StatusNotFound {
			writeJSONError(w, http.StatusNotFound, "unknown persona")
			return
		}
		if st != http.StatusOK {
			writeJSONError(w, http.StatusBadGateway, "failed to set persona")
			return
		}
		w.Write(data)
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
	}
}
