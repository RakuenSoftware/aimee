package engine

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

// gitRun runs a git command in dir, failing the test on error.
func gitRun(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	cmd.Env = append(os.Environ(),
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@e",
		"GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@e")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v: %s", args, err, out)
	}
	return string(out)
}

// setupSliceRepo builds a repo with a feature branch aimee/feat/wi_parent and a
// slice worktree on aimee/wi/wi_child cut from it. Returns (repo, slicedir).
func setupSliceRepo(t *testing.T) (string, string) {
	t.Helper()
	repo := t.TempDir()
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "branch", "aimee/feat/wi_parent", "trunk")
	slicedir := filepath.Join(t.TempDir(), "slice")
	gitRun(t, repo, "worktree", "add", slicedir, "-b", "aimee/wi/wi_child", "aimee/feat/wi_parent")
	return repo, slicedir
}

// advanceFeature commits a file onto aimee/feat/wi_parent in the main repo,
// simulating a sibling slice merging into the feature branch.
func advanceFeature(t *testing.T, repo, name, content string) {
	t.Helper()
	gitRun(t, repo, "checkout", "aimee/feat/wi_parent")
	if err := os.WriteFile(filepath.Join(repo, name), []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", name)
	gitRun(t, repo, "commit", "-m", "sibling: "+name)
	gitRun(t, repo, "checkout", "trunk")
}

func TestIntegrateFeatureBasePicksUpSiblingMerge(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)
	// A sibling slice merges a prerequisite into the feature branch AFTER our
	// worktree was cut.
	advanceFeature(t, repo, "prereq.go", "package prereq\n")

	// The prerequisite is not visible in the slice worktree yet.
	if _, err := os.Stat(filepath.Join(slicedir, "prereq.go")); !os.IsNotExist(err) {
		t.Fatalf("prereq.go should be absent before integration, stat err=%v", err)
	}

	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil {
		t.Fatalf("integrateFeatureBase: %v", err)
	}
	if park != "" {
		t.Fatalf("expected clean integration, got park=%q", park)
	}
	// The prerequisite is now present — the dependent slice can proceed.
	if _, err := os.Stat(filepath.Join(slicedir, "prereq.go")); err != nil {
		t.Fatalf("prereq.go should be present after integration: %v", err)
	}
}

func TestIntegrateFeatureBaseNoopWhenAlreadyCurrent(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)
	_ = repo
	// Base == HEAD's ancestor already: integration is a no-op, no park.
	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil || park != "" {
		t.Fatalf("expected clean no-op, got park=%q err=%v", park, err)
	}
}

func TestIntegrateFeatureBaseConflictAbortsCleanly(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)

	// The slice edits shared.go one way...
	if err := os.WriteFile(filepath.Join(slicedir, "shared.go"), []byte("package a // slice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "shared.go")
	gitRun(t, slicedir, "commit", "-m", "slice edit")

	// ...while a sibling edits the same file the other way on the feature branch.
	advanceFeature(t, repo, "shared.go", "package a // sibling\n")

	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil {
		t.Fatalf("integrateFeatureBase returned err: %v", err)
	}
	if park != "base_integration_conflict" {
		t.Fatalf("expected base_integration_conflict, got %q", park)
	}
	// The worktree must be left clean — never half-merged.
	if status := gitRun(t, slicedir, "status", "--porcelain"); status != "" {
		t.Fatalf("worktree not clean after conflict abort: %q", status)
	}
	// No merge must be in progress (MERGE_HEAD absent).
	cmd := exec.Command("git", "-C", slicedir, "rev-parse", "-q", "--verify", "MERGE_HEAD")
	if err := cmd.Run(); err == nil {
		t.Fatal("MERGE_HEAD still present: merge was not aborted")
	}
	// The slice's own commit survives.
	if _, err := os.Stat(filepath.Join(slicedir, "shared.go")); err != nil {
		t.Fatalf("slice's own shared.go should survive the abort: %v", err)
	}
}

// A delegate that reports partial and writes nothing must fail its slice, not
// advance it. Observed on wi_3d5de168: every implement job came back
//
//	partial — "named file 'docs/runbooks/appliance-state-recovery.md' was not
//	created by delegate ... The task remains unimplemented"
//
// and the engine advanced anyway. freeze then saw an empty diff, read it as "the
// work is already in the base", and accepted the slice — so the run reached
// done=5 with no commits, no file and no PR, recorded as success.
type partialNoCommitAgents struct{}

func (partialNoCommitAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	return DelegateResult{
		Response: "named file 'docs/runbooks/x.md' was not created by delegate; the task remains unimplemented",
		Partial:  true,
	}, nil
}

func (a partialNoCommitAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	out := make([]DelegateGroupResult, len(requests))
	for i := range requests {
		out[i] = DelegateGroupResult{Response: "partial"}
	}
	return out
}

func TestPartialImplementWithNoCommitDoesNotAdvance(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/trunk", "HEAD")
	gitRun(t, repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/trunk")

	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	parent, _ := store.WorkItem(ctx, "wi_parent")
	if _, _, err := manager.Ensure(ctx, parent, true); err != nil {
		t.Fatal(err)
	}
	child, _ := store.WorkItem(ctx, "wi_child")

	runner := &NativeRunner{agents: partialNoCommitAgents{}, worktrees: manager, db: store}
	// docs=true: the verifier is skipped on a documentation slice, which is how
	// this reached freeze with nothing in the worktree.
	out, err := runner.mutate(ctx, StepRequest{WorkItem: child, Node: wfe.Node{ID: "impl"}}, true)
	if err != nil {
		t.Fatalf("mutate: %v", err)
	}
	if out.Status == StepAdvanced {
		t.Fatal("a partial delegate that produced no commit must not advance the slice")
	}
	if out.Status != StepChanges {
		t.Fatalf("expected StepChanges, got %q", out.Status)
	}
	// The delegate already said exactly what was wrong; that is the finding.
	if !strings.Contains(out.Detail, "was not created by delegate") {
		t.Fatalf("delegate's own diagnosis must survive into the step detail: %q", out.Detail)
	}
}
