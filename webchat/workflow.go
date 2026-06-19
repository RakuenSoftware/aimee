package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// Workflow visual composer (W7) proxy: forwards /api/workflow/* to aimee-server's
// first-class /v1/workflow/* routes over the UDS, returning status + body
// verbatim. Mirrors the persona/dashboard proxy pattern; all routes sit behind
// requireAuth in server.go.

// proxyWorkflow forwards one request to a /v1/workflow route. When stripPrefix is
// non-empty, the single trailing path segment (a workflow name or work-item id)
// is appended to v1path; it must be one safe segment (no '/' and no "..").
func (s *server) proxyWorkflow(w http.ResponseWriter, r *http.Request, method, v1path, stripPrefix string) {
	if r.Method != method {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	path := v1path
	if stripPrefix != "" {
		seg := strings.TrimPrefix(r.URL.Path, stripPrefix)
		// one safe segment only: no separators, no traversal, no percent-encoding
		// (which could smuggle a '/' past this check). The server's safe_name is
		// the authoritative guard; this fails fast and clearly.
		if seg == "" || strings.Contains(seg, "/") || strings.Contains(seg, "..") || strings.Contains(seg, "%") {
			http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
			return
		}
		path = v1path + seg
	}
	var body []byte
	if method == http.MethodPost {
		const maxBody = 1 << 20
		body, _ = io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if len(body) > maxBody {
			http.Error(w, `{"error":"request too large"}`, http.StatusRequestEntityTooLarge)
			return
		}
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1Request(ctx, method, path, body)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		fmt.Fprintf(w, `{"error":"workflow service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	w.Write(data)
}

// GET /api/workflow/blocks — the block palette catalog.
func (s *server) handleWorkflowBlocks(w http.ResponseWriter, r *http.Request) {
	s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/blocks", "")
}

// GET /api/workflow/defs        — list definitions
// GET /api/workflow/defs/<name> — one definition (canonical + version + graph)
func (s *server) handleWorkflowDefs(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/api/workflow/defs" {
		s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/defs", "")
		return
	}
	s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/defs/", "/api/workflow/defs/")
}

// POST /api/workflow/validate — validate posted YAML without saving.
func (s *server) handleWorkflowValidate(w http.ResponseWriter, r *http.Request) {
	s.proxyWorkflow(w, r, http.MethodPost, "/v1/workflow/validate", "")
}

// POST /api/workflow/save — canonical-normalize + save (optimistic lock).
func (s *server) handleWorkflowSave(w http.ResponseWriter, r *http.Request) {
	s.proxyWorkflow(w, r, http.MethodPost, "/v1/workflow/save", "")
}

// GET /api/workflow/items       — list work items (run-state)
// GET /api/workflow/items/<id>  — one work item's run-state
func (s *server) handleWorkflowItems(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/api/workflow/items" {
		s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/items", "")
		return
	}
	// POST /api/workflow/items/<id>/gate -> operator approve/reject of a parked
	// human gate. <id> must be one safe segment.
	if r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/gate") {
		id := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/api/workflow/items/"), "/gate")
		if id == "" || strings.ContainsAny(id, "/.%") {
			http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
			return
		}
		const maxBody = 1 << 20
		body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if len(body) > maxBody {
			http.Error(w, `{"error":"request too large"}`, http.StatusRequestEntityTooLarge)
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
		defer cancel()
		st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost,
			"/v1/workflow/items/"+id+"/gate", body)
		if err != nil {
			http.Error(w, `{"error":"aimee-server unavailable"}`, http.StatusBadGateway)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(st)
		_, _ = w.Write(data)
		return
	}
	s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/items/", "/api/workflow/items/")
}
