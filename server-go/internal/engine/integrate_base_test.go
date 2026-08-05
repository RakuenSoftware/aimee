package engine

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

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

func TestNativeRunnerIntegrateFeatureBaseUsesResourcePlaneIdentity(t *testing.T) {
	t.Setenv("AIMEE_GIT_AUTHOR_NAME", "")
	t.Setenv("AIMEE_GIT_AUTHOR_EMAIL", "")
	repo, slicedir := setupSliceRepo(t)
	if err := os.WriteFile(filepath.Join(slicedir, "slice.txt"), []byte("slice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "slice.txt")
	gitRun(t, slicedir, "commit", "-m", "slice change")
	advanceFeature(t, repo, "sibling.txt", "sibling\n")

	runner := &NativeRunner{forge: fixedIdentityForge{}}
	park, err := runner.integrateFeatureBase(t.Context(), slicedir, "wi_parent")
	if err != nil || park != "" {
		t.Fatalf("integrateFeatureBase: park=%q err=%v", park, err)
	}
	author := gitRun(t, slicedir, "show", "-s", "--format=%an <%ae>")
	if strings.TrimSpace(author) != "Vault Operator <vault@example.test>" {
		t.Fatalf("merge author = %q", author)
	}
}

// A slice freeze must review only that slice's delta over the latest feature
// tip. Forge merges advance origin/aimee/feat/<parent>, not the stale local ref;
// using the local ref makes later slice artifacts include every landed sibling
// and causes strict scope review to reject unrelated prerequisite work as drift.
func TestFreezeUsesMergedRemoteFeatureTip(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	gitRun(t, root, "init", "--bare", "-b", "trunk", origin)
	gitRun(t, root, "clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "push", "-u", "origin", "trunk")

	feature := "aimee/feat/wi_parent"
	gitRun(t, repo, "branch", feature)
	gitRun(t, repo, "push", "origin", feature)

	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	child, err := store.WorkItem(ctx, "wi_child")
	if err != nil {
		t.Fatal(err)
	}
	workdir, _, err := manager.Ensure(ctx, child, false)
	if err != nil {
		t.Fatal(err)
	}

	// Simulate a sibling PR merging after this child worktree was created.
	landed := filepath.Join(root, "landed")
	gitRun(t, root, "clone", "-b", feature, origin, landed)
	if err := os.WriteFile(filepath.Join(landed, "sibling.txt"), []byte("landed\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, landed, "add", "sibling.txt")
	gitRun(t, landed, "commit", "-m", "sibling")
	gitRun(t, landed, "push", "origin", feature)
	if park, err := integrateFeatureBase(ctx, workdir, "wi_parent"); err != nil || park != "" {
		t.Fatalf("integrateFeatureBase: park=%q err=%v", park, err)
	}

	if err := os.WriteFile(filepath.Join(workdir, "child.txt"), []byte("child\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "child.txt")
	gitRun(t, workdir, "commit", "-m", "child")

	runner := &NativeRunner{db: store, worktrees: manager}
	result, err := runner.freeze(ctx, StepRequest{WorkItem: child})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StepAdvanced {
		t.Fatalf("freeze status=%q detail=%q", result.Status, result.Detail)
	}
	if strings.Contains(result.Artifact, "sibling.txt") {
		t.Fatalf("freeze leaked previously merged sibling work:\n%s", result.Artifact)
	}
	if !strings.Contains(result.Artifact, "child.txt") {
		t.Fatalf("freeze omitted this slice's change:\n%s", result.Artifact)
	}
}

func TestFreezeRejectsDivergentSiblingCreateCreateCollision(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	gitRun(t, root, "init", "--bare", "-b", "trunk", origin)
	gitRun(t, root, "clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "push", "-u", "origin", "trunk")

	feature := "aimee/feat/wi_parent"
	gitRun(t, repo, "branch", feature)
	gitRun(t, repo, "push", "origin", feature)

	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: slice\nstart: freeze\nnodes:\n  - id: branch\n    block: branch.open\n  - id: freeze\n    block: freeze\n    in: {branch: branch.out}\n    next: review\n  - id: review\n    block: review\n    in: {src: freeze.out}\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}

	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	ctx := t.Context()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_s0", Repo: repo, ProposalPath: "s0", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
		{ID: "wi_s1", Repo: repo, ProposalPath: "s1", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, worktrees: manager, artifacts: artifacts, workflows: registry}

	first, err := store.WorkItem(ctx, "wi_s0")
	if err != nil {
		t.Fatal(err)
	}
	firstDir, _, err := manager.Ensure(ctx, first, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(firstDir, "collision.txt"), []byte("first\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, firstDir, "add", "collision.txt")
	gitRun(t, firstDir, "commit", "-m", "first")
	firstResult, err := runner.freeze(ctx, StepRequest{WorkItem: first, Node: wfe.Node{ID: "freeze"}})
	if err != nil {
		t.Fatal(err)
	}
	if firstResult.Status != StepAdvanced {
		t.Fatalf("first freeze status=%q detail=%q", firstResult.Status, firstResult.Detail)
	}
	second, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	secondDir, _, err := manager.Ensure(ctx, second, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(secondDir, "collision.txt"), []byte("second\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, secondDir, "add", "collision.txt")
	gitRun(t, secondDir, "commit", "-m", "second")
	secondBase, err := freezeBase(ctx, second, secondDir)
	if err != nil {
		t.Fatal(err)
	}
	secondBase = strings.TrimSpace(gitRun(t, secondDir, "rev-parse", secondBase))
	secondHead := strings.TrimSpace(gitRun(t, secondDir, "rev-parse", "HEAD"))
	// The first runner has written its companion artifacts but has not completed
	// the durable Move yet. Treating this crash window as frozen would reject both
	// slices after a restart; the row still at freeze is authoritative.
	if err := runner.rejectDivergentSiblingCreates(ctx, second, secondDir, secondBase, secondHead); err != nil {
		t.Fatalf("orphan freeze marker rejected sibling: %v", err)
	}
	if err := store.Move(ctx, "wi_s0", "freeze", "review", "advance", "", firstResult.ContentHash, 0); err != nil {
		t.Fatal(err)
	}
	secondResult, err := runner.freeze(ctx, StepRequest{WorkItem: second, Node: wfe.Node{ID: "freeze"}})
	if err != nil {
		t.Fatal(err)
	}
	if secondResult.Status != StepFailed {
		t.Fatalf("second freeze status=%q detail=%q", secondResult.Status, secondResult.Detail)
	}
	for _, want := range []string{freezeCreateCreateCollision, "collision.txt", "wi_s1", "wi_s0"} {
		if !strings.Contains(secondResult.Detail, want) {
			t.Fatalf("collision detail %q missing %q", secondResult.Detail, want)
		}
	}
	storedSecond, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	if storedSecond.Stage != "freeze" || storedSecond.State != "active" || storedSecond.ContentHash != "" {
		t.Fatalf("second slice mutated before engine transition: stage=%q state=%q hash=%q", storedSecond.Stage, storedSecond.State, storedSecond.ContentHash)
	}
	if _, err := artifacts.NodeArtifact("wi_s1", "freeze"); err == nil {
		t.Fatal("rejected freeze published an artifact")
	}
	events, err := store.Events(ctx, "wi_s1", 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	for _, event := range events {
		if event.Kind == "loop" || event.Kind == "pause" {
			t.Fatalf("rejected freeze queued retry/merge work: %+v", event)
		}
	}
}

// Replays the appliance-runbook packet failure mode: two slices from the same
// packet set create the same new runbook path differently. The scheduler must
// stop the losing packet at freeze and never queue PR or merge work for it.
func TestApplianceRunbookReplayRejectsDivergentCreateBeforeMergeQueue(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	gitRun(t, root, "init", "--bare", "-b", "trunk", origin)
	gitRun(t, root, "clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "push", "-u", "origin", "trunk")
	gitRun(t, repo, "branch", "aimee/feat/wi_parent")
	gitRun(t, repo, "push", "origin", "aimee/feat/wi_parent")

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
    next: review
  - id: review
    block: gate.human
    in: {src: freeze.out}
    on_pass: done
    on_fail: impl
  - id: done
    block: gate.deliver
    in: {verdict: review.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	def, err := registry.Pin("slice")
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	ctx := t.Context()
	items := []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "parent", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_runbook_a", Repo: repo, ProposalPath: "a", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze", ParentID: "wi_parent"},
		{ID: "wi_runbook_b", Repo: repo, ProposalPath: "b", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze", ParentID: "wi_parent"},
	}
	for _, item := range items {
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		proposal := []byte(`{"packet_id":"` + item.ID + `","dependencies":[]}`)
		if item.ParentID == "" {
			proposal = []byte("write the appliance recovery runbook")
		}
		if err := artifacts.PutProposal(item.ID, proposal); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	for i, id := range []string{"wi_runbook_a", "wi_runbook_b"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		dir, branch, err := manager.Ensure(ctx, item, false)
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(dir, "docs", "runbooks", "appliance-state-recovery.md")
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte{byte('A' + i), '\n'}, 0o600); err != nil {
			t.Fatal(err)
		}
		gitRun(t, dir, "add", "docs/runbooks/appliance-state-recovery.md")
		gitRun(t, dir, "commit", "-m", "runbook slice")
		if _, err := artifacts.PutNodeArtifact(id, "impl", "branch", []byte(branch)); err != nil {
			t.Fatal(err)
		}
	}
	runner := &NativeRunner{db: store, worktrees: manager, artifacts: artifacts, workflows: registry}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}

	// Include the parent plus both slices so the collision is exercised concurrently.
	scheduler := NewScheduler(store, eng, 3, nil)
	scheduler.SetPerWorkflowSource(func() int { return 2 })
	scheduler.fill(ctx)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		scheduler.mu.Lock()
		running := len(scheduler.running)
		scheduler.mu.Unlock()
		if running == 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	scheduler.mu.Lock()
	stillRunning := len(scheduler.running)
	scheduler.mu.Unlock()
	if stillRunning != 0 {
		t.Fatalf("scheduler still has %d running work items", stillRunning)
	}
	advanced, rejected := "", ""
	for _, id := range []string{"wi_runbook_a", "wi_runbook_b"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		switch {
		case item.Stage == "review" && item.State == "active" && item.PauseReason == "human_gate":
			advanced = id
		case item.Stage == "freeze" && item.State == "rejected":
			rejected = id
		default:
			t.Fatalf("unexpected scheduler outcome for %s: %+v", id, item)
		}
	}
	if advanced == "" || rejected == "" || advanced == rejected {
		t.Fatalf("want one advanced and one rejected, advanced=%q rejected=%q", advanced, rejected)
	}
	loser, err := store.WorkItem(ctx, rejected)
	if err != nil {
		t.Fatal(err)
	}
	if loser.Stage != "freeze" || loser.State != "rejected" || loser.PRRef != "" {
		t.Fatalf("loser crossed the freeze boundary: %+v", loser)
	}
	if _, err := artifacts.NodeArtifact(rejected, "pr"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("rejected slice opened a PR/CONFLICTING PR: %v", err)
	}
	if _, err := artifacts.NodeArtifact(rejected, "freeze"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("rejected slice published freeze artifact: %v", err)
	}
	events, err := store.Events(ctx, rejected, 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) == 0 {
		t.Fatal("rejected slice has no terminal audit event")
	}
	for _, event := range events {
		if event.Kind == "loop" || event.Kind == "pause" {
			t.Fatalf("rejected freeze queued retry/merge work: %+v", event)
		}
	}
	detail := events[len(events)-1].Detail
	for _, want := range []string{freezeCreateCreateCollision, "docs/runbooks/appliance-state-recovery.md", advanced, rejected} {
		if !strings.Contains(detail, want) {
			t.Fatalf("terminal detail %q missing %q", detail, want)
		}
	}
}

type blockingFreezeRunner struct {
	*NativeRunner
	mu           sync.Mutex
	calls        int
	firstID      string
	boundaryErr  string
	firstEntered chan struct{}
	releaseFirst chan struct{}
}

func (r *blockingFreezeRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	if req.Node.Block != "freeze" {
		return r.NativeRunner.Run(ctx, req)
	}
	r.mu.Lock()
	r.calls++
	call := r.calls
	if call == 1 {
		r.firstID = req.WorkItem.ID
		close(r.firstEntered)
	}
	firstID := r.firstID
	r.mu.Unlock()
	if call == 1 {
		select {
		case <-r.releaseFirst:
		case <-ctx.Done():
			return StepResult{}, ctx.Err()
		}
	}
	if call == 2 {
		item, err := r.db.WorkItem(ctx, firstID)
		if err != nil || item.Stage != "review" {
			r.mu.Lock()
			r.boundaryErr = "second freeze entered before the first durable Move"
			r.mu.Unlock()
		}
		if _, err := r.artifacts.NodeArtifact(firstID, "freeze"); err != nil {
			r.mu.Lock()
			r.boundaryErr = "second freeze entered before the first artifact publication"
			r.mu.Unlock()
		}
	}
	return r.NativeRunner.Run(ctx, req)
}

func (r *blockingFreezeRunner) callCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.calls
}

func (r *blockingFreezeRunner) boundaryFailure() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.boundaryErr
}

func TestSynchronizedSimultaneousFreezeRejectsExactlyOneDivergentCreate(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	gitRun(t, root, "init", "--bare", "-b", "trunk", origin)
	gitRun(t, root, "clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "push", "-u", "origin", "trunk")
	gitRun(t, repo, "branch", "aimee/feat/wi_parent")
	gitRun(t, repo, "push", "origin", "aimee/feat/wi_parent")

	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: freeze
nodes:
  - id: impl
    block: branch.open
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: review
  - id: review
    block: review
    in: {src: freeze.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	def, err := registry.Pin("slice")
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	ctx := t.Context()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "parent", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_parent.s0", Repo: repo, ProposalPath: "a", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze", ParentID: "wi_parent"},
		{ID: "wi_parent.s1", Repo: repo, ProposalPath: "b", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(item.ID, []byte(`{"packet_id":"`+item.ID+`","dependencies":[]}`)); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	for i, id := range []string{"wi_parent.s0", "wi_parent.s1"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		dir, branch, err := manager.Ensure(ctx, item, false)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "collision.txt"), []byte{byte('A' + i), '\n'}, 0o600); err != nil {
			t.Fatal(err)
		}
		gitRun(t, dir, "add", "collision.txt")
		gitRun(t, dir, "commit", "-m", "colliding create")
		if _, err := artifacts.PutNodeArtifact(id, "impl", "branch", []byte(branch)); err != nil {
			t.Fatal(err)
		}
	}
	native := &NativeRunner{db: store, worktrees: manager, artifacts: artifacts, workflows: registry}
	runner := &blockingFreezeRunner{NativeRunner: native, firstEntered: make(chan struct{}), releaseFirst: make(chan struct{})}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}

	type advanceOutcome struct {
		id     string
		result AdvanceResult
		err    error
	}
	outcomes := make(chan advanceOutcome, 2)
	start := make(chan struct{})
	var ready sync.WaitGroup
	ready.Add(2)
	for _, id := range []string{"wi_parent.s0", "wi_parent.s1"} {
		go func(id string) {
			ready.Done()
			<-start
			result, err := eng.Advance(ctx, id)
			outcomes <- advanceOutcome{id: id, result: result, err: err}
		}(id)
	}
	ready.Wait()
	close(start)
	select {
	case <-runner.firstEntered:
	case <-time.After(5 * time.Second):
		t.Fatal("first freeze never entered the runner")
	}
	// The second Advance has started, but the shared root completion lock must
	// keep it outside the runner until the first freeze has published its
	// artifacts and committed its lifecycle Move.
	time.Sleep(100 * time.Millisecond)
	if calls := runner.callCount(); calls != 1 {
		t.Fatalf("root completion lock admitted %d freezes before the first completed", calls)
	}
	close(runner.releaseFirst)

	advanced, rejected := "", ""
	for i := 0; i < 2; i++ {
		select {
		case outcome := <-outcomes:
			if outcome.err != nil {
				t.Fatalf("advance %s: %v", outcome.id, outcome.err)
			}
			item, err := store.WorkItem(ctx, outcome.id)
			if err != nil {
				t.Fatal(err)
			}
			switch {
			case outcome.result.Ran && outcome.result.NextStage == "review" && item.Stage == "review" && item.State == "active":
				advanced = outcome.id
			case outcome.result.Ran && outcome.result.Terminal && outcome.result.State == "rejected" && item.Stage == "freeze" && item.State == "rejected":
				rejected = outcome.id
			default:
				events, _ := store.Events(ctx, outcome.id, 0, 50)
				t.Fatalf("unexpected synchronized outcome for %s: result=%+v item=%+v events=%+v", outcome.id, outcome.result, item, events)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for synchronized freezes")
		}
	}
	if advanced == "" || rejected == "" || advanced == rejected {
		t.Fatalf("want exactly one advanced and one rejected, advanced=%q rejected=%q", advanced, rejected)
	}
	if failure := runner.boundaryFailure(); failure != "" {
		t.Fatal(failure)
	}
	events, err := store.Events(ctx, rejected, 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) == 0 {
		t.Fatal("rejected slice has no terminal audit event")
	}
	for _, event := range events {
		if event.Kind == "loop" || event.Kind == "pause" {
			t.Fatalf("rejected synchronized freeze queued retry/merge work: %+v", event)
		}
	}
	detail := events[len(events)-1].Detail
	for _, want := range []string{freezeCreateCreateCollision, "collision.txt", advanced, rejected} {
		if !strings.Contains(detail, want) {
			t.Fatalf("terminal detail %q missing %q", detail, want)
		}
	}
	if _, err := artifacts.NodeArtifact(rejected, "pr"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("rejected slice opened a PR/CONFLICTING PR: %v", err)
	}
}

func TestFreezeAllowsIdenticalSiblingCreateAndExistingFileEdits(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)
	ctx := t.Context()
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/aimee/feat/wi_parent", "aimee/feat/wi_parent")
	base := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", featureBaseRef(ctx, slicedir, "wi_parent")))
	if err := os.WriteFile(filepath.Join(slicedir, "same.txt"), []byte("same\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "same.txt")
	gitRun(t, slicedir, "commit", "-m", "same")
	head := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))

	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(t.TempDir(), "artifacts"))
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
	registry, err := wfe.NewRegistry(workflowDir)
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
	if _, err := artifacts.PutNodeArtifact("wi_s0", "freeze", "frozen_diff", []byte("diff")); err != nil {
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
	runner := &NativeRunner{db: store, artifacts: artifacts, workflows: registry}
	item, err := store.WorkItem(ctx, "wi_s1")
	if err != nil {
		t.Fatal(err)
	}
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, head); err != nil {
		t.Fatalf("identical create rejected: %v", err)
	}

	gitRun(t, repo, "checkout", "aimee/feat/wi_parent")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("sibling edit\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "sibling existing edit")
	siblingHead := strings.TrimSpace(gitRun(t, repo, "rev-parse", "HEAD"))
	gitRun(t, repo, "checkout", "trunk")
	gitRun(t, repo, "update-ref", "refs/remotes/origin/aimee/feat/wi_parent", base)
	if err := store.Move(ctx, "wi_s0", "gate", "merge", "advance", "", siblingHead, 0); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "checkout", "-q", "-B", "aimee/wi/wi_child", base)
	if err := os.WriteFile(filepath.Join(slicedir, "README"), []byte("current edit\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "README")
	gitRun(t, slicedir, "commit", "-m", "current existing edit")
	currentHead := strings.TrimSpace(gitRun(t, slicedir, "rev-parse", "HEAD"))
	if err := runner.rejectDivergentSiblingCreates(ctx, item, slicedir, base, currentHead); err != nil {
		t.Fatalf("existing-file edit rejected: %v", err)
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

type documentedNoopAgents struct{}

func (documentedNoopAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	return DelegateResult{
		Response: "Partial result; delegate ended with error: delegate code: no owned files changed; " +
			"result treated as incomplete\n\nNo changes made. Documentation is already complete.",
		Partial: true,
	}, nil
}

func (documentedNoopAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	return make([]DelegateGroupResult, len(requests))
}

func (a partialNoCommitAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	out := make([]DelegateGroupResult, len(requests))
	for i := range requests {
		out[i] = DelegateGroupResult{Response: "partial"}
	}
	return out
}

type rejectDelegateAgents struct{ calls int }

func (a *rejectDelegateAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	a.calls++
	return DelegateResult{}, errors.New("delegate must not run for an already-repaired reviewed tree")
}

func TestDocumentResumeRefreezesHumanRepairWithoutMeaninglessDelegateEdit(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("base\n"), 0o600); err != nil {
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
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_root", Repo: repo,
		ProposalPath: "p", WorkflowName: "build", StartStage: "document"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, "wi_root")
	workdir, branch, err := manager.Ensure(ctx, item, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workdir, "runbook.md"), []byte("reviewed\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "document")
	reviewedDiff := gitRun(t, workdir, "--no-pager", "diff", "origin/trunk...HEAD")
	reviewedHash := wfe.Hash([]byte(strings.TrimSpace(reviewedDiff)))

	if err := os.WriteFile(filepath.Join(workdir, "runbook.md"), []byte("reviewed and human-repaired\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	item, _ = store.WorkItem(ctx, "wi_root")
	item.ContentHash = reviewedHash
	item.Worktree = workdir
	agents := &rejectDelegateAgents{}
	runner := &NativeRunner{agents: agents, worktrees: manager, db: store}
	request := StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}
	if _, err := runner.mutate(ctx, request, true); err == nil || agents.calls != 1 {
		t.Fatalf("dirty repair bypassed delegate: calls=%d err=%v", agents.calls, err)
	}

	gitRun(t, workdir, "add", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "human repair")
	agents.calls = 0
	result, err := runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}, true)
	if err != nil {
		t.Fatal(err)
	}
	if agents.calls != 0 {
		t.Fatalf("delegate calls=%d, want 0", agents.calls)
	}
	if result.Status != StepAdvanced || result.Artifact != branch ||
		!strings.Contains(result.Detail, "re-freezing exact repair") {
		t.Fatalf("result=%+v", result)
	}
	if head := strings.TrimSpace(gitRun(t, workdir, "rev-parse", "HEAD")); result.ContentHash != head {
		t.Fatalf("content hash=%q, want repaired HEAD %q", result.ContentHash, head)
	}

	// Removing the entire reviewed diff is not a repair to re-freeze. The normal
	// delegate path must handle it rather than letting freeze accept an empty root
	// diff as a no-op without another roundtable review.
	gitRun(t, workdir, "rm", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "remove reviewed document")
	agents.calls = 0
	if _, err := runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}, true); err == nil || agents.calls != 1 {
		t.Fatalf("empty repair bypassed delegate: calls=%d err=%v", agents.calls, err)
	}
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

func TestDocumentPartialNoChangeAdvancesUnchangedHead(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("base\n"), 0o600); err != nil {
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
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_root", Repo: repo,
		ProposalPath: "p", WorkflowName: "build", StartStage: "document"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, "wi_root")
	workdir, branch, err := manager.Ensure(ctx, item, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workdir, "docs.md"), []byte("already documented\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "docs.md")
	gitRun(t, workdir, "commit", "-m", "accepted implementation")
	head := strings.TrimSpace(gitRun(t, workdir, "rev-parse", "HEAD"))
	item, _ = store.WorkItem(ctx, "wi_root")

	runner := &NativeRunner{agents: documentedNoopAgents{}, worktrees: manager, db: store}
	out, err := runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Proposal: "Document the accepted implementation."}, true)
	if err != nil {
		t.Fatalf("mutate: %v", err)
	}
	if out.Status != StepAdvanced || out.Artifact != branch || out.ContentHash != head {
		t.Fatalf("result=%+v, want unchanged advanced HEAD %s", out, head)
	}
	if got := strings.TrimSpace(gitRun(t, workdir, "status", "--porcelain")); got != "" {
		t.Fatalf("document no-op dirtied the worktree: %q", got)
	}
}
