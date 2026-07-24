package engine

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
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
