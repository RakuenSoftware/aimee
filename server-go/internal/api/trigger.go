package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os/exec"
	"path"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

// ScanTriggers evaluates the live UI/config trigger registry and graph-native
// trigger.watch-dir starts. It is safe to call on every scheduler tick: filing
// uses immutable git identities and DB1 deduplication.
func (s *Server) ScanTriggers(ctx context.Context) {
	for _, request := range s.triggerRequests() {
		if request.Error != "" {
			continue
		}
		if err := s.scanTrigger(ctx, request); err != nil {
			s.setTriggerError(request, err.Error())
			slog.Error("scan workflow trigger", "workspace", request.Workspace, "workflow", request.Pipeline, "error", err)
		} else {
			s.setTriggerError(request, "")
		}
	}
}

func triggerKey(request triggerFireRequest) string {
	return request.Origin + "\x00" + request.Source + "\x00" + request.Workspace + "\x00" + request.Pipeline + "\x00" + request.Event + "\x00" + request.Ref
}
func (s *Server) setTriggerError(request triggerFireRequest, message string) {
	s.triggerErrorsMu.Lock()
	defer s.triggerErrorsMu.Unlock()
	if message == "" {
		delete(s.triggerErrors, triggerKey(request))
	} else {
		s.triggerErrors[triggerKey(request)] = message
	}
}
func (s *Server) triggerError(request triggerFireRequest) string {
	s.triggerErrorsMu.Lock()
	defer s.triggerErrorsMu.Unlock()
	return s.triggerErrors[triggerKey(request)]
}

func (s *Server) triggerRequests() []triggerFireRequest {
	var out []triggerFireRequest
	if s.config != nil {
		if rules, err := s.config.TriggerRules(); err != nil {
			slog.Error("load workflow trigger rules", "error", err)
		} else {
			for _, rule := range rules {
				if rule.Source != "watch-dir" && rule.Source != "proposals" {
					out = append(out, triggerFireRequest{Source: rule.Source, Event: rule.Event, Ref: rule.Schedule,
						Pipeline: rule.Pipeline.Template, Workspace: rule.Pipeline.Workspace, Mode: rule.Mode,
						Origin: "config", Error: "source is not supported by the Go WFE trigger scanner"})
					continue
				}
				out = append(out, triggerFireRequest{Source: rule.Source, Event: rule.Event,
					Workspace: rule.Pipeline.Workspace, Ref: rule.Schedule,
					Pipeline: rule.Pipeline.Template, Mode: rule.Mode, MaxSpend: rule.Pipeline.MaxSpendUSD, Origin: "config"})
			}
		}
	}
	if registry, err := s.workflowRegistry(); err == nil {
		if definitions, listErr := registry.List(); listErr == nil {
			for _, summary := range definitions {
				definition, loadErr := registry.Load(summary.Name)
				if loadErr != nil {
					continue
				}
				start, ok := definition.Node(definition.Start)
				if !ok && len(definition.Nodes) > 0 {
					start = definition.Nodes[0]
					ok = true
				}
				if !ok || start.Block != "trigger.watch-dir" {
					continue
				}
				workspace := paramText(start.Params, "workspace")
				if workspace == "" {
					continue
				}
				out = append(out, triggerFireRequest{Source: "watch-dir",
					Event:     paramDefault(start.Params, "dir", "docs/proposals/pending"),
					Workspace: workspace, Ref: paramText(start.Params, "ref"), Pipeline: definition.Name,
					Mode: paramDefault(start.Params, "mode", "autonomous"), MaxSpend: paramFloat(start.Params, "max_spend_usd"), Origin: "workflow"})
			}
		}
	}
	return out
}

func (s *Server) scanTrigger(ctx context.Context, request triggerFireRequest) error {
	ref, err := refreshScanRef(ctx, request.Workspace, request.Ref)
	if err != nil {
		return err
	}
	request.Ref = ref
	directory := request.Event
	if directory == "" {
		directory = "docs/proposals/pending"
	}
	listing, err := gitOutput(ctx, request.Workspace, "ls-tree", "-r", "--name-only", request.Ref, "--", directory)
	if err != nil {
		return err
	}
	for _, candidate := range strings.Split(string(listing), "\n") {
		if !autoProposalCandidate(candidate) {
			continue
		}
		request.Proposal = candidate
		_, _, fileErr := s.fileProposal(ctx, request)
		if fileErr == nil {
			if s.notify != nil {
				s.notify()
			}
			continue
		}
		if strings.Contains(fileErr.Error(), "already filed") {
			continue
		}
		if strings.Contains(fileErr.Error(), "admission full") {
			// Every candidate remains eligible on the next tick. Continue so one
			// full/invalid rule never prevents later rules from being inspected.
			continue
		}
		return fileErr
	}
	return nil
}

