package delegates

import "testing"

// The plain cases: a brief that asks for a change allows one.
func TestPromptAllowsWritesOnAsk(t *testing.T) {
	for _, prompt := range []string{
		"implement the retry logic",
		"fix the null deref in parser.c",
		"refactor the handler into two functions",
		"create a new file for the adapter",
		"delete the dead branch",
		"UPDATE THE README", // case-insensitive
	} {
		if !PromptAllowsWrites(prompt) {
			t.Errorf("%q should allow writes", prompt)
		}
	}
}

// A brief with no ask at all does not get a writable tree just because its role
// could write.
func TestPromptWithoutAskDoesNotAllowWrites(t *testing.T) {
	for _, prompt := range []string{
		"what does this function do?",
		"summarise the architecture",
		"which tests cover the parser",
	} {
		if PromptAllowsWrites(prompt) {
			t.Errorf("%q should not allow writes", prompt)
		}
	}
}

// A whole-prompt prohibition wins outright. "read-only: fix the typo" is a
// contradiction, and the safe reading of a contradiction is the one that does
// not edit the user's files.
func TestWholePromptProhibitionIsNotRescued(t *testing.T) {
	for _, prompt := range []string{
		"read-only: fix the typo in the header",
		"analysis only. implement nothing, just report",
		"do not edit files, but tell me how you would refactor it",
		"inspect only -- update me on what you find",
		"read only: create a plan for the migration",
	} {
		if PromptAllowsWrites(prompt) {
			t.Errorf("%q must not allow writes: the prohibition covers the whole task", prompt)
		}
	}
}

// A SCOPED prohibition forbids something specific. Alone it means no writing;
// alongside a real ask it is a boundary on a task that is still a write task.
func TestScopedProhibitionNarrowsRatherThanForbids(t *testing.T) {
	if PromptAllowsWrites("do not edit the generated files") {
		t.Error("a scoped prohibition with no ask should not allow writes")
	}
	for _, prompt := range []string{
		"implement the parser, but do not edit the generated files",
		"fix the leak; do not modify the public header",
	} {
		if !PromptAllowsWrites(prompt) {
			t.Errorf("%q is a write task with a boundary, not a read-only task", prompt)
		}
	}
}

// Nothing to read means this rule abstains: it can only take permission away.
func TestEmptyPromptAbstains(t *testing.T) {
	if !PromptAllowsWrites("") {
		t.Error("an empty prompt should leave the decision to the role")
	}
}

// The composed answer needs BOTH. The role alone would hand a writable tree to
// a review running under a write role; the prompt alone would let any brief
// mentioning "fix" write from a role with no business doing so.
func TestDelegateMayWriteNeedsBoth(t *testing.T) {
	if !DelegateMayWrite("code", "fix the parser") {
		t.Error("a write role with a write ask should be allowed to write")
	}
	if DelegateMayWrite("code", "read-only: review the parser") {
		t.Error("a write role told to read must not get a writable tree")
	}
	if DelegateMayWrite("review", "fix the parser") {
		t.Error("a read-only role must not write however the brief is worded")
	}
	if DelegateMayWrite("review", "summarise the parser") {
		t.Error("neither input allows writing")
	}
	// An alias resolves through the role rule, so it behaves as what it names.
	if !DelegateMayWrite("implement", "fix the parser") {
		t.Error("an alias for a write role should behave as that role")
	}
}
