package engine

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type blockingRunner struct {
	started chan string
	release chan struct{}
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
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: implement\n")
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
