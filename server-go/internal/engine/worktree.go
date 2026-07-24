package engine

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

type WorktreeManager struct {
	db    *db1.Store
	root  string
	mu    sync.Mutex
	locks map[string]*sync.Mutex
}

func NewWorktreeManager(db *db1.Store, root string) (*WorktreeManager, error) {
	if db == nil || root == "" {
		return nil, errors.New("DB1 and worktree root are required")
	}
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, err
	}
	return &WorktreeManager{db: db, root: abs, locks: make(map[string]*sync.Mutex)}, nil
}

func (m *WorktreeManager) Ensure(ctx context.Context, item db1.WorkItem, feature bool) (string, string, error) {
	m.mu.Lock()
	lock := m.locks[item.ID]
	if lock == nil {
		lock = &sync.Mutex{}
		m.locks[item.ID] = lock
	}
	m.mu.Unlock()
	lock.Lock()
	defer lock.Unlock()
	branch := "aimee/wi/" + item.ID
	if feature {
		branch = "aimee/feat/" + item.ID
	}
	if item.Worktree != "" {
		expected := filepath.Join(m.root, item.ID)
		abs, _ := filepath.Abs(item.Worktree)
		if info, err := os.Stat(abs); err == nil && info.IsDir() && abs == expected {
			top, topErr := gitText(ctx, abs, "rev-parse", "--show-toplevel")
			actual, branchErr := gitText(ctx, abs, "rev-parse", "--abbrev-ref", "HEAD")
			if topErr == nil && branchErr == nil && filepath.Clean(top) == abs && actual == branch {
				return abs, branch, nil
			}
		}
		if err := m.db.SetWorktree(ctx, item.ID, ""); err != nil {
			return "", "", err
		}
	}
	repo, err := filepath.Abs(item.Repo)
	if err != nil {
		return "", "", err
	}
	if info, statErr := os.Stat(repo); statErr != nil || !info.IsDir() {
		return "", "", errors.New("workflow repository is not a local directory")
	}
	path := filepath.Join(m.root, item.ID)
	base := "HEAD"
	if feature {
		trunk, trunkErr := repoDefaultBranch(ctx, repo)
		if trunkErr != nil {
			return "", "", trunkErr
		}
		base = "origin/" + trunk
		if _, checkErr := gitText(ctx, repo, "rev-parse", "--verify", base+"^{commit}"); checkErr != nil {
			base = trunk
		}
	} else if item.ParentID != "" {
		base = "aimee/feat/" + item.ParentID
	}
	if _, err := gitText(ctx, repo, "rev-parse", "--verify", base+"^{commit}"); err != nil {
		return "", "", fmt.Errorf("resolve worktree base %q: %w", base, err)
	}
	args := []string{"worktree", "add", "--lock", "-b", branch, path, base}
	if _, err := gitText(ctx, repo, args...); err != nil {
		// Re-entry after a crash may leave the branch but not the recorded path.
		if _, branchErr := gitText(ctx, repo, "rev-parse", "--verify", branch+"^{commit}"); branchErr != nil {
			return "", "", err
		}
		if _, retryErr := gitText(ctx, repo, "worktree", "add", "--lock", path, branch); retryErr != nil {
			return "", "", retryErr
		}
	}
	if err := m.db.SetWorktree(ctx, item.ID, path); err != nil {
		return "", "", err
	}
	return path, branch, nil
}

func (m *WorktreeManager) Cleanup(ctx context.Context, item db1.WorkItem) error {
	if item.Worktree == "" {
		return nil
	}
	abs, err := filepath.Abs(item.Worktree)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(m.root, abs)
	if err != nil || rel == "." || filepath.IsAbs(rel) || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return errors.New("refusing to clean worktree outside managed root")
	}
	// Ensure creates every managed tree with --lock so external GC cannot race a
	// live workflow. Explicit lifecycle deletion owns this path, so unlock it
	// before the validated removal; a missing lock is harmless.
	_, _ = gitText(ctx, item.Repo, "worktree", "unlock", abs)
	_, err = gitText(ctx, item.Repo, "worktree", "remove", "--force", abs)
	return err
}

func repoDefaultBranch(ctx context.Context, repo string) (string, error) {
	ref, err := gitText(ctx, repo, "symbolic-ref", "--short", "refs/remotes/origin/HEAD")
	if err != nil {
		out, ghErr := ghText(ctx, repo, "repo", "view", "--json", "defaultBranchRef", "--jq", ".defaultBranchRef.name")
		if ghErr != nil || strings.TrimSpace(out) == "" {
			return "", errors.New("repository default branch is unresolved")
		}
		ref = strings.TrimSpace(out)
	}
	ref = strings.TrimPrefix(ref, "origin/")
	if ref == "" || strings.ContainsAny(ref, "\r\n") {
		return "", errors.New("repository default branch is invalid")
	}
	return ref, nil
}

func gitText(ctx context.Context, repo string, args ...string) (string, error) {
	commandArgs := append([]string{"-C", repo}, args...)
	cmd := exec.CommandContext(ctx, "git", commandArgs...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("git %s: %s", strings.Join(args, " "), strings.TrimSpace(string(output)))
	}
	return strings.TrimSpace(string(output)), nil
}
