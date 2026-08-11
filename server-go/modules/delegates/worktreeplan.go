package delegates

import (
	"fmt"
	"strings"
)

// What a delegate needs from the workspace before it can run.
//
// A write delegate needs its own branch and worktree, so its edits land
// somewhere the supervisor can inspect and merge deliberately. A read-only
// delegate needs nothing created at all -- it mounts the parent's worktree, and
// cutting a worktree for it would be work whose only product is a directory to
// clean up.
//
// This decides WHAT TO ASK FOR. The workspace module owns worktrees and does
// the cutting; delegates does not create one, name the branch, or -- and this
// is the part that matters -- choose the base ref.

// WorktreePlan is the request to put to the workspace module.
type WorktreePlan struct {
	// Isolated means the delegate needs its own branch and worktree. False
	// means it uses the parent's, and nothing is created.
	Isolated bool
	// WorkName distinguishes this delegate's worktree from its siblings under
	// the same session. Empty when not isolated.
	WorkName string
	// ReadOnlyMount is true when the resulting mount must be read-only. It
	// tracks Isolated inversely and is stated separately because it is what the
	// sandbox spec consumes, and the two must not drift apart.
	ReadOnlyMount bool
}

// workNameSafe reports whether a work name can appear in a branch name and a
// directory name.
//
// Branch names go into git commands and directory names into mount paths, so
// the set is narrow on purpose: a slash would nest a branch namespace, a space
// or a quote would split an argument, and a leading dash would read as a flag.
func workNameSafe(name string) bool {
	if name == "" || len(name) > 64 {
		return false
	}
	if !isAlnum(name[0]) {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if isAlnum(c) || c == '-' || c == '_' {
			continue
		}
		return false
	}
	return true
}

// PlanWorktree decides what a delegate needs from the workspace.
//
// The work name is derived from the delegate's own identity so two delegates in
// one session do not collide on a worktree -- that collision previously put two
// delegates in the same directory, each overwriting the other's edits.
//
// There is deliberately NO base ref here. The workspace resolves the base by
// policy and fails hard when it cannot: guessing one is what let a session
// inherit another session's branch, and a delegate is in no better position to
// guess than a session was.
func PlanWorktree(role, delegateID string) (WorktreePlan, error) {
	if !RoleIsWrite(role) {
		// Nothing to create: the parent's worktree, mounted read-only.
		return WorktreePlan{ReadOnlyMount: true}, nil
	}
	name := strings.TrimSpace(delegateID)
	if !workNameSafe(name) {
		return WorktreePlan{}, fmt.Errorf(
			"delegate id %q cannot name a branch or directory", delegateID)
	}
	return WorktreePlan{Isolated: true, WorkName: name}, nil
}

// SandboxRequestFor assembles the sandbox request once the workspace has
// answered.
//
// worktree and gitDir are what the workspace returned: for an isolated delegate
// its own, for a read-only one the parent's worktree and an empty git dir --
// nothing writable is needed because nothing is written.
//
// Keeping this next to PlanWorktree is deliberate: the plan's ReadOnlyMount and
// the spec's mount modes are the same decision, and computing them in one place
// stops a future edit from making a read-only delegate a writable mount.
func SandboxRequestFor(plan WorktreePlan, role, repoRoot, worktree, gitDir string,
	isGitCheckout bool, socketHost, socketTarget, egressProxy string) SandboxRequest {
	req := SandboxRequest{
		Role:               role,
		RepoRoot:           repoRoot,
		Worktree:           worktree,
		IsGitCheckout:      isGitCheckout,
		ParentSocketHost:   socketHost,
		ParentSocketTarget: socketTarget,
		EgressProxy:        egressProxy,
	}
	if plan.Isolated {
		req.GitDir = gitDir
	}
	return req
}
