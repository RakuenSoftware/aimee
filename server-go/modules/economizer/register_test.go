package economizer

import "testing"

// Ported one-for-one from the retired C register suite so the Go
// implementation is pinned to the behaviour it replaces.

func TestRegisterGlyphs(t *testing.T) {
	cases := []struct {
		in   string
		want Register
	}{
		{"\U0001F3C1 done it", RegVerdict},    // 🏁
		{"▶ running", RegExecuting},           // ▶
		{"⚠ careful", RegHazard},              // ⚠
		{"❓ stuck", RegBlocked},               // ❓
		{"\U0001F50D looking", RegInProgress}, // 🔍
	}
	for _, c := range cases {
		if got := ParseRegister(c.in); got != c.want {
			t.Errorf("ParseRegister(%q) = %v, want %v", c.in, got, c.want)
		}
	}
}

func TestRegisterBracketTags(t *testing.T) {
	cases := []struct {
		in   string
		want Register
	}{
		{"[verdict] the fix works", RegVerdict},
		{"  [DONE] ok", RegVerdict}, // leading whitespace + case-insensitive
		{"[hazard] careful", RegHazard},
		{"[warning] x", RegHazard},
		{"[exec] building", RegExecuting},
		{"[blocked] need input", RegBlocked},
		{"[wip] thinking", RegInProgress},
	}
	for _, c := range cases {
		if got := ParseRegister(c.in); got != c.want {
			t.Errorf("ParseRegister(%q) = %v, want %v", c.in, got, c.want)
		}
	}
}

func TestRegisterDefaultsAndSettled(t *testing.T) {
	for _, in := range []string{"", "just some prose"} {
		if got := ParseRegister(in); got != RegInProgress {
			t.Errorf("ParseRegister(%q) = %v, want RegInProgress", in, got)
		}
	}
	if !RegVerdict.IsSettled() || !RegHazard.IsSettled() {
		t.Error("verdict and hazard must be settled")
	}
	if RegInProgress.IsSettled() || RegExecuting.IsSettled() {
		t.Error("in-progress and executing must not be settled")
	}
	if RegVerdict.Label() != "verdict" {
		t.Errorf("Label() = %q, want %q", RegVerdict.Label(), "verdict")
	}
}

// Only exact, closing-bracket-anchored tags match — no prefix false-positives.
func TestRegisterBracketAnchor(t *testing.T) {
	cases := []struct {
		in   string
		want Register
	}{
		{"[executable] running", RegInProgress},
		{"[verdicts] plural", RegInProgress},
		{"[doner] kebab", RegInProgress},
		{"[exec] x", RegExecuting},      // exact still matches
		{"[executing] x", RegExecuting}, // exact still matches
	}
	for _, c := range cases {
		if got := ParseRegister(c.in); got != c.want {
			t.Errorf("ParseRegister(%q) = %v, want %v", c.in, got, c.want)
		}
	}
}

// Truncated glyph prefixes and partial bracket tags must not misclassify or
// panic; all are in-progress. In C these could read past the buffer, which is
// why the C version used strncmp; in Go the risk is gone but the CASES are kept
// so the ported suite still covers the same inputs.
func TestRegisterShortInputsSafe(t *testing.T) {
	for _, in := range []string{
		"\xF0",     // lone 4-byte lead
		"\xF0\x9F", // partial 4-byte
		"\xE2",     // lone 3-byte lead
		"\xE2\x96", // partial 3-byte
		"[",        // bare bracket
		"[ver",     // partial tag
		" ",        // whitespace only
	} {
		if got := ParseRegister(in); got != RegInProgress {
			t.Errorf("ParseRegister(%q) = %v, want RegInProgress", in, got)
		}
	}
}
