package engine

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"

	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

// hungFreezeRunner blocks inside Run until release is closed. It pins a
// freeze runner in flight so we can observe the lock state from a parallel
// invocation.
type hungFreezeRunner struct {
	started chan struct{}
	release chan struct{}
}

func (r *hungFreezeRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	select {
	case r.started <- struct{}{}:
	case <-ctx.Done():
		return StepResult{}, ctx.Err()
	}
	select {
	case <-r.release:
		return StepResult{Status: StepAdvanced}, nil
	case <-ctx.Done():
		return StepResult{}, ctx.Err()
	}
}

// instantRunner returns immediately on each call and signals that it ran.
type instantRunner struct {
	started chan struct{}
}

func (r *instantRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	select {
	case r.started <- struct{}{}:
	default:
	}
	return StepResult{Status: StepAdvanced}, nil
}

// multiplexRunner dispatches Run to a per-block Runner so a single engine can
// hold a freeze runner that hangs while a non-freeze sibling completes freely.
type multiplexRunner struct {
	routes map[string]Runner
}

func (m *multiplexRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	r, ok := m.routes[req.Node.Block]
	if !ok {
		return StepResult{}, errors.New("multiplexRunner: no route for block " + req.Node.Block)
	}
	return r.Run(ctx, req)
}

// setupCompletionLockHarness builds a fresh engine, db, and artifact store
// with two workflows under the same parent: "alpha" starts at freeze, "beta"
// starts at work. The returned setup is suitable for exercises that need
// siblings targeting different (RootID, WorkflowName) pairs.
func setupCompletionLockHarness(t *testing.T) (workflowDir string, defA, defB *wfe.Definition, store *db1.Store, artifacts *wfe.ArtifactStore) {
	t.Helper()
	root := t.TempDir()
	workflowDir = filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	alphaDef := []byte("name: alpha\nstart: freeze\nnodes:\n  - id: freeze\n    block: freeze\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "alpha.yaml"), alphaDef, 0o600); err != nil {
		t.Fatal(err)
	}
	betaDef := []byte("name: beta\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "beta.yaml"), betaDef, 0o600); err != nil {
		t.Fatal(err)
	}
	var err error
	defA, err = wfe.ParseDefinition(alphaDef)
	if err != nil {
		t.Fatal(err)
	}
	defB, err = wfe.ParseDefinition(betaDef)
	if err != nil {
		t.Fatal(err)
	}
	store, err = db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err = wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	return workflowDir, defA, defB, store, artifacts
}

// TestFreezeLockKeyedOnWorkflowName verifies that a freeze on workflow "alpha"
// does not block a sibling on workflow "beta" under the same RootID, because
// the completion lock is now keyed on (RootID, WorkflowName).
func TestFreezeLockKeyedOnWorkflowName(t *testing.T) {
	workflowDir, defA, defB, store, artifacts := setupCompletionLockHarness(t)

	create := func(id, wfName, version, stage string) db1.CreateWorkItem {
		item := db1.CreateWorkItem{ID: id, Repo: "repo", ProposalPath: "p", WorkflowName: wfName, WorkflowVersion: version, StartStage: stage, ParentID: "wi_parent"}
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(id, []byte("p")); err != nil {
			t.Fatal(err)
		}
		return item
	}
	parent := db1.CreateWorkItem{ID: "wi_parent", Repo: "repo", ProposalPath: "p", WorkflowName: "alpha", WorkflowVersion: defA.Version, StartStage: "freeze"}
	if err := store.CreateWorkItem(t.Context(), parent); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(parent.ID, []byte("p")); err != nil {
		t.Fatal(err)
	}
	alphaItem := create("wi_parent.alpha", "alpha", defA.Version, "freeze")
	betaItem := create("wi_parent.beta", "beta", defB.Version, "work")

	freezeRunner := &hungFreezeRunner{started: make(chan struct{}, 1), release: make(chan struct{})}
	betaRunner := &instantRunner{started: make(chan struct{}, 1)}
	multi := &multiplexRunner{routes: map[string]Runner{
		"freeze":          freezeRunner,
		"author.proposal": betaRunner,
	}}

	eng, err := New(store, artifacts, workflowDir, multi)
	if err != nil {
		t.Fatal(err)
	}

	freezeErr := make(chan error, 1)
	go func() {
		_, err := eng.Advance(t.Context(), alphaItem.ID)
		freezeErr <- err
	}()

	select {
	case <-freezeRunner.started:
	case <-time.After(2 * time.Second):
		close(freezeRunner.release)
		t.Fatal("freeze runner never entered")
	}

	betaDone := make(chan error, 1)
	go func() {
		_, err := eng.Advance(t.Context(), betaItem.ID)
		betaDone <- err
	}()

	select {
	case <-betaRunner.started:
	case <-time.After(2 * time.Second):
		close(freezeRunner.release)
		t.Fatal("beta sibling waited on freeze lock — keys are not split by WorkflowName")
	}

	close(freezeRunner.release)
	if err := <-freezeErr; err != nil {
		t.Fatalf("freeze advance error: %v", err)
	}
	if err := <-betaDone; err != nil {
		t.Fatalf("beta advance error: %v", err)
	}
}

