package api

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/wfe"
	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

func (s *Server) devSubmit(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Proposal   string `json:"proposal_md"`
		Workflow   string `json:"workflow"`
		Repo       string `json:"repo"`
		SourcePath string `json:"source_path"`
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
	sourcePath, err := validateManualProposalSource(r.Context(), repo, request.SourcePath, request.Proposal)
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
			writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": existing.ID,
				"workflow": existing.WorkflowName, "state": existing.State, "deduplicated": true})
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
	if err := s.db.AdmitRoot(r.Context(), workflowstore.CreateWorkItem{ID: id, Repo: repo,
		ProposalPath: identity, WorkflowName: definition.Name, WorkflowVersion: definition.Version,
		StartStage: start, Mode: "autonomous", Submitter: workflowPrincipal(r), SourcePath: sourcePath}, cap); err != nil {
		_ = s.artifacts.DeleteWorkItem(id)
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			if existing, findErr := s.db.WorkItemByProposal(r.Context(), repo, identity); findErr == nil {
				writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": existing.ID,
					"workflow": existing.WorkflowName, "state": existing.State, "deduplicated": true})
				return
			}
		}
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	// work_item_id, workflow and state, because that is the shape the CLI has
	// always printed. The C intake this replaced returned all three; this one
	// returned the id alone, so `aimee workflow run` printed a filled id and then
	// two empty lines -- on every run, for anyone who used the command.
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "work_item_id": id,
		"workflow": definition.Name, "state": "active"})
}

func validateManualProposalSource(ctx context.Context, repo, source, proposal string) (string, error) {
	if source == "" {
		return "", nil
	}
	if source != strings.TrimSpace(source) || filepath.IsAbs(source) || strings.Contains(source, `\`) ||
		strings.IndexFunc(source, func(r rune) bool {
			return !(r >= 'a' && r <= 'z') && !(r >= 'A' && r <= 'Z') &&
				!(r >= '0' && r <= '9') && !strings.ContainsRune("._-/", r)
		}) >= 0 {
		return "", errors.New("source_path must be a plain repository-relative proposal path")
	}
	clean := filepath.Clean(source)
	const pending = "docs/proposals/pending/"
	if clean != source || !strings.HasPrefix(clean, pending) || clean == pending || filepath.Ext(clean) != ".md" {
		return "", errors.New("source_path must name a Markdown file under docs/proposals/pending")
	}
	sourceContent, err := gitOutput(ctx, repo, "show", "HEAD:"+clean)
	if err != nil {
		return "", fmt.Errorf("read source_path from repository HEAD: %w", err)
	}
	if !sameProposalExceptLifecycleState(string(sourceContent), proposal) {
		return "", errors.New("source_path does not identify the submitted proposal at repository HEAD")
	}
	return clean, nil
}

func sameProposalExceptLifecycleState(source, submitted string) bool {
	if source == submitted {
		return true
	}
	sourceLines := strings.Split(source, "\n")
	submittedLines := strings.Split(submitted, "\n")
	if len(sourceLines) != len(submittedLines) {
		return false
	}
	stateDifference := false
	for i := range sourceLines {
		if sourceLines[i] == submittedLines[i] {
			continue
		}
		if stateDifference || !strings.HasPrefix(sourceLines[i], "- **State:**") ||
			!strings.HasPrefix(submittedLines[i], "- **State:**") {
			return false
		}
		stateDifference = true
	}
	return stateDifference
}

func manualSubmissionIdentity(submitter, idempotencyKey, workflow string) string {
	scope := submitter + "\x00" + idempotencyKey
	return fmt.Sprintf("manual:%s:%s", wfe.Hash([]byte(scope)), workflow)
}
