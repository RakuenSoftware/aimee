package engine

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
	"github.com/JBailes/aimee/server-go/internal/workflowstore/workflowstoretest"
)

func TestParentUsesFeatureWorktreeAndChildBranchesFromIt(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example", "GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-b", "trunk", repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")
	run("-C", repo, "remote", "add", "origin", repo)
	run("-C", repo, "update-ref", "refs/remotes/origin/trunk", "HEAD")
	run("-C", repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/trunk")
	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	for _, in := range []workflowstore.CreateWorkItem{{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"}, {ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_parent"}} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	parent, _ := store.WorkItem(ctx, "wi_parent")
	_, branch, err := manager.Ensure(ctx, parent, true)
	if err != nil {
		t.Fatal(err)
	}
	if branch != "aimee/feat/wi_parent" {
		t.Fatalf("parent branch=%s", branch)
	}
	child, _ := store.WorkItem(ctx, "wi_child")
	_, branch, err = manager.Ensure(ctx, child, false)
	if err != nil {
		t.Fatal(err)
	}
	if branch != "aimee/wi/wi_child" {
		t.Fatalf("child branch=%s", branch)
	}
}

func TestCleanupIsIdempotentAfterManagedPathWasRemoved(t *testing.T) {
	root := t.TempDir()
	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	item := workflowstore.WorkItem{ID: "wi_missing", Repo: filepath.Join(root, "repo"),
		Worktree: filepath.Join(root, "trees", "wi_missing")}
	if err := manager.Cleanup(t.Context(), item); err != nil {
		t.Fatalf("repeat cleanup of removed managed path: %v", err)
	}

	item.Worktree = filepath.Join(root, "outside-managed-root")
	if err := manager.Cleanup(t.Context(), item); err == nil {
		t.Fatal("missing path outside the managed root bypassed scope validation")
	}
}

func TestCleanupRemovesOrphanedWorktreeWhenTheRepoIsGone(t *testing.T) {
	// A deleted workspace leaves its worktrees behind. Every `git -C <repo>` then
	// fails with "cannot change to <repo>", so Cleanup used to return an error
	// forever and the work item never reached a terminal state -- observed on a
	// live server retrying one item ~92 times a minute.
	root := t.TempDir()
	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}

	// The checkout exists; the repository it belonged to does not.
	tree := filepath.Join(root, "trees", "wi_orphan")
	if err := os.MkdirAll(tree, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(tree, "file"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	item := workflowstore.WorkItem{ID: "wi_orphan", Repo: filepath.Join(root, "repo-that-was-deleted"),
		Worktree: tree}

	if err := manager.Cleanup(t.Context(), item); err != nil {
		t.Fatalf("cleanup with a missing repo must succeed, got: %v", err)
	}
	if _, err := os.Stat(tree); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("orphaned worktree still present after cleanup: %v", err)
	}

	// Scope validation still applies: a missing repo is not a licence to delete
	// anything outside the managed root.
	outside := filepath.Join(root, "outside-managed-root")
	if err := os.MkdirAll(outside, 0o700); err != nil {
		t.Fatal(err)
	}
	item.Worktree = outside
	if err := manager.Cleanup(t.Context(), item); err == nil {
		t.Fatal("a missing repo bypassed managed-root scope validation")
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatalf("path outside the managed root was removed: %v", err)
	}
}

func TestEnsureMigratesLegacySliceWorktreeAfterReplayLosesDBPath(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-b", "testing", repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")

	id := "wi_88955dcd207cfd11986175109e3effcc.sa90502568a.g0.0"
	legacy := legacySliceBranch(id)
	if legacy != "aimee/wi/wi_88955dcd207cfd11986175109e3effcc-sa90502568a" {
		t.Fatalf("legacy branch=%q", legacy)
	}
	run("-C", repo, "branch", legacy)
	trees := filepath.Join(root, "trees")
	if err := os.MkdirAll(trees, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(trees, id)
	run("-C", repo, "worktree", "add", "--lock", path, legacy)
	if err := os.WriteFile(filepath.Join(path, "implemented.txt"), []byte("preserve me\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", path, "add", "implemented.txt")
	run("-C", path, "commit", "-m", "implementation")

	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: "wi_parent", Repo: repo,
		ProposalPath: "parent", WorkflowName: "build", StartStage: "slices"}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: id, Repo: repo, ProposalPath: "p",
		WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, trees)
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, id)
	gotPath, gotBranch, err := manager.Ensure(ctx, item, false)
	if err != nil {
		t.Fatal(err)
	}
	wantBranch := "aimee/wi/" + id
	if gotPath != path || gotBranch != wantBranch {
		t.Fatalf("got path=%q branch=%q", gotPath, gotBranch)
	}
	actual, err := gitText(ctx, path, "branch", "--show-current")
	if err != nil || actual != wantBranch {
		t.Fatalf("renamed branch=%q err=%v", actual, err)
	}
	if data, err := os.ReadFile(filepath.Join(path, "implemented.txt")); err != nil || string(data) != "preserve me\n" {
		t.Fatalf("implementation was not preserved: %q err=%v", data, err)
	}
	item, _ = store.WorkItem(ctx, id)
	if item.Worktree != path {
		t.Fatalf("persisted worktree=%q", item.Worktree)
	}
}

