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

// PUT/DELETE /api/workflow/blocks/<name> — create/edit or delete a custom
// delegate block. proxyWorkflow forwards a body only for POST, so this item
// handler forwards PUT bodies itself; seg-validated + body-capped, then handed
// to the admin-gated /v1 route (which refuses command executors).
func (s *server) handleWorkflowBlockItem(w http.ResponseWriter, r *http.Request) {
	seg := strings.TrimPrefix(r.URL.Path, "/api/workflow/blocks/")
	if seg == "" || strings.Contains(seg, "/") || strings.Contains(seg, "..") || strings.Contains(seg, "%") {
		writeJSONError(w, http.StatusBadRequest, "bad block name")
		return
	}
	var method string
	var body []byte
	switch r.Method {
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
	st, data, err := s.v1Request(ctx, method, "/v1/workflow/blocks/"+seg, body)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		fmt.Fprintf(w, `{"error":"workflow service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	w.Write(data)
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

// Work-item run-state surface. Every call goes through v1RequestWebuser so the
// aimee-server sees the caller's webuser: principal — the item read handlers scope
// by ownership (submitter == principal), so the wrong identity would 403/empty.
//
//	GET  /api/workflow/items              — the caller's own work items
//	GET  /api/workflow/items/all          — all items (operator view)
//	GET  /api/workflow/items/<id>         — one item's run-state (owner-only)
//	GET  /api/workflow/items/<id>/events  — lifecycle timeline (owner-only, ?after&limit)
//	GET  /api/workflow/items/<id>/proposal— source markdown (owner-only)
//	POST /api/workflow/items/<id>/gate    — approve/reject a parked human gate
func (s *server) handleWorkflowItems(w http.ResponseWriter, r *http.Request) {
	// Exact list routes (no <id> segment).
	if r.URL.Path == "/api/workflow/items" && r.Method == http.MethodGet {
		s.webuserPass(w, r, http.MethodGet, "/v1/workflow/items", nil)
		return
	}
	if r.URL.Path == "/api/workflow/items/all" && r.Method == http.MethodGet {
		s.webuserPass(w, r, http.MethodGet, "/v1/workflow/items/all", nil)
		return
	}

	// Sub-resource routes: /api/workflow/items/<id>[/events|/proposal|/gate].
	rest := strings.TrimPrefix(r.URL.Path, "/api/workflow/items/")
	id, suffix := rest, ""
	if i := strings.IndexByte(rest, '/'); i >= 0 {
		id, suffix = rest[:i], rest[i:]
	}
	// <id> must be one safe segment (no separators/traversal/percent-encoding that
	// could smuggle a '/' past this guard); the server's matcher is authoritative.
	if id == "" || strings.ContainsAny(id, "/.%") {
		http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
		return
	}

	switch {
	case suffix == "/gate" && r.Method == http.MethodPost:
		const maxBody = 1 << 20
		body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if len(body) > maxBody {
			http.Error(w, `{"error":"request too large"}`, http.StatusRequestEntityTooLarge)
			return
		}
		s.webuserPass(w, r, http.MethodPost, "/v1/workflow/items/"+id+"/gate", body)
	case suffix == "/events" && r.Method == http.MethodGet:
		path := "/v1/workflow/items/" + id + "/events"
		if r.URL.RawQuery != "" { // forward ?after=&limit= to the paginating handler
			path += "?" + r.URL.RawQuery
		}
		s.webuserPass(w, r, http.MethodGet, path, nil)
	case suffix == "/proposal" && r.Method == http.MethodGet:
		s.webuserPass(w, r, http.MethodGet, "/v1/workflow/items/"+id+"/proposal", nil)
	case suffix == "" && r.Method == http.MethodGet:
		s.webuserPass(w, r, http.MethodGet, "/v1/workflow/items/"+id, nil)
	default:
		http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
	}
}

// webuserPass proxies to an aimee-server /v1 path under the caller's webuser
// identity (so ownership scoping resolves) and streams the envelope back verbatim.
func (s *server) webuserPass(w http.ResponseWriter, r *http.Request, method, v1path string, body []byte) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), method, v1path, body)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		_, _ = fmt.Fprintf(w, `{"error":"workflow service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	_, _ = w.Write(data)
}
