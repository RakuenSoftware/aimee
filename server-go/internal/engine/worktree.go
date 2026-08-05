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
	expected := filepath.Join(m.root, item.ID)
	candidate := expected
	if item.Worktree != "" {
		candidate, _ = filepath.Abs(item.Worktree)
	}
	if info, err := os.Stat(candidate); err == nil && info.IsDir() && candidate == expected {
		top, topErr := gitText(ctx, candidate, "rev-parse", "--show-toplevel")
		actual, branchErr := gitText(ctx, candidate, "rev-parse", "--abbrev-ref", "HEAD")
		if topErr == nil && branchErr == nil && filepath.Clean(top) == candidate {
			if actual == branch {
				if item.Worktree != candidate {
					if err := m.db.SetWorktree(ctx, item.ID, candidate); err != nil {
						return "", "", err
					}
				}
				return candidate, branch, nil
			}
			// Delegate repair turns can create a temporary side branch while
			// editing, then leave the managed worktree attached to that alias.
			// If both refs name the exact same commit, switching back to the
			// durable branch is lossless. Do not guess when either the commit or
			// the worktree differs: that may contain work requiring a human.
			if target, targetErr := gitText(ctx, item.Repo, "rev-parse", "--verify", branch+"^{commit}"); targetErr == nil {
				current, currentErr := gitText(ctx, candidate, "rev-parse", "HEAD^{commit}")
				status, statusErr := gitText(ctx, candidate, "status", "--porcelain")
				if currentErr == nil && statusErr == nil && status == "" && target == current {
					if _, switchErr := gitText(ctx, candidate, "switch", branch); switchErr != nil {
						return "", "", fmt.Errorf("restore durable worktree branch %q from identical alias %q: %w", branch, actual, switchErr)
					}
					if err := m.db.SetWorktree(ctx, item.ID, candidate); err != nil {
						return "", "", err
					}
					return candidate, branch, nil
				}
			}
			// The former C engine named a generated slice
			// <parent>-<slice-hash> while Go uses the durable child item ID. A
			// crash/replay can lose the DB path after implementation but leave
			// this registered worktree and its commits. Rename that exact legacy
			// branch in place; deleting/recreating the tree would discard work.
			if legacySliceBranch(item.ID) == actual {
				if _, existsErr := gitText(ctx, item.Repo, "rev-parse", "--verify", branch+"^{commit}"); existsErr == nil {
					return "", "", fmt.Errorf("managed worktree uses legacy branch %q but target branch %q has different work", actual, branch)
				}
				if _, renameErr := gitText(ctx, candidate, "branch", "-m", branch); renameErr != nil {
					return "", "", fmt.Errorf("migrate legacy worktree branch: %w", renameErr)
				}
				if err := m.db.SetWorktree(ctx, item.ID, candidate); err != nil {
					return "", "", err
				}
				return candidate, branch, nil
			}
		}
		return "", "", fmt.Errorf("managed worktree path %q already exists with an unexpected repository or branch", candidate)
	}
	if item.Worktree != "" {
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
	path := expected
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
		// A slice merges through the forge, which advances the REMOTE feature
		// branch. This local ref stays wherever the run started, so branching a
		// later slice from it hands the delegate a tree missing every slice that
		// already landed. It then recreates those files and the merge is an
		// add/add conflict no retry can resolve. Prefer the remote tip, exactly
		// as a feature worktree already prefers origin/<trunk> above, and fall
		// back to the local ref when there is no remote (offline or fresh repo).
		remote := "origin/" + base
		_, _ = gitText(ctx, repo, "fetch", "--quiet", "origin",
			"+refs/heads/"+base+":refs/remotes/"+remote)
		if _, checkErr := gitText(ctx, repo, "rev-parse", "--verify", remote+"^{commit}"); checkErr == nil {
			base = remote
		}
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

func legacySliceBranch(id string) string {
	slice := strings.Index(id, ".s")
	if slice < 0 {
		return ""
	}
	generation := strings.Index(id[slice+2:], ".g")
	if generation < 0 {
		return ""
	}
	suffix := id[slice+1 : slice+2+generation]
	if suffix == "" {
		return ""
	}
	return "aimee/wi/" + id[:slice] + "-" + suffix
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
	// Cleanup is a two-step lifecycle operation: remove the checkout, then clear
	// its durable path. If the database write fails after removal, the scheduler
	// retries with the old row. Treat that already-completed physical step as
	// success so the retry can finish clearing durable state.
	if _, statErr := os.Stat(abs); errors.Is(statErr, os.ErrNotExist) {
		return nil
	} else if statErr != nil {
		return fmt.Errorf("stat managed worktree: %w", statErr)
	}
	// The repository can be gone while its worktrees remain -- a deleted webuser
	// workspace leaves exactly that. Every `git -C <repo>` below then fails with
	// "cannot change to <repo>", Cleanup returns an error forever, and the item
	// never reaches a terminal state: observed on a live server retrying one work
	// item ~92 times a minute against a repo directory that no longer existed.
	//
	// There is nothing to deregister in that case. The administrative entry lived
	// in the repo's .git/worktrees and went with it, so the checkout is an orphan
	// and removing the directory IS the cleanup. Safe because abs was already
	// validated to sit inside the managed root above.
	if _, repoErr := os.Stat(item.Repo); errors.Is(repoErr, os.ErrNotExist) {
		if rmErr := os.RemoveAll(abs); rmErr != nil {
			return fmt.Errorf("remove orphaned worktree %s: %w", abs, rmErr)
		}
		return nil
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

// repoIntegrationBranch returns the branch whose checkout admitted the
// proposal. A repository can deliberately run workflows against a non-default
// integration lane (for example testing), while origin/HEAD points somewhere
// else. Final PRs must target this checkout branch; the C forge boundary
// independently resolves and enforces the same trusted value.
func repoIntegrationBranch(ctx context.Context, repo string) (string, error) {
	branch, err := gitText(ctx, repo, "branch", "--show-current")
	if err != nil || branch == "" {
		return "", errors.New("repository integration branch is unresolved")
	}
	if strings.HasPrefix(branch, "-") {
		return "", errors.New("repository integration branch is invalid")
	}
	if _, err := gitText(ctx, repo, "check-ref-format", "--branch", branch); err != nil {
		return "", errors.New("repository integration branch is invalid")
	}
	return branch, nil
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
