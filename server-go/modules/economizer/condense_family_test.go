package economizer

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// DIFFERENTIAL tests for the family filters and the two line models. The
// expectations were captured from the retired C implementation before removal.

// THE TWO LINE MODELS. tc_strip_noise stops at end-of-string; the dedup /
// truncate / signal-filter family counts the empty segment after a trailing
// newline as a line. Real tool output almost always ends with a newline, so
// getting this wrong shifts every elided count and makes tail=N keep an empty
// last line instead of the last real one.
func TestTCLineModelsDifferOnTrailingNewline(t *testing.T) {
	// C: tc_truncate_with_signal("1\n2\n3\n4\n", 1, 1, NULL) -> "1\n... 3 lines elided ...\n"
	if got := TCTruncateWithSignal("1\n2\n3\n4\n", 1, 1, ""); got != "1\n... 3 lines elided ...\n" {
		t.Errorf("truncate with trailing newline:\n got: %q\nwant: %q",
			got, "1\n... 3 lines elided ...\n")
	}
	// C: without the trailing newline -> "1\n... 2 lines elided ...\n4"
	if got := TCTruncateWithSignal("1\n2\n3\n4", 1, 1, ""); got != "1\n... 2 lines elided ...\n4" {
		t.Errorf("truncate without trailing newline:\n got: %q", got)
	}
	// C: tc_dedup_lines("a\na\n") -> "a  (x2)\n"
	if got := TCDedupLines("a\na\n"); got != "a  (x2)\n" {
		t.Errorf("dedup with trailing newline: got %q, want %q", got, "a  (x2)\n")
	}
	// C: tc_strip_noise("a\nb\n") -> "a\nb"  (the OTHER model)
	if got := TCStripNoise("a\nb\n"); got != "a\nb" {
		t.Errorf("strip_noise with trailing newline: got %q, want %q", got, "a\nb")
	}
}

const goFailTranscript = "=== RUN   TestA\n--- PASS: TestA (0.00s)\n=== RUN   TestB\n" +
	"noise line 1\nnoise line 2\nnoise line 3\nnoise line 4\n" +
	"    x_test.go:63: expected 5 got 4\n--- FAIL: TestB (0.01s)\n" +
	"after1\nafter2\nafter3\n=== RUN   TestC\n--- PASS: TestC (0.00s)\n" +
	"filler1\nfiller2\nfiller3\nfiller4\nfiller5\nFAIL\nexit status 1\nFAIL\tpkg/x\t0.02s\n"

const cleanTranscript = "ok  \tpkg/a\t0.01s\nok  \tpkg/b\t0.01s\nok  \tpkg/c\t0.01s\n" +
	"ok  \tpkg/d\t0.01s\nok  \tpkg/e\t0.01s\nok  \tpkg/f\t0.01s\n"

// The failure's MESSAGE sits above its marker in go test output, so the
// before-context is what keeps "x_test.go:63: expected 5 got 4" alive while
// "--- FAIL:" survives. Losing it would leave a marker with no explanation.
func TestTCFamilyTestRunnerMatchesC(t *testing.T) {
	want := "=== RUN   TestA\n--- PASS: TestA (0.00s)\n... 3 lines elided ...\n" +
		"noise line 3\nnoise line 4\n    x_test.go:63: expected 5 got 4\n" +
		"--- FAIL: TestB (0.01s)\nafter1\nafter2\nafter3\n=== RUN   TestC\n" +
		"--- PASS: TestC (0.00s)\nfiller1\nfiller2\nfiller3\nfiller4\nfiller5\n" +
		"FAIL\nexit status 1\nFAIL\tpkg/x\t0.02s\n"
	got, ok := TCFamilyTestRunner(1, goFailTranscript)
	if !ok {
		t.Fatal("filter declined on a transcript with failures")
	}
	if got != want {
		t.Errorf("\n got: %q\nwant: %q", got, want)
	}
}

