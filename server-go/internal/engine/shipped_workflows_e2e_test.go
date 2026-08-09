package engine

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// TestShippedWorkflowDefinitionsRunWithNativeEngine loads the exact workflow
// files shipped in config/workflows. The forge is a local bare Git repository
// and the agents are deterministic stubs, so this exercises every graph without
// opening external pull requests or invoking a model provider.
func TestShippedWorkflowDefinitionsRunWithNativeEngine(t *testing.T) {
	tests := []struct {
		name      string
		wantState string
		wantPause string
	}{
		{name: "managed-change", wantState: "accepted"},
		{name: "hotfix", wantState: "accepted"},
		{name: "build", wantState: "accepted"},
		{name: "build-triggered", wantState: "accepted"},
		{name: "manual-review", wantState: "active", wantPause: "human_gate"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			runExactShippedWorkflow(t, test.name, test.wantState, test.wantPause)
		})
	}
}

func TestShippedAutonomousWorkflowRetriesAreBounded(t *testing.T) {
	registry, err := wfe.NewRegistry(copyShippedWorkflowDefinitions(t))
	if err != nil {
		t.Fatal(err)
	}
	checks := []struct {
		workflow, node string
		max            int
	}{
		{workflow: "build", node: "plan_gate", max: 6},
		{workflow: "build", node: "slices", max: 3},
		{workflow: "build-triggered", node: "plan_gate", max: 6},
		{workflow: "build-triggered", node: "slices", max: 3},
		{workflow: "slice", node: "impl", max: 3},
	}
	for _, check := range checks {
		definition, err := registry.Pin(check.workflow)
		if err != nil {
			t.Fatal(err)
		}
		var found *wfe.Node
		for i := range definition.Nodes {
			if definition.Nodes[i].ID == check.node {
				found = &definition.Nodes[i]
				break
			}
		}
		if found == nil {
			t.Fatalf("workflow %s has no node %s", check.workflow, check.node)
		}
		if got := maxIterations(*found); got > check.max {
			t.Errorf("workflow %s node %s retries %d times, want at most %d", check.workflow, check.node, got, check.max)
		}
	}
}

func runExactShippedWorkflow(t *testing.T, workflowName, wantState, wantPause string) {
	t.Helper()
	root := t.TempDir()
	repo := newShippedWorkflowRepo(t, root)
	workflowDir := copyShippedWorkflowDefinitions(t)
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	definition, err := registry.Pin(workflowName)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	id := "wi_shipped_" + workflowName
	proposal := []byte("Implement and document the test feature.\n")
	if err := artifacts.PutProposal(id, proposal); err != nil {
		t.Fatal(err)
	}
	sourcePath := ""
	if workflowName == "build" || workflowName == "build-triggered" {
		sourcePath = "docs/proposals/pending/" + workflowName + ".md"
	}
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: id, Repo: repo, ProposalPath: "proposal:" + workflowName,
		WorkflowName: workflowName, WorkflowVersion: definition.Version,
		StartStage: definition.Start, SourcePath: sourcePath,
	}); err != nil {
		t.Fatal(err)
	}
	worktrees, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	runner, err := NewNativeRunner(store, worktrees, &e2eAgents{}, passVerifier{}, artifacts, registry, &e2eForge{})
	if err != nil {
		t.Fatal(err)
	}
	withPanel(runner, shippedRoundtableStore(t))
	engine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, engine, 4, nil)
	scheduler.perWorkflow = 4
	scheduler.pollEvery = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { scheduler.Run(ctx); close(done) }()
	defer func() { cancel(); <-done }()

	deadline := time.Now().Add(25 * time.Second)
	var item db1.WorkItem
	for time.Now().Before(deadline) {
		item, err = store.WorkItem(t.Context(), id)
		if err != nil {
			t.Fatal(err)
		}
		if item.State == wantState && item.PauseReason == wantPause &&
			(item.State != "active" || item.PauseReason != "") {
			break
		}
		if item.State != "active" && item.State != wantState {
			t.Fatalf("workflow ended unexpectedly: %+v", item)
		}
		if item.PauseReason != "" && item.PauseReason != "slices_running" && item.PauseReason != wantPause {
			events, _ := store.Events(t.Context(), id, 0, 1000)
			t.Fatalf("workflow parked unexpectedly: item=%+v events=%+v", item, events)
		}
		time.Sleep(10 * time.Millisecond)
	}
	item, err = store.WorkItem(t.Context(), id)
	if err != nil {
		t.Fatal(err)
	}
	if item.State != wantState || item.PauseReason != wantPause {
		events, _ := store.Events(t.Context(), id, 0, 1000)
		children, _ := store.Children(t.Context(), id)
		t.Fatalf("workflow timed out: item=%+v children=%+v events=%+v", item, children, events)
	}
	assertShippedStagesVisited(t, store, id, definition)
	if workflowName == "build" || workflowName == "build-triggered" {
		children, err := store.Children(t.Context(), id)
		if err != nil || len(children) != 1 {
			t.Fatalf("slice children=%+v err=%v", children, err)
		}
		if children[0].State != "accepted" || children[0].WorkflowName != "slice" {
			t.Fatalf("slice child did not complete: %+v", children[0])
		}
		slice, err := registry.Pin("slice")
		if err != nil {
			t.Fatal(err)
		}
		assertShippedStagesVisited(t, store, children[0].ID, slice)
		archived := filepath.Join(item.Worktree, "docs", "proposals", "done", workflowName+".md")
		if _, err := os.Stat(archived); err != nil {
			t.Fatalf("trigger proposal was not archived: %v", err)
		}
	}
}

