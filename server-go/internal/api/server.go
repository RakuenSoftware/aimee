package api

import (
	"context"
	"crypto/subtle"
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	appconfig "github.com/JBailes/aimee/server-go/internal/config"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type Server struct {
	db              *db1.Store
	artifacts       *wfe.ArtifactStore
	workflowDir     string
	workflows       *wfe.Registry
	config          *appconfig.Store
	mux             *http.ServeMux
	notify          func()
	cancel          func(string)
	cleanupWorktree func(context.Context, db1.WorkItem) error
	triggerMu       sync.Mutex
	triggerErrorsMu sync.Mutex
	triggerErrors   map[string]string
}

func New(db *db1.Store, artifacts *wfe.ArtifactStore, workflowDir ...string) (*Server, error) {
	dir := ""
	if len(workflowDir) > 0 {
		dir = workflowDir[0]
	}
	var registry *wfe.Registry
	var err error
	if dir != "" {
		registry, err = wfe.NewRegistry(dir)
		if err != nil {
			return nil, err
		}
	}
	s := &Server{db: db, artifacts: artifacts, workflowDir: dir, workflows: registry, mux: http.NewServeMux(), triggerErrors: make(map[string]string)}
	s.mux.HandleFunc("GET /v1/health", s.health)
	s.mux.HandleFunc("GET /v1/workflow/items", s.items)
	s.mux.HandleFunc("GET /v1/workflow/items/all", s.items)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}", s.item)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}/events", s.events)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}/proposal", s.proposal)
	s.mux.HandleFunc("POST /v1/workflow/items/{id}/pause", s.workflowPause)
	s.mux.HandleFunc("POST /v1/workflow/items/{id}/resume", s.workflowResume)
	s.mux.HandleFunc("POST /v1/workflow/items/{id}/gate", s.workflowGate)
	s.mux.HandleFunc("POST /v1/workflow/items/{id}/stop", s.workflowStop)
	s.mux.HandleFunc("DELETE /v1/workflow/items/{id}", s.workflowDelete)
	s.mux.HandleFunc("GET /v1/workflow/blocks", s.workflowBlocks)
	s.mux.HandleFunc("PUT /v1/workflow/blocks/{name}", s.workflowBlockPut)
	s.mux.HandleFunc("DELETE /v1/workflow/blocks/{name}", s.workflowBlockDelete)
	s.mux.HandleFunc("GET /v1/workflow/defs", s.workflowDefinitions)
	s.mux.HandleFunc("GET /v1/workflow/defs/{name}", s.workflowDefinition)
	s.mux.HandleFunc("POST /v1/workflow/validate", s.workflowValidate)
	s.mux.HandleFunc("POST /v1/workflow/save", s.workflowSave)
	s.mux.HandleFunc("GET /v1/workflow/triggers", s.workflowTriggers)
	s.mux.HandleFunc("GET /v1/workflow/config", s.configGet)
	s.mux.HandleFunc("POST /v1/workflow/config/set", s.configSet)
	s.mux.HandleFunc("GET /v1/config", s.configGet)
	s.mux.HandleFunc("POST /v1/config/set", s.configSet)
	s.mux.HandleFunc("POST /v1/trigger/fire", s.triggerFire)
	s.mux.HandleFunc("POST /v1/dev/submit", s.devSubmit)
	return s, nil
}

func (s *Server) SetSchedulerNotify(notify func())       { s.notify = notify }
func (s *Server) SetSchedulerCancel(cancel func(string)) { s.cancel = cancel }
func (s *Server) SetWorktreeCleanup(cleanup func(context.Context, db1.WorkItem) error) {
	s.cleanupWorktree = cleanup
}
func (s *Server) SetConfigStore(store *appconfig.Store) { s.config = store }
func (s *Server) workflowRegistry() (*wfe.Registry, error) {
	if s.workflows != nil {
		return s.workflows, nil
	}
	registry, err := wfe.NewRegistry(s.workflowDir)
	if err != nil {
		return nil, err
	}
	s.workflows = registry
	return registry, nil
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mux.ServeHTTP(w, r)
}

func RequireBearer(next http.Handler, token string) http.Handler {
	if token == "" {
		return next
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		provided := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		if len(provided) != len(token) || subtle.ConstantTimeCompare([]byte(provided), []byte(token)) != 1 {
			writeError(w, http.StatusUnauthorized, errors.New("unauthorized"))
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) health(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "aimee-server", "implementation": "go"})
}

var errWorkflowAccessDenied = errors.New("workflow item belongs to another user")

func workflowPrincipal(r *http.Request) string {
	return strings.TrimSpace(r.Header.Get("X-Aimee-Webuser"))
}

// workflowOperator is a capability attested separately from the username by
// the trusted runtime. A username is data, not a role: treating the literal
// string "admin" as authority lets a non-bootstrap account collide with the
// control-plane sentinel after the appliance administrator is renamed.
func workflowOperator(r *http.Request) bool {
	return r.Header.Get("X-Aimee-Workflow-Operator") == "true"
}