// THE SAFETY VALVE: a non-zero exit with NO recognisable failure line is passed
// through verbatim. The one thing worse than long output is condensed output
// that hid why the command failed.
func TestTCFamilyTestRunnerPassesThroughUnexplainedFailure(t *testing.T) {
	if _, ok := TCFamilyTestRunner(1, cleanTranscript); ok {
		t.Error("a non-zero exit with no failure signal must pass through verbatim")
	}
	// The same transcript on a ZERO exit is condensed normally.
	got, ok := TCFamilyTestRunner(0, cleanTranscript)
	if !ok {
		t.Fatal("a clean exit should still condense")
	}
	if got != cleanTranscript {
		t.Errorf("short clean output should survive intact:\n got: %q", got)
	}
}

const diagTranscript = "Compiling foo v0.1.0\nCompiling bar v0.2.0\nDownloading crates\n" +
	"Checking foo\nsrc/main.rs:10:5: error[E0308]: mismatched types\n" +
	"   --> src/main.rs:10:5\nhelp: try this\nCompiling baz\nCompiling qux\n" +
	"Compiling quux\nwarning: unused variable `x`\nFinished dev target\n"

func TestTCFamilyDiagnosticsMatchesC(t *testing.T) {
	want := "Compiling foo v0.1.0\nCompiling bar v0.2.0\n... 2 lines elided ...\n" +
		"src/main.rs:10:5: error[E0308]: mismatched types\n   --> src/main.rs:10:5\n" +
		"help: try this\n... 2 lines elided ...\nCompiling quux\n" +
		"warning: unused variable `x`\nFinished dev target\n"
	got, ok := TCFamilyDiagnostics(0, diagTranscript)
	if !ok {
		t.Fatal("diagnostics filter declined")
	}
	if got != want {
		t.Errorf("\n got: %q\nwant: %q", got, want)
	}
}

// ---- apply / spill / recall ----

func TestTCApplyRoundTripAndRecall(t *testing.T) {
	TCStatsReset()
	dir := t.TempDir()

	// A transcript long enough to clear the material-gain gate.
	var b strings.Builder
	b.WriteString("=== RUN   TestX\n")
	for i := 0; i < 200; i++ {
		b.WriteString("--- PASS: TestFiller (0.00s)\n")
	}
	b.WriteString("    x_test.go:9: expected 1 got 2\n--- FAIL: TestX (0.01s)\nFAIL\n")
	raw := b.String()

	final, stats, err := TCApply("go test ./...", 1, raw, dir)
	if err != nil {
		t.Fatalf("apply declined: %v", err)
	}
	if !stats.Recognized || !stats.Spilled || stats.Family != "test" {
		t.Fatalf("stats wrong: %+v", stats)
	}
	if stats.FinalBytes >= stats.RawBytes {
		t.Errorf("no material gain: %d -> %d", stats.RawBytes, stats.FinalBytes)
	}
	// The failure and its message survive; the pointer is present.
	for _, want := range []string{"--- FAIL: TestX", "x_test.go:9", "tool_output_get", stats.SpillRef} {
		if !strings.Contains(final, want) {
			t.Errorf("condensed body is missing %q", want)
		}
	}

	// LOSSLESS-ON-DEMAND: the full raw output comes back byte-for-byte.
	got, err := TCRecall(dir, stats.SpillRef)
	if err != nil {
		t.Fatalf("recall: %v", err)
	}
	if got != raw {
		t.Error("recall did not return the original bytes")
	}

	tot := TCStatsSnapshot()
	if tot.Applied != 1 || tot.FamilyTest != 1 || tot.Recovered != 1 {
		t.Errorf("counters wrong: %+v", tot)
	}
	if tot.SavedBytes != tot.AppliedRaw-tot.AppliedFinal {
		t.Error("SavedBytes is not the gross saving")
	}
	if tot.NetSavedBytes != tot.SavedBytes-tot.RecoveredBytes {
		t.Error("NetSavedBytes must subtract the page-back cost")
	}
}

// No spill directory means no recoverable backstop, so nothing is condensed —
// the lossless invariant, not an optimisation.
func TestTCApplyRefusesWithoutSpillDir(t *testing.T) {
	raw := strings.Repeat("--- PASS: T (0.00s)\n", 200) + "--- FAIL: X\nFAIL\n"
	if _, _, err := TCApply("go test ./...", 1, raw, ""); err == nil {
		t.Error("condensing without a spill directory must be refused")
	}
}

