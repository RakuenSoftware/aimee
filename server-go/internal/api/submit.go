package api

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func (s *Server) devSubmit(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Proposal string `json:"proposal_md"`
		Workflow string `json:"workflow"`
		Repo     string `json:"repo"`
	}
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	if strings.TrimSpace(request.Proposal) == "" || request.Repo == "" {
		writeError(w, http.StatusBadRequest, errors.New("proposal_md and repo are required"))
		return
	}
	if request.Workflow == "" {
		request.Workflow = "build"
	}
	repo, err := filepath.Abs(request.Repo)
	if err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	idempotencyKey := strings.TrimSpace(r.Header.Get("Idempotency-Key"))
	if len(idempotencyKey) > 128 {
		writeError(w, http.StatusBadRequest, errors.New("Idempotency-Key is too long"))
		return
	}
	identity := ""
	if idempotencyKey != "" {
		identity = fmt.Sprintf("manual:%s:%s", wfe.Hash([]byte(idempotencyKey)), request.Workflow)
	}
	if identity != "" {
		if existing, findErr := s.db.WorkItemByProposal(r.Context(), repo, identity); findErr == nil {
			writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": existing.ID, "deduplicated": true})
			return
		} else if !errors.Is(findErr, sql.ErrNoRows) && !strings.Contains(findErr.Error(), "no rows") {
			writeError(w, http.StatusInternalServerError, findErr)
			return
		}
	}
	cap := 2
	if s.config != nil {
		cap = s.config.Int("trigger.max_concurrent", cap)
	}
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	definition, err := registry.Pin(request.Workflow)
	if err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	id, err := mintWorkItemID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	if identity == "" {
		identity = "manual-run:" + id
	}
	if err := s.artifacts.PutProposal(id, []byte(request.Proposal)); err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	start := definition.Start
	if start == "" {
		start = definition.Nodes[0].ID
	}
	if err := s.db.AdmitRoot(r.Context(), db1.CreateWorkItem{ID: id, Repo: repo,
		ProposalPath: identity, WorkflowName: definition.Name, WorkflowVersion: definition.Version,
		StartStage: start, Mode: "autonomous", Submitter: r.Header.Get("X-Aimee-Webuser")}, cap); err != nil {
		_ = s.artifacts.DeleteWorkItem(id)
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			if existing, findErr := s.db.WorkItemByProposal(r.Context(), repo, identity); findErr == nil {
				writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": existing.ID, "deduplicated": true})
				return
			}
		}
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": id})
}
