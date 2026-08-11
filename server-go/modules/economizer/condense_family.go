package economizer

import (
	"fmt"
	"strings"
)

// Per-command family filters for tool-output condensation.
//
// Ported from tc_signal_filter / tc_family_test_runner / tc_family_diagnostics
// in src/modules/economizer/tool_condense.c.

// Failure and keep signals for the test-runner family.
var (
	tcFailSigs = []string{
		"fail", "error", "panic", "assert", "traceback", "exception", "not ok",
		"✗", "✖", "✘",
	}
	tcKeepSigs = []string{
		"test result", "result:", "====", "collected", "summary", "warning", "warnings",
	}
)

// Failure and keep signals for the compiler/linter diagnostics family.
var (
	tcDiagFailSigs = []string{
		"error", "undefined", "cannot", "expected", "fatal", "panicked",
		"no such", "not found", "not a type", "not a package", "imported and",
		"declared but not used", "multiple definition", "relocation", "unresolved",
		"syntaxerror", "typeerror",
	}
	tcDiagKeepSigs = []string{
		"warning", "warn:", "note:", "help:", "-->", "error[", "::error", "::warning", "====",
	}
)

// Context kept around each failure marker: a few lines BEFORE (go test puts the
// message above the marker) and a wider span AFTER (pytest/jest/rust/java
// tracebacks fall below it). A pathological traceback longer than the after
// window overflows it but survives in full in the spill.
const (
	tcTestCtxBefore = 3
	tcTestCtxAfter  = 8
)

// lineHasAny reports whether the line contains any signal, case-insensitively.
func lineHasAny(line string, sigs []string) bool {
	lower := asciiLower(line)
	for _, s := range sigs {
		if strings.Contains(lower, asciiLower(s)) {
			return true
		}
	}
	return false
}

// tcSignalFilter is the shared "keep the signal, drop the noise" line filter.
//
// Returns ok=false to mean VERBATIM PASSTHROUGH. That is the safety valve: when
// requireFailNonzero is set and the command failed but no recognised failure
// signal appears, the filter refuses to run at all — because the one thing worse
// than a long output is a condensed one that hid why the command failed.
func tcSignalFilter(exitCode int, in string, failSigs, keepSigs []string,
	requireFailNonzero bool, head, tail, ctxBefore, ctxAfter int) (string, bool) {

	lines := splitLines(in)
	n := len(lines)

	anyFail := false
	for _, l := range lines {
		if lineHasAny(l, failSigs) {
			anyFail = true
			break
		}
	}
	if requireFailNonzero && exitCode != 0 && !anyFail {
		return "", false // never hide the cause behind the filter
	}

	// Pre-pass: mark which lines to keep. A fail-signal line drags in its DETAIL
	// BLOCK — ctxBefore above and ctxAfter below — so a failure's message (often
	// a separate line from its marker) is never elided while the marker survives.
	keep := make([]bool, n)
	for i, l := range lines {
		if i < head || i+tail >= n || lineHasAny(l, keepSigs) {
			keep[i] = true
		}
		if lineHasAny(l, failSigs) {
			lo := i - ctxBefore
			if lo < 0 {
				lo = 0
			}
			hi := i + ctxAfter
			if hi > n-1 {
				hi = n - 1
			}
			for j := lo; j <= hi; j++ {
				keep[j] = true
			}
		}
	}

	var out []string
	elided := 0
	flush := func() {
		if elided > 0 {
			out = append(out, fmt.Sprintf("... %d lines elided ...", elided))
			elided = 0
		}
	}
	for i, l := range lines {
		if keep[i] {
			flush()
			out = append(out, l)
		} else {
			elided++
		}
	}
	flush()
	return strings.Join(out, "\n"), true
}

// TCFamilyTestRunner keeps the summary and every failure verbatim, dropping
// passing-case transcripts.
//
// ok=false means pass the raw output through unchanged: a non-zero exit with no
// recognisable failure line is never condensed, so an unfamiliar runner's
// failure cannot be hidden.
func TCFamilyTestRunner(exitCode int, in string) (string, bool) {
	return tcSignalFilter(exitCode, in, tcFailSigs, tcKeepSigs, true, 2, 6,
		tcTestCtxBefore, tcTestCtxAfter)
}

// TCFamilyDiagnostics keeps errors, warnings, notes and file:line diagnostics,
// dropping progress chatter.
//
// Context is 0/0: a compiler diagnostic is self-contained (file:line:col plus
// message), so the source echo and caret below it are redundant once the model
// has the location. A clean build with warnings is still condensed; a non-zero
// exit whose error format is unrecognised falls back to verbatim passthrough.
func TCFamilyDiagnostics(exitCode int, in string) (string, bool) {
	return tcSignalFilter(exitCode, in, tcDiagFailSigs, tcDiagKeepSigs, true, 2, 4, 0, 0)
}
