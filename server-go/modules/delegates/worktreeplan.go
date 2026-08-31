package delegates

import (
	"fmt"
	"strings"
)

// What a delegate needs from the workspace before it can run.
//
// Every repository-backed delegate needs its own branch and worktree. Write
// delegates use that boundary for changes; read-only delegates mount their own
// checkout read-only rather than sharing another session's live directory.
//
// This decides WHAT TO ASK FOR. The workspace module owns worktrees and does
// the cutting; delegates does not create one, name the branch, or -- and this
// is the part that matters -- choose the base ref.

// WorktreePlan is the request to put to the workspace module.
type WorktreePlan struct {
	// Isolated means the delegate needs its own branch and worktree. Repository
	// delegates are always isolated; read-only is a mount mode, never a reason
	// to share another session's checkout.
	Isolated bool
	// WorkName distinguishes this delegate's worktree from its siblings under
	// the same session.
	WorkName string
	// ReadOnlyMount is true when the resulting mount must be read-only. It
	// is independent of Isolated: a read-only delegate receives its own
	// worktree, bound read-only.
	ReadOnlyMount bool
}

// workNameMax bounds a work name. The wire field that carries one is sized from
// this, so a name this module accepts can never be truncated on the way out
// into a different branch than the one it decided on.
const workNameMax = 64

// workNameSafe reports whether a work name can appear in a branch name and a
// directory name.
//
// Branch names go into git commands and directory names into mount paths, so
// the set is narrow on purpose: a slash would nest a branch namespace, a space
// or a quote would split an argument, and a leading dash would read as a flag.
func workNameSafe(name string) bool {
	if name == "" || len(name) > workNameMax {
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
//
// writesAllowed is the caller's composed answer, not the role's default. It
// selects the mount mode, not whether isolation exists.
func PlanWorktree(writesAllowed bool, delegateID string) (WorktreePlan, error) {
	name := strings.TrimSpace(delegateID)
	if !workNameSafe(name) {
		return WorktreePlan{}, fmt.Errorf(
			"delegate id %q cannot name a branch or directory", delegateID)
	}
	return WorktreePlan{Isolated: true, WorkName: name, ReadOnlyMount: !writesAllowed}, nil
}

// SandboxRequestFor assembles the sandbox request once the workspace has
// answered.
//
// worktree and gitDir are what the workspace returned. Every delegate receives
// its own worktree and Git identity; mount policy decides which metadata, if
// any, may be writable.
//
// Keeping this next to PlanWorktree is deliberate: the plan's ReadOnlyMount and
// the spec's mount modes are the same decision, and computing them in one place
// stops a future edit from making a read-only delegate a writable mount.
func SandboxRequestFor(plan WorktreePlan, repoRoot, worktree, gitDir string,
	isGitCheckout bool, socketHost, socketTarget, egressProxy string) SandboxRequest {
	req := SandboxRequest{
		WritesAllowed:      !plan.ReadOnlyMount,
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