// An unrecognized or opaque command is never condensed.
func TestTCApplyPassthroughForUnrecognized(t *testing.T) {
	raw := strings.Repeat("noise\n", 500)
	for _, cmd := range []string{"unknowncmd", "make -j8", "./git test", "git status | grep x"} {
		if _, _, err := TCApply(cmd, 0, raw, t.TempDir()); err == nil {
			t.Errorf("%q should not have been condensed", cmd)
		}
	}
}

// The material-gain gate: a body that barely shrinks is not worth a spill
// round-trip.
func TestTCApplyMaterialGainGate(t *testing.T) {
	raw := "--- FAIL: X (0.01s)\nFAIL\n"
	if _, _, err := TCApply("go test ./...", 1, raw, t.TempDir()); err == nil {
		t.Error("a tiny transcript should not be condensed")
	}
}

func TestTCRefValidRejectsPaths(t *testing.T) {
	for _, bad := range []string{
		"", "tc-", "tc-xyz", "tc-0123456789abcde", "tc-0123456789abcdef0",
		"tc-../../etc/passwd", "../tc-0123456789abcdef", "tc-0123456789ABCDEF",
	} {
		if TCRefValid(bad) {
			t.Errorf("TCRefValid(%q) must be false", bad)
		}
	}
	if !TCRefValid("tc-0123456789abcdef") {
		t.Error("a well-formed ref must validate")
	}
}

func TestTCRecallRejectsBadRef(t *testing.T) {
	dir := t.TempDir()
	if _, err := TCRecall(dir, "tc-../../etc/passwd"); err == nil {
		t.Error("a traversal ref must be refused")
	}
	if _, err := TCRecall(dir, "tc-0123456789abcdef"); err == nil {
		t.Error("a missing spill must report expiry")
	}
	if _, err := TCRecall("", "tc-0123456789abcdef"); err == nil {
		t.Error("an empty spill dir must be refused")
	}
}

func TestTCRecallRefusesSymlink(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "secret")
	if err := os.WriteFile(target, []byte("must not escape"), 0o600); err != nil {
		t.Fatal(err)
	}
	ref := "tc-0123456789abcdef"
	if err := os.Symlink(target, filepath.Join(dir, ref+".out")); err != nil {
		t.Fatal(err)
	}
	if got, err := TCRecall(dir, ref); err == nil || got != "" {
		t.Fatalf("symlink recall = %q, %v", got, err)
	}
}

func TestTCSpillWriteRefusesTempSymlink(t *testing.T) {
	dir := t.TempDir()
	ref := "tc-0123456789abcdef"
	target := filepath.Join(dir, "target")
	if err := os.WriteFile(target, []byte("unchanged"), 0o600); err != nil {
		t.Fatal(err)
	}
	tmp := filepath.Join(dir, ref+"."+strconv.Itoa(os.Getpid())+".tmp")
	if err := os.Symlink(target, tmp); err != nil {
		t.Fatal(err)
	}
	if err := tcSpillWrite(dir, ref, "replacement"); err == nil {
		t.Fatal("writer followed a planted temp symlink")
	}
	got, err := os.ReadFile(target)
	if err != nil || string(got) != "unchanged" {
		t.Fatalf("target changed: %q, %v", got, err)
	}
}

func TestTCRecallRefusesOversizedSpill(t *testing.T) {
	dir := t.TempDir()
	ref := "tc-0123456789abcdef"
	if err := os.WriteFile(filepath.Join(dir, ref+".out"), make([]byte, TCCeiling+1), 0o600); err != nil {
		t.Fatal(err)
	}
	if got, err := TCRecall(dir, ref); err == nil || got != "" {
		t.Fatalf("oversized recall = %d bytes, %v", len(got), err)
	}
}

// The spill store stays bounded: the OLDEST files go first.
func TestTCSpillEvict(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"tc-aaaaaaaaaaaaaaa1.out", "tc-aaaaaaaaaaaaaaa2.out"} {
		if err := os.WriteFile(dir+"/"+n, make([]byte, 1024), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	tcSpillEvict(dir, 512) // budget below the total forces eviction
	entries, _ := os.ReadDir(dir)
	if len(entries) >= 2 {
		t.Errorf("eviction did not bring the store under budget: %d files left", len(entries))
	}
}
