package api

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	appconfig "github.com/JBailes/aimee/server-go/internal/config"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func newTestServer(t *testing.T) (*Server, *db1.Store, *wfe.ArtifactStore) {
	t.Helper()
	root := t.TempDir()
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	server, err := New(store, artifacts)
	if err != nil {
		t.Fatal(err)
	}
	return server, store, artifacts
}

func TestProposalEndpointImportsLegacySourceWithoutTruncation(t *testing.T) {
	server, store, _ := newTestServer(t)
	tail := "ACCEPTANCE_CRITERION_AFTER_ALL_PRIOR_BYTE_LIMITS"
	proposal := strings.Repeat("complete proposal paragraph λ\n", 200_000) + tail
	source := filepath.Join(t.TempDir(), "proposal.md")
	if err := os.WriteFile(source, []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
		ID: "wi_api", Repo: "repo", ProposalPath: source, WorkflowName: "build",
		WorkflowVersion: strings.Repeat("a", 64), StartStage: "plan", Mode: "autonomous",
	}); err != nil {
		t.Fatal(err)
	}

	req := httptest.NewRequest(http.MethodGet, "/v1/workflow/items/wi_api/proposal", nil)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		Proposal  string `json:"proposal_md"`
		Truncated bool   `json:"truncated"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.Truncated || response.Proposal != proposal || !strings.HasSuffix(response.Proposal, tail) {
		t.Fatal("proposal endpoint did not return the complete immutable source")
	}

	// The imported copy is authoritative after the first request.
	if err := os.WriteFile(source, []byte("mutated legacy source"), 0o600); err != nil {
		t.Fatal(err)
	}
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.Proposal != proposal {
		t.Fatal("mutable legacy source changed the imported proposal")
	}
}

func TestHealthIdentifiesGoImplementation(t *testing.T) {
	server, _, _ := newTestServer(t)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"implementation":"go"`) {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestBearerAuthentication(t *testing.T) {
	server, _, _ := newTestServer(t)
	handler := RequireBearer(server, "secret-token")
	request := httptest.NewRequest(http.MethodGet, "/v1/health", nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("missing bearer status=%d", recorder.Code)
	}
	request = httptest.NewRequest(http.MethodGet, "/v1/health", nil)
	request.Header.Set("Authorization", "Bearer secret-token")
	recorder = httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("valid bearer status=%d body=%s", recorder.Code, recorder.Body.String())
	}
}

