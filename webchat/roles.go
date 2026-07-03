package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// handleRoles proxies GET /v1/role_templates — the list of delegate role names
// (the shared vocabulary personas and agents are matched on). Degrades to an
// empty list so the Personas page renders when aimee-server is unreachable.
func (s *server) handleRoles(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/role_templates", nil)
	w.Header().Set("Content-Type", "application/json")
	if err != nil || st != http.StatusOK {
		fmt.Fprintf(w, `{"role_templates":[]}`)
		return
	}
	w.Write(data)
}

// handleRoleItem proxies per-role show/upsert/delete so the Personas page can
// edit a role and what it does (its template body). Forwards to aimee-server's
// admin-gated /v1/role_templates/<name> routes over the trusted UDS, verbatim:
//
//	GET    /api/roles/<name>  -> {"content": "<markdown body>"}
//	PUT    /api/roles/<name>  -> create or edit (body {"content": "..."})
//	DELETE /api/roles/<name>  -> remove (built-ins reset to the bundled default)
func (s *server) handleRoleItem(w http.ResponseWriter, r *http.Request) {
	seg := strings.TrimPrefix(r.URL.Path, "/api/roles/")
	// one safe segment only — the server's role_template_name_valid is the
	// authoritative guard; this fails fast and clearly.
	if seg == "" || strings.Contains(seg, "/") || strings.Contains(seg, "..") || strings.Contains(seg, "%") {
		writeJSONError(w, http.StatusBadRequest, "bad role name")
		return
	}
	v1path := "/v1/role_templates/" + seg

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
		fmt.Fprintf(w, `{"error":"role service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	w.Write(data)
}