// TestFreezeLockSerializesSameWorkflow verifies that two freezes on the same
// (RootID, WorkflowName) still serialize. Only one freeze runner should be
// in-flight at a time.
func TestFreezeLockSerializesSameWorkflow(t *testing.T) {
	workflowDir, defA, _, store, artifacts := setupCompletionLockHarness(t)

	create := func(id, wfName, version, stage string) db1.CreateWorkItem {
		item := db1.CreateWorkItem{ID: id, Repo: "repo", ProposalPath: "p", WorkflowName: wfName, WorkflowVersion: version, StartStage: stage, ParentID: "wi_parent"}
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(id, []byte("p")); err != nil {
			t.Fatal(err)
		}
		return item
	}
	parent := db1.CreateWorkItem{ID: "wi_parent", Repo: "repo", ProposalPath: "p", WorkflowName: "alpha", WorkflowVersion: defA.Version, StartStage: "freeze"}
	if err := store.CreateWorkItem(t.Context(), parent); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(parent.ID, []byte("p")); err != nil {
		t.Fatal(err)
	}
	first := create("wi_parent.a", "alpha", defA.Version, "freeze")
	second := create("wi_parent.b", "alpha", defA.Version, "freeze")

	runner := &hungFreezeRunner{started: make(chan struct{}, 1), release: make(chan struct{})}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}

	firstErr := make(chan error, 1)
	go func() {
		_, err := eng.Advance(t.Context(), first.ID)
		firstErr <- err
	}()

	select {
	case <-runner.started:
	case <-time.After(2 * time.Second):
		close(runner.release)
		t.Fatal("first freeze runner never entered")
	}

	// Second caller should be blocked while the first holds the lock.
	secondCtx, secondCancel := context.WithTimeout(t.Context(), 200*time.Millisecond)
	defer secondCancel()
	_, secondErr := eng.Advance(secondCtx, second.ID)
	if secondErr == nil {
		close(runner.release)
		t.Fatal("second freeze runner entered while first held the lock — same-workflow serialization is broken")
	}
	if !errors.Is(secondErr, context.DeadlineExceeded) &&
		!strings.Contains(secondErr.Error(), "context deadline exceeded") {
		close(runner.release)
		t.Fatalf("second freeze error = %v, want context.DeadlineExceeded", secondErr)
	}

	close(runner.release)
	if err := <-firstErr; err != nil {
		t.Fatalf("first freeze advance error: %v", err)
	}
}

// TestFreezeLockWatchdogReleasesOnContextCancel verifies that when a freeze
// runner never releases, a concurrent freeze invocation with a bounded
// context surfaces the contention via the existing error path instead of
// blocking forever, and that the lock is released so subsequent callers
// can proceed once the stuck freeze is cleared.
func TestFreezeLockWatchdogReleasesOnContextCancel(t *testing.T) {
	workflowDir, defA, _, store, artifacts := setupCompletionLockHarness(t)

	create := func(id, wfName, version, stage string) db1.CreateWorkItem {
		item := db1.CreateWorkItem{ID: id, Repo: "repo", ProposalPath: "p", WorkflowName: wfName, WorkflowVersion: version, StartStage: stage, ParentID: "wi_parent"}
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(id, []byte("p")); err != nil {
			t.Fatal(err)
		}
		return item
	}
	parent := db1.CreateWorkItem{ID: "wi_parent", Repo: "repo", ProposalPath: "p", WorkflowName: "alpha", WorkflowVersion: defA.Version, StartStage: "freeze"}
	if err := store.CreateWorkItem(t.Context(), parent); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(parent.ID, []byte("p")); err != nil {
		t.Fatal(err)
	}
	first := create("wi_parent.a", "alpha", defA.Version, "freeze")
	third := create("wi_parent.c", "alpha", defA.Version, "freeze")

	runner := &hungFreezeRunner{started: make(chan struct{}, 1), release: make(chan struct{})}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}

	firstCtx, firstCancel := context.WithCancel(t.Context())
	firstErr := make(chan error, 1)
	go func() {
		_, err := eng.Advance(firstCtx, first.ID)
		firstErr <- err
	}()

	select {
	case <-runner.started:
	case <-time.After(2 * time.Second):
		firstCancel()
		close(runner.release)
		t.Fatal("freeze runner never entered")
	}

	// Second caller has a short, bounded context. The watchdog must give up
	// rather than block forever while the first freeze holds the lock.
	second := create("wi_parent.b", "alpha", defA.Version, "freeze")
	secondCtx, secondCancel := context.WithTimeout(t.Context(), 200*time.Millisecond)
	defer secondCancel()
	secondResult, secondErr := eng.Advance(secondCtx, second.ID)
	if secondErr == nil {
		close(runner.release)
		t.Fatalf("second freeze should have failed, got result=%+v", secondResult)
	}
	if !errors.Is(secondErr, context.DeadlineExceeded) &&
		!strings.Contains(secondErr.Error(), "context deadline exceeded") {
		close(runner.release)
		t.Fatalf("second freeze error = %v, want context.DeadlineExceeded", secondErr)
	}
	// The runner should not have been invoked for the second caller.
	select {
	case <-runner.started:
		close(runner.release)
		t.Fatal("second freeze runner started despite the watchdog rejecting acquisition")
	default:
	}

	// Cleanup: release the first freeze and confirm it returns.
	close(runner.release)
	firstCancel()
	if err := <-firstErr; err != nil && !errors.Is(err, context.Canceled) {
		t.Fatalf("first freeze unexpected error: %v", err)
	}

	// Now the watchdog must have released the lock; a fresh third caller
	// under the same (RootID, WorkflowName) should be able to acquire it.
	thirdResult, thirdErr := eng.Advance(t.Context(), third.ID)
	if thirdErr != nil {
		t.Fatalf("third freeze after watchdog release failed: %v (result=%+v)", thirdErr, thirdResult)
	}
}