func TestProposalTriggerFilesCompleteGitBlobAndDeduplicates(t *testing.T) {
	server, store, artifacts := newTestServer(t)
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	workflow := `name: build
start: plan
nodes:
  - id: source
    block: author.proposal
    next: plan
  - id: plan
    block: author.plan
    in: {proposal: source.out}
`
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), []byte(workflow), 0o600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = workflowDir
	repo := filepath.Join(root, "repo")
	proposalDir := filepath.Join(repo, "docs", "proposals", "pending")
	if err := os.MkdirAll(proposalDir, 0o700); err != nil {
		t.Fatal(err)
	}
	tail := "TRIGGER_PROPOSAL_END"
	proposal := strings.Repeat("trigger proposal body λ\n", 180_000) + tail
	if err := os.WriteFile(filepath.Join(proposalDir, "large.md"), []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "init")
	runGit(t, repo, "config", "user.email", "test@example.invalid")
	runGit(t, repo, "config", "user.name", "Test")
	runGit(t, repo, "add", ".")
	runGit(t, repo, "commit", "-m", "proposal")

	body := []byte(`{"source":"proposals","proposal":"large","workspace":` +
		strconv.Quote(repo) + `,"ref":"HEAD","pipeline":"build","mode":"autonomous"}`)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		WorkItemID string `json:"work_item_id"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), response.WorkItemID)
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.Stage != "plan" {
		t.Fatalf("filed item=%+v", item)
	}
	got, err := artifacts.Proposal(response.WorkItemID)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != proposal || !strings.HasSuffix(string(got), tail) {
		t.Fatal("triggered proposal artifact was not preserved completely")
	}

	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusConflict || !strings.Contains(rec.Body.String(), "already filed") {
		t.Fatalf("duplicate status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestConfiguredTriggerScannerFilesPendingProposalWithoutManualFire(t *testing.T) {
	server, store, artifacts := newTestServer(t)
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), []byte("name: build\nstart: draft\nnodes:\n  - id: draft\n    block: author.proposal\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = workflowDir
	server.workflows = nil
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(filepath.Join(repo, "docs/proposals/pending"), 0o700); err != nil {
		t.Fatal(err)
	}
	proposal := "# Automatically discovered\n\ncomplete content\n"
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/auto.md"), []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/.gitkeep"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/notes.txt"), []byte("not a proposal"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "init")
	runGit(t, repo, "config", "user.email", "test@example.invalid")
	runGit(t, repo, "config", "user.name", "Test")
	runGit(t, repo, "add", ".")
	runGit(t, repo, "commit", "-m", "proposal")
	configPath := filepath.Join(root, "aimee.yaml")
	configText := "trigger:\n  max_concurrent: 2\ntrigger_rules:\n  - source: watch-dir\n    event: docs/proposals/pending\n    mode: autonomous\n    pipeline:\n      template: build\n      workspace: " + strconv.Quote(repo) + "\n"
	if err := os.WriteFile(configPath, []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	configStore, err := appconfig.NewStore(configPath)
	if err != nil {
		t.Fatal(err)
	}
	server.SetConfigStore(configStore)
	server.ScanTriggers(context.Background())
	items, err := store.WorkItems(context.Background())
	if err != nil || len(items) != 1 {
		t.Fatalf("items=%v err=%v", items, err)
	}
	got, err := artifacts.Proposal(items[0].ID)
	if err != nil || string(got) != proposal {
		t.Fatalf("proposal=%q err=%v", got, err)
	}
	server.ScanTriggers(context.Background())
	items, _ = store.WorkItems(context.Background())
	if len(items) != 1 {
		t.Fatalf("scanner duplicated proposal: %d items", len(items))
	}
}

func TestRefreshScanRefUsesNewRemoteBranchTip(t *testing.T) {
	root := t.TempDir()
	remote := filepath.Join(root, "remote.git")
	workspace := filepath.Join(root, "workspace")
	publisher := filepath.Join(root, "publisher")
	runGit(t, root, "init", "--bare", remote)
	runGit(t, root, "init", workspace)
	runGit(t, workspace, "config", "user.email", "test@example.invalid")
	runGit(t, workspace, "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(workspace, "seed"), []byte("seed"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, workspace, "add", ".")
	runGit(t, workspace, "commit", "-m", "seed")
	runGit(t, workspace, "branch", "-M", "testing")
	runGit(t, workspace, "remote", "add", "origin", remote)
	runGit(t, workspace, "push", "-u", "origin", "testing")
	runGit(t, remote, "symbolic-ref", "HEAD", "refs/heads/testing")
	runGit(t, root, "clone", "--branch", "testing", remote, publisher)
	runGit(t, publisher, "config", "user.email", "test@example.invalid")
	runGit(t, publisher, "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(publisher, "new-proposal"), []byte("new"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, publisher, "add", ".")
	runGit(t, publisher, "commit", "-m", "new proposal")
	runGit(t, publisher, "push", "origin", "testing")

	ref, err := refreshScanRef(t.Context(), workspace, "testing")
	if err != nil {
		t.Fatal(err)
	}
	if ref != "origin/testing" {
		t.Fatalf("ref=%q", ref)
	}
	listing, err := gitOutput(t.Context(), workspace, "ls-tree", "--name-only", ref)
	if err != nil || !strings.Contains(string(listing), "new-proposal") {
		t.Fatalf("listing=%q err=%v", listing, err)
	}
	localListing, err := gitOutput(t.Context(), workspace, "ls-tree", "--name-only", "testing")
	if err != nil || strings.Contains(string(localListing), "new-proposal") {
		t.Fatalf("local branch unexpectedly moved: %q err=%v", localListing, err)
	}
	defaultRef, err := refreshScanRef(t.Context(), workspace, "")
	if err != nil || defaultRef != "origin/testing" {
		t.Fatalf("default ref=%q err=%v", defaultRef, err)
	}
	if _, err := refreshScanRef(t.Context(), workspace, "missing"); err == nil {
		t.Fatal("missing remote branch silently fell back to a local ref")
	}
	runGit(t, publisher, "checkout", "-b", "feature/nested")
	runGit(t, publisher, "push", "origin", "feature/nested")
	nested, err := refreshScanRef(t.Context(), workspace, "feature/nested")
	if err != nil || nested != "origin/feature/nested" {
		t.Fatalf("nested ref=%q err=%v", nested, err)
	}
}

func TestAutoProposalCandidate(t *testing.T) {
	for candidate, want := range map[string]bool{
		"docs/proposals/pending/feature.md":         true,
		"docs/proposals/pending/FEATURE.MD":         true,
		"docs/proposals/pending/.gitkeep":           false,
		"docs/proposals/pending/notes.txt":          false,
		"docs/proposals/pending/.drafts/feature.md": false,
		"docs/.private/pending/feature.md":          false,
		"":                                          false,
	} {
		if got := autoProposalCandidate(candidate); got != want {
			t.Errorf("autoProposalCandidate(%q)=%v want=%v", candidate, got, want)
		}
	}
}

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	command := exec.Command("git", append([]string{"-C", dir}, args...)...)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("git %s: %v: %s", strings.Join(args, " "), err, output)
	}
}
