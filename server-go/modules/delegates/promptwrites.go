package delegates

import "strings"

// Whether a delegate may write, from its role AND what it was asked to do.
//
// Two inputs, one answer, decided together. The role's default is not the whole
// story: a write role told to inspect something must not be handed a writable
// tree, because the mount is the enforcement and there is nothing above it that
// would stop the edit. Composing the two here means the answer that reaches the
// worktree plan and the container spec is the same answer, rather than two
// halves a caller recombines.

// noWriteWholePrompt are phrases that rule out writing outright. They describe
// the WHOLE task, so no later keyword rescues them: "read-only: fix the typo"
// is a contradiction, and the safe reading of a contradiction is the one that
// does not edit the user's files.
var noWriteWholePrompt = []string{
	"do not edit files",
	"do not modify anything",
	"do not write files",
	"do not change files",
	"do not make changes",
	"read-only",
	"read only",
	"inspect only",
	"analysis only",
}

// noWriteScoped are narrower prohibitions -- "do not edit the config" -- which
// forbid something specific rather than everything. On their own they mean no
// writing; alongside an explicit ask to create or change something, they are a
// boundary on a task that IS a write task.
var noWriteScoped = []string{
	"do not edit", "do not modify", "do not write", "do not change",
}

// scopedWriteIntent rescues a scoped prohibition. Deliberately narrower than
// writeIntent: it takes a clear ask, not any mention of a word like "edit".
var scopedWriteIntent = []string{
	"create", "new file", "add ", "add file", "implement ", "update",
	"fix", "refactor", "delete", "remove",
}

// writeIntent is what asking for a change looks like.
var writeIntent = []string{
	"create", "new file", "add file", "edit", "modify", "update", "fix",
	"implement", "write", "refactor", "delete", "remove",
}

func containsAnyFold(haystack string, needles []string) bool {
	for _, n := range needles {
		if strings.Contains(haystack, n) {
			return true
		}
	}
	return false
}

// PromptAllowsWrites reads a delegate's brief for whether it asks for changes.
//
// An EMPTY prompt allows writes: there is nothing to read, so this rule
// abstains and the role decides alone. It is a narrowing rule, not a granting
// one -- it can only ever take permission away from a role that had it.
func PromptAllowsWrites(prompt string) bool {
	if prompt == "" {
		return true
	}
	lower := strings.ToLower(prompt)

	if containsAnyFold(lower, noWriteWholePrompt) {
		return false
	}

	// A scoped prohibition with no write asked for anywhere is a read-only task.
	if containsAnyFold(lower, noWriteScoped) && !containsAnyFold(lower, scopedWriteIntent) {
		return false
	}

	return containsAnyFold(lower, writeIntent)
}

// DelegateMayWrite is the composed answer: the role permits writing AND the
// brief asks for it.
//
// Both must hold. The role alone would hand a writable tree to a review that
// happens to run under a write role; the prompt alone would let any brief
// mentioning "fix" write from a role that has no business doing so.
func DelegateMayWrite(role, prompt string) bool {
	return RoleIsWrite(role) && PromptAllowsWrites(prompt)
}
