package api

import (
	"database/sql"
	"errors"
	"fmt"
	"io"
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
	decoder := jsonDecoder(r.Body)
	if err := decoder.Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, errors.New("request must contain one JSON value"))
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
		// Idempotency belongs to the authenticated submitter, not merely the repo.
		// Otherwise two browser users choosing the same client key would cause the
		// second request to return the first user's work-item ID and silently skip
		// the second proposal.
		identity = manualSubmissionIdentity(workflowPrincipal(r), idempotencyKey, request.Workflow)
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
	// See scanTrigger: the store treats <=0 as unlimited (child slices depend on
	// that), so a configured 0 must be refused here or "pause admission" would
	// instead remove the limit.
	if cap == 0 {
		writeError(w, http.StatusConflict, errors.New("admission paused: trigger.max_concurrent is 0"))
		return
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
		StartStage: start, Mode: "autonomous", Submitter: workflowPrincipal(r)}, cap); err != nil {
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

func manualSubmissionIdentity(submitter, idempotencyKey, workflow string) string {
	scope := submitter + "\x00" + idempotencyKey
	return fmt.Sprintf("manual:%s:%s", wfe.Hash([]byte(scope)), workflow)
}
