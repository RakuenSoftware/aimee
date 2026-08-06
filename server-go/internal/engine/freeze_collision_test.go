package engine

import (
	"context"
	"fmt"
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

	gitRun(t, slicedir, "checkout", "-q", "-B", "aimee/wi/wi_child", base)
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

// TestRejectDivergentSiblingCreatesSkipsSiblingWithInvalidFreezeHeadType
// exercises the wrong-type branch of the durable-triple check: the sibling
// carries a freeze artifact of type frozen_diff plus a freeze-head artifact
// whose declared type is not commit, so the triple is incomplete. The sibling
// is therefore not frozen under the durable-artifact definition and the
// collision check must skip it (continue), not reject the current slice. This
// guards the inverse of the regression that previously over-rejected slices
// whose active sibling had only a partial freeze-artifact set.
func TestRejectDivergentSiblingCreatesSkipsSiblingWithInvalidFreezeHeadType(t *testing.T) {
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
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("sibling without the durable freeze triple must be skipped, got: %v", err)
	}
}

// TestRejectDivergentSiblingCreatesSkipsSiblingWithMissingFreezeArtifacts
// covers the missing-artifacts branch: all three freeze artifacts are removed
// from the store, so the durable triple is not present. The sibling is
// therefore not frozen under the durable-artifact definition and the collision
// check must skip it (continue), not reject the current slice. This guards
// against regressions to over-rejection: a sibling that simply has never been
// frozen is not a collision candidate.
func TestRejectDivergentSiblingCreatesSkipsSiblingWithMissingFreezeArtifacts(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	if err := store.SetWorktree(ctx, "wi_s0", ""); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.DeleteWorkItem("wi_s0"); err != nil {
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
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("sibling without the durable freeze triple must be skipped, got: %v", err)
	}
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

// TestRejectDivergentSiblingCreatesUsesDurableArtifactsWithoutWorkflowRegistry
// proves the registry-independent invariant: a NativeRunner constructed
// without a workflow registry must still surface a genuine
// freeze_create_create_collision from rejectDivergentSiblingCreates when the
// sibling's freeze artifacts (frozen_diff + freeze-head + freeze-base) prove
// the freeze is durable. This is the regression guard against re-introducing a
// sibling-stage walk that depends on r.workflows to identify frozen siblings.
func TestRejectDivergentSiblingCreatesUsesDurableArtifactsWithoutWorkflowRegistry(t *testing.T) {
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
	// Deliberately omit workflows: durable freeze artifacts alone must drive the
	// collision check, so the divergent sibling freeze is rejected.
	runner := &NativeRunner{db: store, artifacts: artifacts}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
}

// TestRejectDivergentSiblingCreatesRecognizesFrozenSiblingRoutedBackToImplement
// is the regression guard for the durable-artifact definition of "frozen": a
// sibling that legitimately produced and froze a competing diff but was then
// routed back to implement for revisions (current stage "implement") is still
// reported as frozen, so the second slice's collision check does not skip it.
// Previously siblingStageHasFrozenDiff inspected the sibling's current stage
// via the workflow graph and missed this case; the fix is to anchor the check
// to the freeze/freeze-head/freeze-base artifact triple directly.
func TestRejectDivergentSiblingCreatesRecognizesFrozenSiblingRoutedBackToImplement(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

	// Route the frozen sibling back to implement for revisions, simulating a
	// review/CI loop that returned it to implementation. The sibling's freeze
	// artifacts remain on disk but its current stage is no longer freeze.
	if err := store.Move(ctx, "wi_s0", "gate", "implement", "reroute", "", "", 0); err != nil {
		t.Fatal(err)
	}
	wiS0, err := store.WorkItem(ctx, "wi_s0")
	if err != nil {
		t.Fatal(err)
	}
	if wiS0.Stage != "implement" {
		t.Fatalf("setup precondition: expected wi_s0 stage=implement, got %q", wiS0.Stage)
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

// TestRejectDivergentSiblingCreatesAllowsIdenticalCreateFromDistinctSiblingWorktree
// exercises the case the false-positive rejection guarded against: the sibling
// was frozen on its OWN worktree (so the sibling's commits are reachable from
// the sibling's recorded Worktree, not from the current workdir). When the
// current slice creates the same file with the same blob, the resolver must
// resolve the sibling's commits against the sibling's worktree so the diff
// succeeds and the identical create is allowed.
func TestRejectDivergentSiblingCreatesAllowsIdenticalCreateFromDistinctSiblingWorktree(t *testing.T) {
	ctx, store, artifacts, registry, repo, slicedir, base, _ := setupFreezeCollisionHarness(t)

	// Put the sibling's frozen commits in a clone of the repo so the commits
	// are reachable from the sibling's recorded Worktree but the post-clone
	// sibling-only commits are not reachable from the current slicedir. This
	// is the production layout for two simultaneous sibling slices whose
	// durable Worktrees live behind separate git directory boundaries --
	// slicedir cannot see the sibling-only objects.
	root := t.TempDir()
	siblingDir := filepath.Join(root, "sibling")
	if err := os.MkdirAll(siblingDir, 0o700); err != nil {
		t.Fatal(err)
	}
	gitRun(t, siblingDir, "clone", repo, ".")
	gitRun(t, siblingDir, "checkout", "-q", "-B", "aimee/wi/wi_s0", base)
	if err := os.WriteFile(filepath.Join(siblingDir, "frozen.txt"), []byte("frozen sibling blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, siblingDir, "add", "frozen.txt")
	gitRun(t, siblingDir, "commit", "-m", "frozen create")
	siblingHead := strings.TrimSpace(gitRun(t, siblingDir, "rev-parse", "HEAD"))

	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "commit", []byte(siblingHead)); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-base", "commit", []byte(base)); err != nil {
		t.Fatal(err)
	}
	if err := store.SetWorktree(ctx, "wi_s0", siblingDir); err != nil {
		t.Fatal(err)
	}

	gitRun(t, slicedir, "checkout", "-q", "-B", "aimee/wi/wi_child", base)
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
		t.Fatalf("identical create on distinct sibling worktree should pass, got: %v", err)
	}
}

// TestRejectDivergentSiblingCreatesStillRejectsDivergenceFromDistinctSiblingWorktree
// guards the regression-sensitive behavior: when the sibling has its own
// worktree AND the current slice creates the same file with a different blob,
// the function must still raise freezeCreateCreateCollision. The
// sibling-worktree resolution path is only meant to make the comparison
// decidable, not to disable divergence rejection.
func TestRejectDivergentSiblingCreatesStillRejectsDivergenceFromDistinctSiblingWorktree(t *testing.T) {
	ctx, store, artifacts, registry, repo, slicedir, base, _ := setupFreezeCollisionHarness(t)

	// Sibling frozen in a clone of the repo so the post-clone sibling-only
	// commits are unreachable from the current slicedir.
	root := t.TempDir()
	siblingDir := filepath.Join(root, "sibling")
	if err := os.MkdirAll(siblingDir, 0o700); err != nil {
		t.Fatal(err)
	}
	gitRun(t, siblingDir, "clone", repo, ".")
	gitRun(t, siblingDir, "checkout", "-q", "-B", "aimee/wi/wi_s0", base)
	if err := os.WriteFile(filepath.Join(siblingDir, "frozen.txt"), []byte("frozen sibling blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, siblingDir, "add", "frozen.txt")
	gitRun(t, siblingDir, "commit", "-m", "frozen create")
	siblingHead := strings.TrimSpace(gitRun(t, siblingDir, "rev-parse", "HEAD"))

	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "commit", []byte(siblingHead)); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-base", "commit", []byte(base)); err != nil {
		t.Fatal(err)
	}
	if err := store.SetWorktree(ctx, "wi_s0", siblingDir); err != nil {
		t.Fatal(err)
	}

	gitRun(t, slicedir, "checkout", "-q", "-B", "aimee/wi/wi_child", base)
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

// TestRejectDivergentSiblingCreatesReportsLexicographicallySmallestCollision
// guards the determinism invariant the freeze-collision diagnostic relies on:
// when multiple current-slice creates collide with the sibling, the named
// colliding path must be stable across runs rather than depending on Go's
// non-deterministic map iteration order. The diagnostic now sorts the
// candidate creates before iterating so the smallest path is always reported
// first, matching the comparable-and-consistent ordering rule already used by
// freezeUncomparablePathIdentifier for fail-closed diagnostics. A regression
// to the previous "first map key" behavior would make this test flaky.
func TestRejectDivergentSiblingCreatesReportsLexicographicallySmallestCollision(t *testing.T) {
	ctx, store, artifacts, registry, _, slicedir, base, siblingHead := setupFreezeCollisionHarness(t)

	// The harness registered one frozen create (frozen.txt). Add two more
	// creates to the sibling's frozen commit so the sibling's frozen-diff
	// triple covers alpha.txt and zeta.txt in addition to frozen.txt. The
	// sibling's freeze is then THIS amended commit; update freeze-head to
	// match.
	if err := os.WriteFile(filepath.Join(slicedir, "alpha.txt"), []byte("sibling alpha\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(slicedir, "zeta.txt"), []byte("sibling zeta\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "alpha.txt", "zeta.txt")
	gitRun(t, slicedir, "commit", "--amend", "--no-edit")
	amendedSiblingHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "commit", []byte(amendedSiblingHead)); err != nil {
		t.Fatal(err)
	}
	mergedDiff := []byte("frozen.txt\n+frozen sibling blob\nalpha.txt\n+sibling alpha\nzeta.txt\n+sibling zeta\n")
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze", "frozen_diff", mergedDiff); err != nil {
		t.Fatal(err)
	}

	// Current slice overwrites all three with divergent content.
	if err := os.WriteFile(filepath.Join(slicedir, "alpha.txt"), []byte("current alpha\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(slicedir, "frozen.txt"), []byte("current frozen\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(slicedir, "zeta.txt"), []byte("current zeta\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "alpha.txt", "frozen.txt", "zeta.txt")
	gitRun(t, slicedir, "commit", "-m", "divergent creates")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
	// All three creates collide; the diagnostic must name "alpha.txt" because
	// it is the smallest colliding path. A non-deterministic implementation
	// would surface "frozen.txt" or "zeta.txt" on some runs and fail this
	// assertion.
	assertFreezeCreateCollision(t, err, "alpha.txt", item.ID, "wi_s0")
	if strings.Contains(err.Error(), "zeta.txt") {
		t.Fatalf("collision diagnostic must name the smallest colliding path, got: %v", err)
	}
	if !strings.Contains(err.Error(), "freeze_create_create_collision") {
		t.Fatalf("expected collision prefix, got: %v", err)
	}
	_ = siblingHead // referenced only to keep the harness return stable
}

// TestRejectDivergentSiblingCreatesReportsDeterministicCollisionAcrossRuns
// strengthens the determinism guarantee by repeating the multi-create
// collision many times with fresh harness state. With a non-deterministic
// implementation, Go's randomized map iteration would surface a different
// colliding path on different invocations; with sorted iteration the
// diagnostic is byte-identical across runs, satisfying the comparable-and-
// consistent ordering rule for fail-closed diagnostics.
func TestRejectDivergentSiblingCreatesReportsDeterministicCollisionAcrossRuns(t *testing.T) {
	const runs = 8
	var seen []string
	for i := 0; i < runs; i++ {
		i := i
		t.Run(fmt.Sprintf("run-%d", i), func(t *testing.T) {
			ctx, store, artifacts, registry, _, slicedir, base, _ := setupFreezeCollisionHarness(t)

			// Extend the sibling's frozen create set with two more files so
			// every current-slice create will collide with the sibling.
			if err := os.WriteFile(filepath.Join(slicedir, "m.txt"), []byte("sibling m\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(slicedir, "z.txt"), []byte("sibling z\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			gitRun(t, slicedir, "add", "m.txt", "z.txt")
			gitRun(t, slicedir, "commit", "--amend", "--no-edit")
			amendedSiblingHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))
			if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze-head", "commit", []byte(amendedSiblingHead)); err != nil {
				t.Fatal(err)
			}
			mergedDiff := []byte("frozen.txt\n+frozen sibling blob\nm.txt\n+sibling m\nz.txt\n+sibling z\n")
			if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze", "frozen_diff", mergedDiff); err != nil {
				t.Fatal(err)
			}

			// Overwrite the sibling's frozen creates with divergent content so
			// every current-slice create that the sibling ALSO created
			// (frozen.txt, m.txt, z.txt) collides. The smallest colliding path
			// ("frozen.txt") must be reported first, demonstrating the sort
			// guarantees determinism. "a.txt" is a fresh file the sibling
			// never created, so it is intentionally NOT a collision entry.
			for _, name := range []string{"frozen.txt", "m.txt", "z.txt"} {
				if err := os.WriteFile(filepath.Join(slicedir, name), []byte("conflicting "+name+"\n"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(filepath.Join(slicedir, "a.txt"), []byte("current a\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			gitRun(t, slicedir, "add", "frozen.txt", "m.txt", "a.txt", "z.txt")
			gitRun(t, slicedir, "commit", "-m", "divergent creates")
			currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

			item, err := store.WorkItem(ctx, "wi_s1")
			if err != nil {
				t.Fatal(err)
			}
			runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
			err = runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead)
			assertFreezeCreateCollision(t, err, "frozen.txt", item.ID, "wi_s0")
			seen = append(seen, err.Error())
		})
	}
	for i := 1; i < len(seen); i++ {
		if seen[i] != seen[0] {
			t.Fatalf("collision diagnostic non-deterministic across runs:\n  run 0: %s\n  run %d: %s", seen[0], i, seen[i])
		}
	}
}