// rootSubmitter follows durable parent links instead of trusting child IDs or a
// child row's submitter. Slice rows created by older engines did not copy the
// submitter, but they still belong to the principal that admitted the root run.
func (s *Server) rootSubmitter(ctx context.Context, item db1.WorkItem) (string, error) {
	seen := make(map[string]struct{})
	for {
		if _, duplicate := seen[item.ID]; duplicate {
			return "", errors.New("workflow parent cycle")
		}
		seen[item.ID] = struct{}{}
		if item.ParentID == "" {
			return item.Submitter, nil
		}
		parent, err := s.db.WorkItem(ctx, item.ParentID)
		if err != nil {
			return "", err
		}
		item = parent
	}
}

func rootSubmitterFromList(item db1.WorkItem, byID map[string]db1.WorkItem) (string, bool) {
	seen := make(map[string]struct{})
	for {
		if _, duplicate := seen[item.ID]; duplicate {
			return "", false
		}
		seen[item.ID] = struct{}{}
		if item.ParentID == "" {
			return item.Submitter, true
		}
		parent, ok := byID[item.ParentID]
		if !ok {
			return "", false
		}
		item = parent
	}
}

// authorizedWorkItem is the shared ownership boundary for detail, artifact,
// timeline, and lifecycle endpoints. The trusted runtime attests the appliance
// administrator as a separate capability. Human gates are operator-only; other
// lifecycle actions allow the root owner or the operator.
func (s *Server) authorizedWorkItem(r *http.Request, operatorOnly bool) (db1.WorkItem, error) {
	item, err := s.db.WorkItem(r.Context(), r.PathValue("id"))
	if err != nil {
		return db1.WorkItem{}, err
	}
	principal := workflowPrincipal(r)
	if workflowOperator(r) {
		return item, nil
	}
	if operatorOnly || principal == "" {
		return db1.WorkItem{}, errWorkflowAccessDenied
	}
	owner, err := s.rootSubmitter(r.Context(), item)
	if err != nil {
		return db1.WorkItem{}, err
	}
	if owner == "" || owner != principal {
		return db1.WorkItem{}, errWorkflowAccessDenied
	}
	return item, nil
}

func writeWorkItemAccessError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, errWorkflowAccessDenied):
		writeError(w, http.StatusForbidden, errWorkflowAccessDenied)
	case errors.Is(err, sql.ErrNoRows) || strings.Contains(err.Error(), "no rows"):
		writeError(w, http.StatusNotFound, errors.New("work item not found"))
	default:
		writeError(w, http.StatusInternalServerError, err)
	}
}

func (s *Server) items(w http.ResponseWriter, r *http.Request) {
	all := r.URL.Path == "/v1/workflow/items/all"
	if all && !workflowOperator(r) {
		writeError(w, http.StatusForbidden, errors.New("operator access required"))
		return
	}
	items, err := s.db.WorkItems(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	principal := workflowPrincipal(r)
	if !all {
		byID := make(map[string]db1.WorkItem, len(items))
		for _, item := range items {
			byID[item.ID] = item
		}
		visible := make([]db1.WorkItem, 0, len(items))
		for _, item := range items {
			owner, ok := rootSubmitterFromList(item, byID)
			if ok && owner != "" && owner == principal {
				visible = append(visible, item)
			}
		}
		items = visible
	}
	if items == nil {
		items = []db1.WorkItem{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (s *Server) item(w http.ResponseWriter, r *http.Request) {
	item, err := s.authorizedWorkItem(r, false)
	if err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (s *Server) events(w http.ResponseWriter, r *http.Request) {
	if _, err := s.authorizedWorkItem(r, false); err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit < 1 || limit > 200 {
		limit = 200
	}
	events, err := s.db.Events(r.Context(), r.PathValue("id"), after, limit)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	next := after
	if len(events) > 0 {
		next = events[len(events)-1].ID
	}
	if events == nil {
		events = []db1.Event{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"events": events, "next_after": next})
}

func (s *Server) proposal(w http.ResponseWriter, r *http.Request) {
	item, err := s.authorizedWorkItem(r, false)
	if err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	content, err := s.artifacts.Proposal(item.ID)
	if err != nil && item.ProposalPath != "" {
		// One-time import supports current DB1 rows during migration. PutProposal
		// makes the imported copy immutable; subsequent reads never depend on a
		// mutable repository path.
		if importErr := s.artifacts.ImportProposal(item.ID, item.ProposalPath); importErr == nil {
			content, err = s.artifacts.Proposal(item.ID)
		}
	}
	if err != nil {
		writeError(w, http.StatusNotFound, errors.New("proposal not found"))
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"proposal_md":   string(content),
		"truncated":     false,
		"proposal_name": filepath.Base(item.ProposalPath),
	})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func writeError(w http.ResponseWriter, status int, err error) {
	writeJSON(w, status, map[string]any{"ok": false, "error": err.Error()})
}