func TestEnsureMigratesLegacySliceWhenIdenticalTargetRefAlreadyExists(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-b", "testing", repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")

	id := "wi_parent.sa90502568a.g0.0"
	legacy := legacySliceBranch(id)
	target := "aimee/wi/" + id
	run("-C", repo, "branch", legacy)
	run("-C", repo, "branch", target)
	trees := filepath.Join(root, "trees")
	if err := os.MkdirAll(trees, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(trees, id)
	run("-C", repo, "worktree", "add", "--lock", path, legacy)

	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: "wi_parent", Repo: repo,
		ProposalPath: "parent", WorkflowName: "build", StartStage: "slices"}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: id, Repo: repo, ProposalPath: "p",
		WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, trees)
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, id)
	gotPath, gotBranch, err := manager.Ensure(ctx, item, false)
	if err != nil {
		t.Fatal(err)
	}
	if gotPath != path || gotBranch != target {
		t.Fatalf("got path=%q branch=%q", gotPath, gotBranch)
	}
	actual, err := gitText(ctx, path, "branch", "--show-current")
	if err != nil || actual != target {
		t.Fatalf("renamed branch=%q err=%v", actual, err)
	}
}

func TestEnsureRestoresDurableBranchFromIdenticalDelegateAlias(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-b", "testing", repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")

	id := "wi_parent.sa90502568a.g0.0"
	target := "aimee/wi/" + id
	alias := "aimee/wi/sa90502568a-doc-fix-v3"
	run("-C", repo, "branch", target)
	run("-C", repo, "branch", alias)
	trees := filepath.Join(root, "trees")
	if err := os.MkdirAll(trees, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(trees, id)
	run("-C", repo, "worktree", "add", "--lock", path, alias)

	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: "wi_parent", Repo: repo,
		ProposalPath: "parent", WorkflowName: "build", StartStage: "slices"}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, workflowstore.CreateWorkItem{ID: id, Repo: repo, ProposalPath: "p",
		WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, trees)
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, id)
	dirtyPath := filepath.Join(path, "uncommitted.txt")
	if err := os.WriteFile(dirtyPath, []byte("preserve me"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := manager.Ensure(ctx, item, false); err == nil {
		t.Fatal("restored durable branch despite uncommitted alias work")
	}
	if data, err := os.ReadFile(dirtyPath); err != nil || string(data) != "preserve me" {
		t.Fatalf("uncommitted alias work was not preserved: %q err=%v", data, err)
	}
	if err := os.Remove(dirtyPath); err != nil {
		t.Fatal(err)
	}
	gotPath, gotBranch, err := manager.Ensure(ctx, item, false)
	if err != nil {
		t.Fatal(err)
	}
	if gotPath != path || gotBranch != target {
		t.Fatalf("got path=%q branch=%q", gotPath, gotBranch)
	}
	actual, err := gitText(ctx, path, "branch", "--show-current")
	if err != nil || actual != target {
		t.Fatalf("restored branch=%q err=%v", actual, err)
	}
}

func TestRepoIntegrationBranchUsesAdmittedCheckoutNotOriginHEAD(t *testing.T) {
	repo := t.TempDir()
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-b", "testing", repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")
	run("-C", repo, "update-ref", "refs/remotes/origin/other", "HEAD")
	run("-C", repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/other")

	branch, err := repoIntegrationBranch(context.Background(), repo)
	if err != nil || branch != "testing" {
		t.Fatalf("integration branch=%q err=%v", branch, err)
	}
}

// A slice merges through the forge, which advances the REMOTE feature branch and
// leaves the local ref where the run started. A later slice branched from that
// stale local ref gets a tree missing the work earlier slices already landed, so
// it recreates the same files and its merge becomes an add/add conflict that no
// retry can resolve. The slice worktree must therefore branch from the remote
// tip. Observed live: wi_f96d4b18 kept local aimee/feat/... at e161dd34 while the
// remote sat at da80f8e7, and slices g0.1/g0.2 both re-created a file g0.0 had
// already merged.
func TestSliceWorktreeBranchesFromMergedRemoteFeatureTip(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "--bare", "-b", "trunk", origin)
	run("clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", repo, "add", "README")
	run("-C", repo, "commit", "-m", "init")
	run("-C", repo, "push", "-u", "origin", "trunk")

	// The feature branch exists locally and remotely at the run's starting point.
	feature := "aimee/feat/wi_parent"
	run("-C", repo, "branch", feature)
	run("-C", repo, "push", "origin", feature)

	// An earlier slice lands through the forge: the REMOTE feature branch gains a
	// file while the local ref deliberately stays behind, reproducing the defect.
	landed := filepath.Join(root, "landed")
	run("clone", "-b", feature, origin, landed)
	if err := os.WriteFile(filepath.Join(landed, "slice-0.txt"), []byte("landed by g0.0\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run("-C", landed, "add", "slice-0.txt")
	run("-C", landed, "commit", "-m", "slice g0.0")
	run("-C", landed, "push", "origin", feature)

	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	for _, in := range []workflowstore.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_parent.s1", Repo: repo, ProposalPath: "s1", WorkflowName: "slice",
			StartStage: "impl", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	child, _ := store.WorkItem(ctx, "wi_parent.s1")
	path, _, err := manager.Ensure(ctx, child, false)
	if err != nil {
		t.Fatal(err)
	}
	// The next slice must SEE the file the previous slice landed. Without it the
	// delegate recreates that file and the merge conflicts terminally.
	if _, statErr := os.Stat(filepath.Join(path, "slice-0.txt")); statErr != nil {
		t.Fatalf("slice worktree branched from a stale base: %v", statErr)
	}
}
