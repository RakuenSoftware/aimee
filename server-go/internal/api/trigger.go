package api

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os/exec"
	"path"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type triggerFireRequest struct {
	Source    string `json:"source"`
	Proposal  string `json:"proposal"`
	Workspace string `json:"workspace"`
	Ref       string `json:"ref"`
	Pipeline  string `json:"pipeline"`
	Mode      string `json:"mode"`
	Event     string `json:"event"`
}

func (s *Server) triggerFire(w http.ResponseWriter, r *http.Request) {
	var request triggerFireRequest
	if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, fmt.Errorf("decode trigger: %w", err))
		return
	}
	if request.Source != "proposals" && request.Source != "watch-dir" {
		writeError(w, http.StatusBadRequest, errors.New("this Go vertical slice accepts proposals/watch-dir triggers"))
		return
	}
	if request.Workspace == "" || request.Proposal == "" {
		writeError(w, http.StatusBadRequest, errors.New("workspace and proposal are required"))
		return
	}
	if request.Ref == "" {
		request.Ref = "HEAD"
	}
	if request.Pipeline == "" {
		request.Pipeline = "build"
	}
	if request.Mode == "" {
		request.Mode = "autonomous"
	}
	if s.workflowDir == "" {
		writeError(w, http.StatusServiceUnavailable, errors.New("workflow directory is not configured"))
		return
	}

	s.triggerMu.Lock()
	defer s.triggerMu.Unlock()
	workItemID, proposalPath, err := s.fileProposal(r.Context(), request)
	if err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok": true, "work_item_id": workItemID, "proposal": filepath.Base(proposalPath),
	})
}

func (s *Server) fileProposal(ctx context.Context, request triggerFireRequest) (string, string, error) {
	workspace, err := filepath.Abs(request.Workspace)
	if err != nil {
		return "", "", fmt.Errorf("resolve workspace: %w", err)
	}
	commitBytes, err := gitOutput(ctx, workspace, "rev-parse", "--verify", request.Ref+"^{commit}")
	if err != nil {
		return "", "", fmt.Errorf("resolve ref %q: %w", request.Ref, err)
	}
	commit := strings.TrimSpace(string(commitBytes))
	if len(commit) != 40 && len(commit) != 64 {
		return "", "", errors.New("git returned an invalid commit id")
	}
	directory := request.Event
	if directory == "" {
		directory = "docs/proposals/pending"
	}
	cleanDirectory := path.Clean(directory)
	if cleanDirectory == "." || strings.HasPrefix(cleanDirectory, "../") || path.IsAbs(cleanDirectory) {
		return "", "", errors.New("proposal directory must be a confined repository-relative path")
	}
	listing, err := gitOutput(ctx, workspace, "ls-tree", "-r", "--name-only", commit, "--", cleanDirectory)
	if err != nil {
		return "", "", fmt.Errorf("list pending proposals: %w", err)
	}
	proposalPath := ""
	for _, candidate := range strings.Split(string(listing), "\n") {
		if proposalMatches(candidate, request.Proposal) {
			if proposalPath != "" && proposalPath != candidate {
				return "", "", fmt.Errorf("proposal selector %q is ambiguous", request.Proposal)
			}
			proposalPath = candidate
		}
	}
	if proposalPath == "" {
		return "", "", fmt.Errorf("proposal %q not found at %s", request.Proposal, request.Ref)
	}
	content, err := gitOutput(ctx, workspace, "show", commit+":"+proposalPath)
	if err != nil {
		return "", "", fmt.Errorf("read proposal blob: %w", err)
	}
	definition, err := wfe.LoadDefinition(filepath.Join(s.workflowDir, request.Pipeline+".yaml"))
	if err != nil {
		return "", "", err
	}
	identity := fmt.Sprintf("git:%s:%s:%s:%s:%s", commit, proposalPath, wfe.Hash(content),
		request.Pipeline, request.Mode)
	if existing, err := s.db.WorkItemByProposal(ctx, workspace, identity); err == nil {
		return "", "", fmt.Errorf("proposal already filed as %s", existing.ID)
	} else if !errors.Is(err, sql.ErrNoRows) && !strings.Contains(err.Error(), "no rows") {
		return "", "", err
	}
	workItemID, err := mintWorkItemID()
	if err != nil {
		return "", "", err
	}
	if err := s.artifacts.PutProposal(workItemID, content); err != nil {
		return "", "", err
	}
	start := definition.Start
	if start == "" {
		start = definition.Nodes[0].ID
	}
	if err := s.db.CreateWorkItem(ctx, db1.CreateWorkItem{
		ID: workItemID, Repo: workspace, ProposalPath: identity, WorkflowName: definition.Name,
		WorkflowVersion: definition.Version, StartStage: start, Mode: request.Mode,
	}); err != nil {
		return "", "", err
	}
	return workItemID, proposalPath, nil
}

func gitOutput(ctx context.Context, workspace string, args ...string) ([]byte, error) {
	commandArgs := append([]string{"-C", workspace}, args...)
	command := exec.CommandContext(ctx, "git", commandArgs...)
	output, err := command.Output() // bytes.Buffer grows; no artificial output ceiling.
	if err != nil {
		if exitErr := new(exec.ExitError); errors.As(err, &exitErr) {
			return nil, fmt.Errorf("git %s: %s", strings.Join(args, " "), strings.TrimSpace(string(exitErr.Stderr)))
		}
		return nil, err
	}
	return output, nil
}

func proposalMatches(candidate, requested string) bool {
	if candidate == "" || requested == "" {
		return false
	}
	if candidate == requested || path.Base(candidate) == requested {
		return true
	}
	base := path.Base(candidate)
	return strings.HasSuffix(base, ".md") && strings.TrimSuffix(base, ".md") == requested
}

func mintWorkItemID() (string, error) {
	var random [16]byte
	if _, err := rand.Read(random[:]); err != nil {
		return "", fmt.Errorf("mint work-item id: %w", err)
	}
	return "wi_" + hex.EncodeToString(random[:]), nil
}
