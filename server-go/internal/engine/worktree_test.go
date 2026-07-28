package engine

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
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
	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"}, {ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_parent"}} {
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

	store, err := db1.Open(filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{
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