func copyShippedWorkflowDefinitions(t *testing.T) string {
	t.Helper()
	workflowDir := filepath.Join(t.TempDir(), "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	_, thisFile, _, _ := runtime.Caller(0)
	shippedDir := filepath.Join(filepath.Dir(thisFile), "..", "..", "..", "config", "workflows")
	entries, err := os.ReadDir(shippedDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		base := strings.TrimSuffix(entry.Name(), ".yaml")
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".yaml" || strings.LastIndex(base, ".v") >= 0 {
			continue
		}
		content, readErr := os.ReadFile(filepath.Join(shippedDir, entry.Name()))
		if readErr != nil {
			t.Fatal(readErr)
		}
		if writeErr := os.WriteFile(filepath.Join(workflowDir, entry.Name()), content, 0o600); writeErr != nil {
			t.Fatal(writeErr)
		}
	}
	return workflowDir
}

func shippedRoundtableStore(t *testing.T) *roundtablecfg.Store {
	t.Helper()
	_, thisFile, _, _ := runtime.Caller(0)
	dir := filepath.Join(filepath.Dir(thisFile), "..", "..", "..", "config", "roundtables")
	store, err := roundtablecfg.NewStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	return store
}

func assertShippedStagesVisited(t *testing.T, store *db1.Store, id string, definition wfe.Definition) {
	t.Helper()
	events, err := store.Events(t.Context(), id, 0, 1000)
	if err != nil {
		t.Fatal(err)
	}
	visited := make(map[string]bool)
	for _, event := range events {
		visited[event.Stage] = true
	}
	for _, node := range definition.Nodes {
		if !visited[node.ID] {
			t.Errorf("workflow %s never visited stage %s; events=%+v", definition.Name, node.ID, events)
		}
	}
}

func newShippedWorkflowRepo(t *testing.T, root string) string {
	t.Helper()
	bare := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	run := func(dir string, args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example", "GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run(root, "init", "--bare", bare)
	run(root, "clone", bare, repo)
	run(repo, "checkout", "-b", "trunk")
	for _, name := range []string{"build", "build-triggered"} {
		path := filepath.Join(repo, "docs", "proposals", "pending", name+".md")
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(fmt.Sprintf("%s proposal\n", name)), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(repo, "README.md"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(repo, "add", ".")
	run(repo, "commit", "-m", "init")
	run(repo, "push", "-u", "origin", "trunk")
	run(repo, "remote", "set-head", "origin", "trunk")
	return repo
}
