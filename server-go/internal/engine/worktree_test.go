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