func autoProposalCandidate(candidate string) bool {
	if candidate == "" || !strings.EqualFold(path.Ext(candidate), ".md") {
		return false
	}
	for _, component := range strings.Split(candidate, "/") {
		if strings.HasPrefix(component, ".") {
			return false
		}
	}
	return true
}

func refreshScanRef(ctx context.Context, workspace, configured string) (string, error) {
	if _, err := gitOutput(ctx, workspace, "remote", "get-url", "origin"); err != nil {
		if configured != "" {
			return configured, nil
		}
		return "HEAD", nil
	}
	if _, err := gitOutput(ctx, workspace, "fetch", "--quiet", "--prune", "origin"); err != nil {
		return "", fmt.Errorf("refresh origin before proposal scan: %w", err)
	}
	if configured == "" {
		ref, err := gitOutput(ctx, workspace, "symbolic-ref", "--short", "refs/remotes/origin/HEAD")
		if err != nil || strings.TrimSpace(string(ref)) == "" {
			if _, autoErr := gitOutput(ctx, workspace, "remote", "set-head", "origin", "--auto"); autoErr != nil {
				return "", fmt.Errorf("resolve refreshed origin default branch: %w", autoErr)
			}
			ref, err = gitOutput(ctx, workspace, "symbolic-ref", "--short", "refs/remotes/origin/HEAD")
			if err != nil || strings.TrimSpace(string(ref)) == "" {
				return "", errors.New("refreshed origin default branch is unresolved")
			}
		}
		remote := strings.TrimSpace(string(ref))
		if _, err := gitOutput(ctx, workspace, "rev-parse", "--verify", remote+"^{commit}"); err != nil {
			return "", fmt.Errorf("resolve refreshed default branch %q: %w", remote, err)
		}
		return remote, nil
	}
	// Fully-qualified refs and immutable commit IDs retain their exact
	// UI-authored meaning. Every other value is an ordinary remote branch name,
	// including names with slashes, and must resolve on the freshly fetched
	// origin rather than silently falling back to a stale local branch.
	if strings.HasPrefix(configured, "refs/") || fullCommitID(configured) {
		return configured, nil
	}
	remote := configured
	if !strings.HasPrefix(remote, "origin/") {
		remote = "origin/" + remote
	}
	if _, err := gitOutput(ctx, workspace, "rev-parse", "--verify", remote+"^{commit}"); err != nil {
		return "", fmt.Errorf("resolve refreshed branch %q: %w", remote, err)
	}
	return remote, nil
}

func fullCommitID(value string) bool {
	if len(value) != 40 && len(value) != 64 {
		return false
	}
	for _, r := range value {
		if !((r >= '0' && r <= '9') || (r >= 'a' && r <= 'f') || (r >= 'A' && r <= 'F')) {
			return false
		}
	}
	return true
}

func paramText(params map[string]any, key string) string {
	value, _ := params[key].(string)
	return value
}

func paramDefault(params map[string]any, key, fallback string) string {
	if value := paramText(params, key); value != "" {
		return value
	}
	return fallback
}

func paramFloat(params map[string]any, key string) float64 {
	switch value := params[key].(type) {
	case float64:
		return value
	case int:
		return float64(value)
	default:
		return 0
	}
}

type triggerFireRequest struct {
	Source    string  `json:"source"`
	Proposal  string  `json:"proposal"`
	Workspace string  `json:"workspace"`
	Ref       string  `json:"ref"`
	Pipeline  string  `json:"pipeline"`
	Mode      string  `json:"mode"`
	Event     string  `json:"event"`
	MaxSpend  float64 `json:"max_spend_usd,omitempty"`
	Origin    string  `json:"-"`
	Error     string  `json:"-"`
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
	registry, err := s.workflowRegistry()
	if err != nil {
		return "", "", err
	}
	definition, err := registry.Pin(request.Pipeline)
	if err != nil {
		return "", "", err
	}
	identity := fmt.Sprintf("git:%s:%s:%s:%s:%s", commit, proposalPath, wfe.Hash(content),
		request.Pipeline, request.Mode)
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
	cap := 2
	if s.config != nil {
		cap = s.config.Int("trigger.max_concurrent", cap)
	}
	if err := s.db.AdmitRoot(ctx, db1.CreateWorkItem{
		ID: workItemID, Repo: workspace, ProposalPath: identity, WorkflowName: definition.Name,
		WorkflowVersion: definition.Version, StartStage: start, Mode: request.Mode,
		SourcePath: proposalPath, MaxCostUSD: request.MaxSpend,
	}, cap); err != nil {
		_ = s.artifacts.DeleteWorkItem(workItemID)
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			if existing, findErr := s.db.WorkItemByProposal(ctx, workspace, identity); findErr == nil {
				return "", "", fmt.Errorf("proposal already filed as %s", existing.ID)
			}
		}
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
