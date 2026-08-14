package engine

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type freezeHarness struct {
	engine    *Engine
	store     *db1.Store
	worktrees map[string]string
}

func newFreezeHarness(t *testing.T, baseFiles map[string]string) *freezeHarness {
	t.Helper()
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o700); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	for path, content := range baseFiles {
		full := filepath.Join(repo, path)
		if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	gitRun(t, repo, "add", "-A")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "branch", "aimee/feat/wi_parent", "trunk")

	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: freeze
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: pr
  - id: pr
    block: pr.open
    in: {src: freeze.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	parsed, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	ctx := t.Context()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_parent", Repo: repo,
		ProposalPath: "parent", WorkflowName: "slice", WorkflowVersion: parsed.Version,
		StartStage: "source"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	worktrees := make(map[string]string)
	for _, id := range []string{"wi_slice_a", "wi_slice_b"} {
		if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: id, Repo: repo,
			ProposalPath: id, WorkflowName: "slice", WorkflowVersion: parsed.Version,
			StartStage: "freeze", ParentID: "wi_parent"}); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(id, []byte(`{"packet_id":"`+id+`","dependencies":[]}`)); err != nil {
			t.Fatal(err)
		}
		if _, err := artifacts.PutNodeArtifact(id, "impl", "branch", []byte("head")); err != nil {
			t.Fatal(err)
		}
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		workdir, _, err := manager.Ensure(ctx, item, false)
		if err != nil {
			t.Fatal(err)
		}
		worktrees[id] = workdir
	}
	runner := &NativeRunner{db: store, worktrees: manager, artifacts: artifacts}
	engine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	return &freezeHarness{engine: engine, store: store, worktrees: worktrees}
}

func (h *freezeHarness) commit(t *testing.T, workItemID, path, content string) {
	t.Helper()
	workdir := h.worktrees[workItemID]
	full := filepath.Join(workdir, path)
	if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(full, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "-A")
	gitRun(t, workdir, "commit", "-m", "slice change")
}

func TestSiblingFreezeRejectsExactlyOneDivergentConcurrentCreate(t *testing.T) {
	h := newFreezeHarness(t, nil)
	const path = "docs/appliance-runbook.md"
	h.commit(t, "wi_slice_a", path, "slice a\n")
	h.commit(t, "wi_slice_b", path, "slice b\n")

	start := make(chan struct{})
	results := make(map[string]AdvanceResult)
	errorsByID := make(map[string]error)
	var mu sync.Mutex
	var wg sync.WaitGroup
	for _, id := range []string{"wi_slice_a", "wi_slice_b"} {
		wg.Add(1)
		go func(id string) {
			defer wg.Done()
			<-start
			result, err := h.engine.Advance(context.Background(), id)
			mu.Lock()
			results[id], errorsByID[id] = result, err
			mu.Unlock()
		}(id)
	}
	close(start)
	wg.Wait()

	winner, loser := "", ""
	for _, id := range []string{"wi_slice_a", "wi_slice_b"} {
		if errorsByID[id] != nil {
			t.Fatalf("advance %s: %v", id, errorsByID[id])
		}
		item, err := h.store.WorkItem(t.Context(), id)
		if err != nil {
			t.Fatal(err)
		}
		switch {
		case item.State == "active" && item.Stage == "pr":
			winner = id
		case item.State == "rejected" && item.Stage == "freeze" && results[id].Terminal:
			loser = id
		default:
			t.Fatalf("unexpected %s outcome: result=%+v item=%+v", id, results[id], item)
		}
		if item.PRRef != "" {
			t.Fatalf("%s reached PR creation: %q", id, item.PRRef)
		}
	}
	if winner == "" || loser == "" || winner == loser {
		t.Fatalf("winner=%q loser=%q results=%+v", winner, loser, results)
	}
	events, err := h.store.Events(t.Context(), loser, 0, 100)
	if err != nil {
		t.Fatal(err)
	}
	detail := events[len(events)-1].Detail
	for _, want := range []string{path, winner, loser} {
		if !strings.Contains(detail, want) {
			t.Fatalf("collision detail %q does not name %q", detail, want)
		}
	}
}

func TestSiblingFreezeAllowsIdenticalCreateContent(t *testing.T) {
	h := newFreezeHarness(t, nil)
	for _, id := range []string{"wi_slice_a", "wi_slice_b"} {
		h.commit(t, id, "docs/shared.md", "identical\n")
		result, err := h.engine.Advance(t.Context(), id)
		if err != nil || result.NextStage != "pr" {
			t.Fatalf("%s identical freeze = %+v, %v", id, result, err)
		}
	}
}

func TestSiblingFreezeAllowsExistingFileEdits(t *testing.T) {
	base := "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n"
	h := newFreezeHarness(t, map[string]string{"shared.txt": base})
	h.commit(t, "wi_slice_a", "shared.txt", strings.Replace(base, "one\n", "ONE\n", 1))
	h.commit(t, "wi_slice_b", "shared.txt", strings.Replace(base, "ten\n", "TEN\n", 1))
	for _, id := range []string{"wi_slice_a", "wi_slice_b"} {
		result, err := h.engine.Advance(t.Context(), id)
		if err != nil || result.NextStage != "pr" {
			t.Fatalf("%s existing-file freeze = %+v, %v", id, result, err)
		}
	}
}
