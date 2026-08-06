package engine

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

// setupFreezeCollisionHarness builds a parent plus two children: wi_s0 is the
// pre-frozen sibling (carries a frozen_diff artifact and a worktree on disk
// containing the file the frozen_diff claims was created) and wi_s1 is a
// second child that callers use as the "current" item under inspection, so
// wi_s0 is not skipped as self. The sibling's record can be mutated by
// callers to test missing-worktree scenarios.
func setupFreezeCollisionHarness(t *testing.T) (ctx context.Context, store *db1.Store, artifacts *wfe.ArtifactStore, registry *wfe.Registry, repo, slicedir, base, head string) {
	t.Helper()
	ctx = t.Context()

	repo, slicedir = setupSliceRepo(t)
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/aimee/feat/wi_parent", "aimee/feat/wi_parent")
	base = strings.TrimSpace(gitRun(t, slicedir, "rev-parse", featureBaseRef(ctx, slicedir, "wi_parent")))

	// Give the sibling a real on-disk "frozen" create: a new file "frozen.txt".
	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("frozen sibling blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "frozen create")
	head = strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	var err error
	store, err = db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })

	artifacts, err = wfe.NewArtifactStore(filepath.Join(t.TempDir(), "artifacts"))
	if err != nil {
		t.Fatal(err)
	}

	workflowDir := filepath.Join(t.TempDir(), "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: branch
nodes:
  - id: branch
    block: branch.open
    next: freeze
  - id: freeze
    block: freeze
    in: {branch: branch.out}
    next: gate
  - id: gate
    block: review
    in: {src: freeze.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	registry, err = wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}

	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_s0", Repo: repo, ProposalPath: "s0", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
		{ID: "wi_s1", Repo: repo, ProposalPath: "s1", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}

	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze", "frozen_diff", []byte("frozen.txt\n+frozen sibling blob\n")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "commit", []byte(head)); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-base", "commit", []byte(base)); err != nil {
		t.Fatal(err)
	}
	if err := store.Move(ctx, "wi_s0", "freeze", "gate", "advance", "", head, 0); err != nil {
		t.Fatal(err)
	}
	if err := store.SetWorktree(ctx, "wi_s0", slicedir); err != nil {
		t.Fatal(err)
	}
	return ctx, store, artifacts, registry, repo, slicedir, base, head
}

func TestRejectDivergentSiblingCreatesComparesEmptyWorktreeFromFreezeCommits(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "current.txt"), []byte("current slice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "current.txt")
	gitRun(t, slicedir, "commit", "-m", "current create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("missing-worktree sibling with freeze commits should still compare non-overlapping creates, got: %v", err)
	}
}

func TestRejectDivergentSiblingCreatesRejectsCollisionWithoutWorktree(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("conflicting slice blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
}

func TestRejectDivergentSiblingCreatesRejectsCollisionWithMissingWorktreeDir(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	missingDir := filepath.Join(t.TempDir(), "definitely-not-present")
	if err := store.SetWorktree(ctx, "wi_s0", missingDir); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("conflicting slice blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
}

// TestRejectDivergentSiblingCreatesStillRejectsWithWorktree guards the
// regression-sensitive behavior: when the sibling's worktree IS present and the
// current slice creates the same file with a different blob, the function must
// still raise freezeCreateCreateCollision. This proves the change did not
// disable divergence rejection for the readable-worktree case.
func TestRejectDivergentSiblingCreatesStillRejectsWithWorktree(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("conflicting slice blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
}

func TestRejectDivergentSiblingCreatesAllowsIdenticalCreateWithoutWorktree(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("frozen sibling blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "identical create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("identical create without sibling worktree should pass, got: %v", err)
	}
}

func TestRejectDivergentSiblingCreatesAllowsEditOnlyChange(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "README"), []byte("x\ncurrent edit\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "README")
	gitRun(t, slicedir, "commit", "-m", "edit existing")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("edit-only current slice should pass, got: %v", err)
	}
}

func TestRejectDivergentSiblingCreatesFailsClosedWithoutFreezeArtifacts(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "opaque", []byte("not a commit")); err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("conflicting slice blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
}

func assertFreezeCreateCollision(t *testing.T, err error, path, currentSlice, siblingSlice string) {
	t.Helper()
	if err == nil {
		t.Fatal("expected freezeCreateCreateCollision error, got nil")
	}
	for _, want := range []string{freezeCreateCreateCollision, path, currentSlice, siblingSlice} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("expected collision error containing %q, got: %v", want, err)
		}
	}
}

// TestRejectDivergentSiblingCreatesFailsClosedWithoutWorkflowRegistry proves the
// fail-closed invariant: a NativeRunner constructed without a workflow registry
// must surface a non-nil error from rejectDivergentSiblingCreates when the
// current slice would otherwise need a sibling-collision check. A silent return
// would let colliding sibling freezes pass undetected.
func TestRejectDivergentSiblingCreatesFailsClosedWithoutWorkflowRegistry(t *testing.T) {
	ctx, store, artifacts, _, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("conflicting slice blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "frozen.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent create")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	// Deliberately omit workflows: the runner cannot inspect sibling workflow
	// stages, so the collision check must fail closed rather than pass silently.
	runner := &NativeRunner{db: store, artifacts: artifacts}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	if err == nil {
		t.Fatal("expected workflow-registry error from workflow-less runner, got nil")
	}
	if !strings.Contains(err.Error(), "workflow registry is required") {
		t.Fatalf("expected workflow-registry-required error, got: %v", err)
	}
}
