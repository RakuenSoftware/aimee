package engine

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type blockingRunner struct {
	started chan string
	release chan struct{}
}

type transientRoundtableRunner struct {
	mu       sync.Mutex
	reasons  map[string]string
	versions map[string]string
}

func (r *transientRoundtableRunner) Run(_ context.Context, request StepRequest) (StepResult, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if first, ok := r.versions[request.WorkItem.ID]; !ok {
		r.versions[request.WorkItem.ID] = request.WorkItem.UpdatedAt
		return StepResult{Status: StepPending, PauseReason: r.reasons[request.WorkItem.ID], Detail: "temporary roundtable phase failure"}, nil
	} else if first == request.WorkItem.UpdatedAt {
		return StepResult{}, errors.New("scheduler retry reused the parked execution version")
	}
	return StepResult{Status: StepAdvanced}, nil
}

func (r *blockingRunner) Run(ctx context.Context, request StepRequest) (StepResult, error) {
	select {
	case r.started <- request.WorkItem.ID:
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

func TestSchedulerFillsFreedSlotImmediately(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"wi_one", "wi_two", "wi_three"} {
		if err := artifacts.PutProposal(id, []byte(id)); err != nil {
			t.Fatal(err)
		}
		if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
			ID: id, Repo: "repo", ProposalPath: id, WorkflowName: "one",
			WorkflowVersion: def.Version, StartStage: "work", Mode: "autonomous",
		}); err != nil {
			t.Fatal(err)
		}
	}
	runner := &blockingRunner{started: make(chan string, 3), release: make(chan struct{}, 3)}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, workflowEngine, 2, nil)
	scheduler.pollEvery = time.Hour // the third item must start via slot-release notify.
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go scheduler.Run(ctx)

	waitStarted(t, runner.started)
	waitStarted(t, runner.started)
	select {
	case third := <-runner.started:
		t.Fatalf("concurrency cap exceeded; third started early: %s", third)
	case <-time.After(50 * time.Millisecond):
	}
	runner.release <- struct{}{}
	waitStarted(t, runner.started)
	runner.release <- struct{}{}
	runner.release <- struct{}{}
}

func waitStarted(t *testing.T, started <-chan string) string {
	t.Helper()
	select {
	case id := <-started:
		return id
	case <-time.After(2 * time.Second):
		t.Fatal("workflow did not start")
		return ""
	}
}

func TestSchedulerRecoversRoundtablePhasePausesWithNewExecutionVersion(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	reasons := map[string]string{
		"wi_discussion": "roundtable_discussion",
		"wi_chairman":   "roundtable_chairman",
	}
	for id := range reasons {
		if err := artifacts.PutProposal(id, []byte("proposal")); err != nil {
			t.Fatal(err)
		}
		if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
			ID: id, Repo: "repo", ProposalPath: id, WorkflowName: "one",
			WorkflowVersion: def.Version, StartStage: "work", Mode: "autonomous",
		}); err != nil {
			t.Fatal(err)
		}
	}
	runner := &transientRoundtableRunner{reasons: reasons, versions: make(map[string]string)}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, workflowEngine, 2, nil)
	scheduler.pollEvery = 10 * time.Millisecond
	scheduler.transientPauses = []transientPause{
		{reason: "roundtable_discussion", backoff: time.Second},
		{reason: "roundtable_chairman", backoff: time.Second},
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go scheduler.Run(ctx)

	deadline := time.Now().Add(4 * time.Second)
	for time.Now().Before(deadline) {
		accepted := 0
		for id := range reasons {
			item, err := store.WorkItem(t.Context(), id)
			if err != nil {
				t.Fatal(err)
			}
			if item.State == "accepted" {
				accepted++
			}
		}
		if accepted == len(reasons) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	for id, reason := range reasons {
		item, _ := store.WorkItem(t.Context(), id)
		t.Logf("%s (%s): state=%s pause=%s updated=%s", id, reason, item.State, item.PauseReason, item.UpdatedAt)
	}
	t.Fatal("roundtable phase pauses did not recover through the scheduler")
}

func TestSchedulerCancelCannotAdvancePausedWorkflow(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	report, err := registry.Save("one", []byte("name: one\nnodes:\n  - id: work\n    block: author.proposal\n"), "")
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, _ := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	_ = artifacts.PutProposal("wi_cancel", []byte("proposal"))
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{ID: "wi_cancel", Repo: "repo", ProposalPath: "proposal", WorkflowName: "one", WorkflowVersion: report.Version, StartStage: "work"}); err != nil {
		t.Fatal(err)
	}
	runner := &blockingRunner{started: make(chan string, 1), release: make(chan struct{})}
	eng, _ := New(store, artifacts, workflowDir, runner)
	scheduler := NewScheduler(store, eng, 1, nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go scheduler.Run(ctx)
	waitStarted(t, runner.started)
	if err := store.Pause(t.Context(), "wi_cancel"); err != nil {
		t.Fatal(err)
	}
	scheduler.Cancel("wi_cancel")
	time.Sleep(30 * time.Millisecond)
	item, err := store.WorkItem(t.Context(), "wi_cancel")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.PauseReason != "manual" {
		t.Fatalf("item=%+v", item)
	}
}

func TestSchedulerReconciliationCancelsRunningOrphan(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_terminal_root", Repo: "repo", ProposalPath: "root", WorkflowName: "build", StartStage: "slices"},
		{ID: "wi_running_orphan", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_terminal_root"},
	} {
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
	}
	// Reproduce the old single-row terminal transition while the child already
	// has a scheduler execution context.
	if err := store.Finish(t.Context(), "wi_terminal_root", "slices", "stopped", "operator_stop", "", 0); err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, nil, 1, nil)
	cancelled := false
	scheduler.cancels["wi_running_orphan"] = func() { cancelled = true }
	scheduler.fill(t.Context())
	if !cancelled {
		t.Fatal("reconciled running descendant was not locally cancelled")
	}
	child, err := store.WorkItem(t.Context(), "wi_running_orphan")
	if err != nil || child.State != "stopped" {
		t.Fatalf("child=%+v err=%v", child, err)
	}
}

func TestSchedulerReconciliationStopsAnActuallyRunningOrphan(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	report, err := registry.Save("one", []byte("name: one\nnodes:\n  - id: work\n    block: author.proposal\n"), "")
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_live_root", Repo: "repo", ProposalPath: "live-root", WorkflowName: "one", WorkflowVersion: report.Version, StartStage: "work"},
		{ID: "wi_live_orphan", Repo: "repo", ProposalPath: "live-child", WorkflowName: "one", WorkflowVersion: report.Version, StartStage: "work", ParentID: "wi_live_root"},
	} {
		if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
			t.Fatal(err)
		}
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
	}
	runner := &blockingRunner{started: make(chan string, 2), release: make(chan struct{})}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, eng, 2, nil)
	scheduler.pollEvery = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go scheduler.Run(ctx)
	if started := waitStarted(t, runner.started); started != "wi_live_orphan" {
		t.Fatalf("running item=%s want orphan descendant", started)
	}
	// Reproduce a terminal root written by the old single-item stop while its
	// descendant is genuinely blocked inside a scheduler-owned execution.
	if err := store.Finish(t.Context(), "wi_live_root", "work", "stopped", "operator_stop", "", 0); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		item, err := store.WorkItem(t.Context(), "wi_live_orphan")
		if err != nil {
			t.Fatal(err)
		}
		scheduler.mu.Lock()
		_, running := scheduler.running["wi_live_orphan"]
		scheduler.mu.Unlock()
		if item.State == "stopped" && !running {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("running orphan was not reconciled and cancelled")
}
