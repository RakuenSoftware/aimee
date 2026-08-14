// Package economizer holds the context economizer: the levers that shrink an
// assembled prompt before it goes to a provider, and the accounting that says
// what that saved.
//
// Ported from the retired C implementation. Its tests were carried over
// one-for-one so the Go owner stays pinned to the behaviour it replaced.
package economizer

import "strings"

// Register is the trust/register grammar for an assistant turn (fold §6).
//
// The register says how much weight a folded line carries: a verdict is a
// settled claim, an in-progress note is not. Folding keeps settled turns and
// thins transient ones, so a misread here changes what survives a fold.
type Register int

const (
	// RegInProgress is the default for anything unmarked. Defaulting to the
	// WEAKEST register matters: an unmarked turn must never be mistaken for a
	// settled conclusion.
	RegInProgress Register = iota
	RegExecuting
	RegVerdict
	RegHazard
	RegBlocked
)

// Label is the short name used in a folded skeleton line.
func (r Register) Label() string {
	switch r {
	case RegExecuting:
		return "exec"
	case RegVerdict:
		return "verdict"
	case RegHazard:
		return "hazard"
	case RegBlocked:
		return "blocked"
	default:
		return "wip"
	}
}

// IsSettled reports whether the register represents a CONCLUSION rather than
// work in flight. Only settled turns are safe to keep as assertions when the
// surrounding detail is folded away.
func (r Register) IsSettled() bool {
	return r == RegVerdict || r == RegHazard
}

// bracketTag reports whether s begins with tag (ASCII case-insensitive)
// immediately followed by ']'.
//
// The closing bracket is required so only exact tags match: "[exec]" is a
// register, "[executable]" is prose that happens to share a prefix.
func bracketTag(s, tag string) bool {
	if len(s) < len(tag)+1 {
		return false
	}
	if !strings.EqualFold(s[:len(tag)], tag) {
		return false
	}
	return s[len(tag)] == ']'
}

// ParseRegister classifies an assistant turn by its leading marker.
//
// Deterministic, and biased toward RegInProgress: anything unrecognised is
// treated as unsettled work rather than promoted to a claim.
func ParseRegister(text string) Register {
	s := strings.TrimLeft(text, " \t\n\r")

	// Leading glyphs, matched as UTF-8 byte sequences. Prefix matching on a
	// string is inherently bounds-safe here, so the truncated-glyph cases the C
	// version guarded with strncmp fall out for free.
	switch {
	case strings.HasPrefix(s, "\U0001F3C1"): // 🏁
		return RegVerdict
	case strings.HasPrefix(s, "\U0001F50D"): // 🔍
		return RegInProgress
	case strings.HasPrefix(s, "▶"): // ▶
		return RegExecuting
	case strings.HasPrefix(s, "⚠"): // ⚠
		return RegHazard
	case strings.HasPrefix(s, "❓"): // ❓
		return RegBlocked
	}

	if strings.HasPrefix(s, "[") {
		t := s[1:]
		switch {
		case bracketTag(t, "verdict"), bracketTag(t, "done"):
			return RegVerdict
		case bracketTag(t, "hazard"), bracketTag(t, "warning"):
			return RegHazard
		case bracketTag(t, "executing"), bracketTag(t, "exec"):
			return RegExecuting
		case bracketTag(t, "blocked"):
			return RegBlocked
		case bracketTag(t, "in-progress"), bracketTag(t, "wip"):
			return RegInProgress
		}
	}
	return RegInProgress
}
