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
	return New(store, artifacts), store, artifacts
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

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	command := exec.Command("git", append([]string{"-C", dir}, args...)...)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("git %s: %v: %s", strings.Join(args, " "), err, output)
	}
}
