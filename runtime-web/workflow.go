package main

import (
	"context"
	"fmt"
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
		limit := int64(1 << 20)
		if v1path == "/v1/workflow/validate" {
			limit = 4 << 20
		}
		var ok bool
		body, ok = readBoundedBody(w, r, limit)
		if !ok {
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
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	s.webuserPassAuthorized(w, r, s.isAdmin(r), http.MethodGet, "/v1/workflow/blocks", nil)
}

// PUT/DELETE /api/workflow/blocks/<name> — create/edit or delete a custom
// delegate block. proxyWorkflow forwards a body only for POST, so this item
// handler forwards PUT bodies itself; seg-validated + body-capped, then handed
// to the admin-gated /v1 route (which refuses command executors).
func (s *server) handleWorkflowBlockItem(w http.ResponseWriter, r *http.Request) {
	if !s.isAdmin(r) {
		writeJSONError(w, http.StatusForbidden, "administrator access required")
		return
	}
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
		var ok bool
		body, ok = readBoundedBody(w, r, 1<<20)
		if !ok {
			return
		}
	case http.MethodDelete:
		method = http.MethodDelete
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	s.webuserPassAuthorized(w, r, true, method, "/v1/workflow/blocks/"+seg, body)
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

// GET /api/workflow/triggers — the configured trigger rules that auto-start runs.
func (s *server) handleWorkflowTriggers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	// The Go registry response includes whether this principal may edit global
	// triggers, so this read must carry the attested web identity too. The runtime
	// owns the appliance-admin lookup (the bootstrap account is renamed during
	// setup); attest that capability separately from the literal username.
	s.webuserPassAuthorized(w, r, s.isAdmin(r), http.MethodGet, "/v1/workflow/triggers", nil)
}

func (s *server) handleWorkflowConfig(w http.ResponseWriter, r *http.Request) {
	s.proxyWorkflow(w, r, http.MethodGet, "/v1/workflow/config", "")
}

func (s *server) handleWorkflowConfigSet(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	// Global run policy affects every user's autonomous work. The appliance's
	// bootstrap administrator is the only web identity allowed to mutate it;
	// per-user submission and lifecycle endpoints remain available to all users.
	if !s.isAdmin(r) {
		writeJSONError(w, http.StatusForbidden, "administrator access required")
		return
	}
	body, ok := readBoundedBody(w, r, 1<<20)
	if !ok {
		return
	}
	s.webuserPassAuthorized(w, r, true, http.MethodPost, "/v1/workflow/config/set", body)
}

// POST /api/workflow/validate — validate posted YAML without saving.
func (s *server) handleWorkflowValidate(w http.ResponseWriter, r *http.Request) {
	s.proxyWorkflow(w, r, http.MethodPost, "/v1/workflow/validate", "")
}

// POST /api/workflow/save — canonical-normalize + save (optimistic lock).
func (s *server) handleWorkflowSave(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	if !s.isAdmin(r) {
		writeJSONError(w, http.StatusForbidden, "administrator access required")
		return
	}
	body, ok := readBoundedBody(w, r, 1<<20)
	if !ok {
		return
	}
	s.webuserPassAuthorized(w, r, true, http.MethodPost, "/v1/workflow/save", body)
}

// Work-item run-state surface. Every call goes through v1RequestWebuser so the
// aimee-server sees the caller's webuser: principal — the item read handlers scope
// by ownership (submitter == principal), so the wrong identity would 403/empty.
//
//	GET  /api/workflow/items              — the caller's own work items
//	GET  /api/workflow/items/all          — all items (operator view)
//	GET  /api/workflow/items/<id>         — one item's run-state (owner-only)
//	GET  /api/workflow/items/<id>/events  — lifecycle timeline (owner-only, ?after&limit)
//	GET    /api/workflow/items/<id>/proposal— source markdown (owner-only)
//	POST   /api/workflow/items/<id>/gate    — approve/reject a parked human gate
//	POST   /api/workflow/items/<id>/pause   — operator-pause an active run
//	POST   /api/workflow/items/<id>/resume  — resume an operator-paused run
//	POST   /api/workflow/items/<id>/stop    — abandon (stop) an active run
//	DELETE /api/workflow/items/<id>         — auto-stop then purge the run
func (s *server) handleWorkflowItems(w http.ResponseWriter, r *http.Request) {
	// Exact list routes (no <id> segment).
	if r.URL.Path == "/api/workflow/items" && r.Method == http.MethodGet {
		s.webuserPass(w, r, http.MethodGet, "/v1/workflow/items", nil)
		return
	}
	if r.URL.Path == "/api/workflow/items/all" && r.Method == http.MethodGet {
		if !s.isAdmin(r) {
			writeJSONError(w, http.StatusForbidden, "administrator access required")
			return
		}
		s.webuserPassAuthorized(w, r, true, http.MethodGet, "/v1/workflow/items/all", nil)
		return
	}

	// Sub-resource routes: /api/workflow/items/<id>[/events|/proposal|/gate].
	rest := strings.TrimPrefix(r.URL.Path, "/api/workflow/items/")
	id, suffix := rest, ""
	if i := strings.IndexByte(rest, '/'); i >= 0 {
		id, suffix = rest[:i], rest[i:]
	}
	// <id> must be one safe segment: no '/' or '%' (could smuggle a separator past
	// this guard) and no ".." traversal sequence. A single '.' IS allowed — child
	// slice work-item ids are "<parent>.s<N>" (e.g. wi_abc123.s0), so rejecting all
	// dots made every slice's detail/gate/events unreachable (400 "bad path"),
	// which blocked approving a slice parked at a human gate. The server's matcher
	// is authoritative for what resolves.
	if id == "" || strings.ContainsAny(id, "/%") || strings.Contains(id, "..") {
		http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
		return
	}
	operator := s.isAdmin(r)

	switch {
	case suffix == "/gate" && r.Method == http.MethodPost:
		if !s.isAdmin(r) {
			writeJSONError(w, http.StatusForbidden, "administrator access required")
			return
		}
		body, ok := readBoundedBody(w, r, 64<<10)
		if !ok {
			return
		}
		s.webuserPassAuthorized(w, r, true, http.MethodPost, "/v1/workflow/items/"+id+"/gate", body)
	case suffix == "/events" && r.Method == http.MethodGet:
		path := "/v1/workflow/items/" + id + "/events"
		if r.URL.RawQuery != "" { // forward ?after=&limit= to the paginating handler
			path += "?" + r.URL.RawQuery
		}
		s.webuserPassAuthorized(w, r, operator, http.MethodGet, path, nil)
	case suffix == "/proposal" && r.Method == http.MethodGet:
		s.webuserPassAuthorized(w, r, operator, http.MethodGet, "/v1/workflow/items/"+id+"/proposal", nil)
	// Lifecycle mutations (body-less). The /v1 handlers enforce owner-or-operator
	// and the legal state transitions; we just forward under the webuser identity.
	case (suffix == "/pause" || suffix == "/resume" || suffix == "/stop") && r.Method == http.MethodPost:
		s.webuserPassAuthorized(w, r, operator, http.MethodPost, "/v1/workflow/items/"+id+suffix, nil)
	case suffix == "" && r.Method == http.MethodDelete:
		s.webuserPassAuthorized(w, r, operator, http.MethodDelete, "/v1/workflow/items/"+id, nil)
	case suffix == "" && r.Method == http.MethodGet:
		s.webuserPassAuthorized(w, r, operator, http.MethodGet, "/v1/workflow/items/"+id, nil)
	default:
		http.Error(w, `{"error":"bad path"}`, http.StatusBadRequest)
	}
}

// webuserPass proxies to an aimee-server /v1 path under the caller's webuser
// identity (so ownership scoping resolves) and streams the envelope back verbatim.
func (s *server) webuserPass(w http.ResponseWriter, r *http.Request, method, v1path string, body []byte) {
	s.webuserPassAuthorized(w, r, false, method, v1path, body)
}

// webuserPassAuthorized keeps identity and operator capability separate. The
// latter is resolved by isAdmin at this trusted runtime boundary; a username
// that happens to be "admin" remains an ordinary owner without the capability.
func (s *server) webuserPassAuthorized(w http.ResponseWriter, r *http.Request, operator bool, method, v1path string, body []byte) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	var st int
	var data []byte
	var err error
	if operator {
		st, data, err = s.v1RequestWorkflowOperator(ctx, currentUser(r), method, v1path, body)
	} else {
		st, data, err = s.v1RequestWebuser(ctx, currentUser(r), method, v1path, body)
	}
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		_, _ = fmt.Fprintf(w, `{"error":"workflow service unreachable"}`)
		return
	}
	w.WriteHeader(st)
	_, _ = w.Write(data)
}
