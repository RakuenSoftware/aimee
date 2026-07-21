package api

import (
	"crypto/subtle"
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type Server struct {
	db          *db1.Store
	artifacts   *wfe.ArtifactStore
	workflowDir string
	mux         *http.ServeMux
	notify      func()
	triggerMu   sync.Mutex
}

func New(db *db1.Store, artifacts *wfe.ArtifactStore, workflowDir ...string) *Server {
	dir := ""
	if len(workflowDir) > 0 {
		dir = workflowDir[0]
	}
	s := &Server{db: db, artifacts: artifacts, workflowDir: dir, mux: http.NewServeMux()}
	s.mux.HandleFunc("GET /v1/health", s.health)
	s.mux.HandleFunc("GET /v1/workflow/items", s.items)
	s.mux.HandleFunc("GET /v1/workflow/items/all", s.items)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}", s.item)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}/events", s.events)
	s.mux.HandleFunc("GET /v1/workflow/items/{id}/proposal", s.proposal)
	s.mux.HandleFunc("POST /v1/trigger/fire", s.triggerFire)
	return s
}

func (s *Server) SetSchedulerNotify(notify func()) { s.notify = notify }

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

func (s *Server) items(w http.ResponseWriter, r *http.Request) {
	items, err := s.db.WorkItems(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	if items == nil {
		items = []db1.WorkItem{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (s *Server) item(w http.ResponseWriter, r *http.Request) {
	item, err := s.db.WorkItem(r.Context(), r.PathValue("id"))
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) || strings.Contains(err.Error(), "no rows") {
			writeError(w, http.StatusNotFound, errors.New("work item not found"))
			return
		}
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (s *Server) events(w http.ResponseWriter, r *http.Request) {
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
	item, err := s.db.WorkItem(r.Context(), r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, errors.New("work item not found"))
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
