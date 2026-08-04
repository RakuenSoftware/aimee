package engine

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type crashingRunner struct{}

func (crashingRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{}, errors.New("worker process connection disappeared")
}

type missingGitIdentityRunner struct{}

func (missingGitIdentityRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{}, fmt.Errorf("commit changes: %w", ErrGitIdentityMissing)
}

func TestRunnerCrashCannotAbandonOrCrashControlPlane(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: build\nstart: plan\nnodes:\n  - id: plan\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), definition, 0o600); err != nil {
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
	if err := artifacts.PutProposal("wi_crash", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: "wi_crash", Repo: "repo", ProposalPath: "proposal", WorkflowName: "build",
		WorkflowVersion: def.Version, StartStage: "plan", Mode: "autonomous",
	}); err != nil {
		t.Fatal(err)
	}
	engine, err := New(store, artifacts, workflowDir, crashingRunner{})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := engine.Advance(t.Context(), "wi_crash"); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(t.Context(), "wi_crash")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.PauseReason != "runner_unavailable" {
		t.Fatalf("runner crash changed item to %+v", item)
	}
	if resumed, err := store.ResumeTransient(t.Context(), "runner_unavailable", 0); err != nil || resumed != 1 {
		t.Fatalf("resume transient: count=%d err=%v", resumed, err)
	}
	item, err = store.WorkItem(t.Context(), "wi_crash")
	if err != nil || item.State != "active" || item.PauseReason != "" {
		t.Fatalf("recoverable item after resume: item=%+v err=%v", item, err)
	}
}

func TestMissingGitIdentityParksWithoutTransientRetry(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: build\nstart: impl\nnodes:\n  - id: impl\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), definition, 0o600); err != nil {
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
	if err := artifacts.PutProposal("wi_identity", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: "wi_identity", Repo: "repo", ProposalPath: "proposal", WorkflowName: "build",
		WorkflowVersion: def.Version, StartStage: "impl", Mode: "autonomous",
	}); err != nil {
		t.Fatal(err)
	}
	engine, err := New(store, artifacts, workflowDir, missingGitIdentityRunner{})
	if err != nil {
		t.Fatal(err)
	}
	result, err := engine.Advance(t.Context(), "wi_identity")
	if err != nil {
		t.Fatal(err)
	}
	if !result.Parked || result.PauseReason != "git_identity_missing" {
		t.Fatalf("result = %+v, want persistent git_identity_missing park", result)
	}
	item, err := store.WorkItem(t.Context(), "wi_identity")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.PauseReason != "git_identity_missing" {
		t.Fatalf("item = %+v, want active/git_identity_missing", item)
	}
	if resumed, err := store.ResumeTransient(t.Context(), "runner_unavailable", 0); err != nil || resumed != 0 {
		t.Fatalf("runner retry resumed persistent identity park: count=%d err=%v", resumed, err)
	}
}
