package economizer

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync/atomic"

	"golang.org/x/sys/unix"
)

// Spill store and the top-level condensation entry.
//
// Ported from the spill/apply/recall half of
// src/modules/economizer/tool_condense.c.

// TCSpillMaxBytes bounds the per-user spill store.
const TCSpillMaxBytes = 64 * 1024 * 1024

// TCStats is the per-condensation ledger record.
type TCStats struct {
	RawBytes   int
	FinalBytes int
	Recognized bool
	Spilled    bool
	Family     string // "test", "diag", or ""
	SpillRef   string
}

// TCTotals are the cumulative process-wide counters.
type TCTotals struct {
	Recognized     int64
	Applied        int64
	AppliedRaw     int64
	AppliedFinal   int64
	FamilyTest     int64
	FamilyDiag     int64
	Recovered      int64
	RecoveredBytes int64
	// SavedBytes is the gross condense saving.
	SavedBytes int64
	// NetSavedBytes is SavedBytes minus RecoveredBytes, and is MEANINGFULLY
	// negative when page-backs exceed the saving — i.e. the lever is net-loss on
	// this workload. Deliberately not clamped at zero: clamping would hide
	// exactly the signal this counter exists to expose.
	NetSavedBytes int64
}

var (
	tcRecognized     atomic.Int64
	tcApplied        atomic.Int64
	tcAppliedRaw     atomic.Int64
	tcAppliedFinal   atomic.Int64
	tcFamilyTest     atomic.Int64
	tcFamilyDiag     atomic.Int64
	tcRecovered      atomic.Int64
	tcRecoveredBytes atomic.Int64
)

// TCStatsSnapshot reads the counters.
//
// Each field is read atomically, but the set is NOT a transactional
// point-in-time view — a concurrent condensation may land between reads. Fine
// for monotonic metrics; do not assert cross-counter invariants on a live
// snapshot.
func TCStatsSnapshot() TCTotals {
	t := TCTotals{
		Recognized:     tcRecognized.Load(),
		Applied:        tcApplied.Load(),
		AppliedRaw:     tcAppliedRaw.Load(),
		AppliedFinal:   tcAppliedFinal.Load(),
		FamilyTest:     tcFamilyTest.Load(),
		FamilyDiag:     tcFamilyDiag.Load(),
		Recovered:      tcRecovered.Load(),
		RecoveredBytes: tcRecoveredBytes.Load(),
	}
	t.SavedBytes = t.AppliedRaw - t.AppliedFinal
	t.NetSavedBytes = t.SavedBytes - t.RecoveredBytes
	return t
}

// TCStatsReset zeroes the counters. TEST-ONLY: it races lost updates with live
// traffic, so call it only when no condensation is in flight.
func TCStatsReset() {
	for _, c := range []*atomic.Int64{
		&tcRecognized, &tcApplied, &tcAppliedRaw, &tcAppliedFinal,
		&tcFamilyTest, &tcFamilyDiag, &tcRecovered, &tcRecoveredBytes,
	} {
		c.Store(0)
	}
}

// tcHashRef is an opaque, deterministic spill ref: FNV-1a-64 over
// cmdline || NUL || raw, in hex.
//
// Deterministic rather than a counter, for two reasons: identical output dedups
// to one spill file, and a monotonic id would be ENUMERABLE — a caller could
// walk other conversations' refs.
func tcHashRef(seed, content string) string {
	var h uint64 = 1469598103934665603
	mix := func(s string) {
		for i := 0; i < len(s); i++ {
			h ^= uint64(s[i])
			h *= 1099511628211
		}
	}
	mix(seed)
	h ^= 0
	h *= 1099511628211
	mix(content)
	return fmt.Sprintf("tc-%016x", h)
}

// TCRefValid reports whether ref is exactly "tc-" plus 16 lowercase hex digits.
//
// Strict by design: the ref reaches the filesystem, so anything looser would let
// a recall traverse out of the spill directory.
func TCRefValid(ref string) bool {
	if !strings.HasPrefix(ref, "tc-") {
		return false
	}
	h := ref[3:]
	if len(h) != 16 {
		return false
	}
	for i := 0; i < len(h); i++ {
		c := h[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

// tcSpillEvict keeps the spill directory under budget by removing the OLDEST
// .out files by mtime. Best-effort: any error just leaves the file.
func tcSpillEvict(dir string, budget int64) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	type ent struct {
		path string
		mt   int64
		sz   int64
	}
	var files []ent
	var total int64
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".out") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		// Lstat + regular-file check so a symlink planted in the directory is
		// never followed for size or mtime.
		st, err := os.Lstat(p)
		if err != nil || !st.Mode().IsRegular() {
			continue
		}
		total += st.Size()
		files = append(files, ent{p, st.ModTime().UnixNano(), st.Size()})
	}
	if total <= budget {
		return
	}
	sort.Slice(files, func(a, b int) bool { return files[a].mt < files[b].mt })
	for _, f := range files {
		if total <= budget {
			return
		}
		if os.Remove(f.path) == nil {
			total -= f.sz
		}
	}
}

// tcSpillWrite writes the full raw output durably.
//
// Temp file, fsync, atomic rename — so a partial or crashed write is never
// promoted to a readable ref. The temp name carries the PID so two processes
// spilling the same ref cannot race on one temp path. Returns an error unless
// the durable rename landed; the caller then passes through, because a condensed
// body without a recoverable backstop would be lossy.
func tcSpillWrite(dir, ref, content string) error {
	path := filepath.Join(dir, ref+".out")
	tmp := filepath.Join(dir, fmt.Sprintf("%s.%d.tmp", ref, os.Getpid()))

	fd, err := unix.Open(tmp, unix.O_WRONLY|unix.O_CREAT|unix.O_TRUNC|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0o600)
	if err != nil {
		return err
	}
	f := os.NewFile(uintptr(fd), "economizer-spill-temp")
	if f == nil {
		_ = unix.Close(fd)
		return errors.New("spill temp unavailable")
	}
	if _, err := f.WriteString(content); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	// Best-effort directory fsync for crash-durability of the dir entry; it does
	// not gate success, because the rename already made the content readable for
	// the recall that matters this run.
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}

var tcTestRunners = map[string]bool{
	"pytest": true, "jest": true, "vitest": true, "mocha": true, "ctest": true,
}

var tcTestSubRunners = map[string]bool{
	"cargo": true, "go": true, "npm": true, "yarn": true, "pnpm": true,
	"mvn": true, "gradle": true, "dotnet": true,
}

var tcDiagCmds = map[string]bool{
	"tsc": true, "eslint": true, "ruff": true, "mypy": true, "flake8": true,
	"gcc": true, "g++": true, "cc": true, "clang": true, "clang++": true,
	"rustc": true, "cmake": true,
}

func tcIsTestInvocation(r TCRecoResult) bool {
	if tcTestRunners[r.Cmd] {
		return true
	}
	return tcTestSubRunners[r.Cmd] && r.Sub == "test"
}

func tcIsDiagnosticsInvocation(r TCRecoResult) bool {
	if tcDiagCmds[r.Cmd] {
		return true
	}
	switch r.Cmd {
	case "cargo":
		return r.Sub == "build" || r.Sub == "check" || r.Sub == "clippy"
	case "go":
		return r.Sub == "build" || r.Sub == "vet"
	case "dotnet":
		return r.Sub == "build"
	case "npm", "yarn", "pnpm":
		return r.Sub == "build" || r.Sub == "lint"
	}
	return false
}

// ErrTCPassthrough signals that the caller should use the raw output unchanged.
var ErrTCPassthrough = errors.New("economizer: condensation declined (passthrough)")

// TCApply condenses tool output for a recognized command, spilling the full raw
// output first.
//
// Returns ErrTCPassthrough whenever the raw output should be used unchanged —
// which is every ambiguous case. The invariant is LOSSLESS-ON-DEMAND: a
// condensed body only ships if the full output was durably spilled and the
// recovery pointer fits, so nothing is ever dropped without a way back.
func TCApply(cmdline string, exitCode int, raw, spillDir string) (string, TCStats, error) {
	stats := TCStats{RawBytes: len(raw), FinalBytes: len(raw)}
	if raw == "" {
		return "", stats, ErrTCPassthrough
	}
	if len(raw) > TCCeiling {
		// Over the input cap: hand back to the size-based fallback.
		return "", stats, ErrTCPassthrough
	}

	reco := TCRecognize(cmdline)
	stats.Recognized = reco.Outcome == TCRecognized
	if reco.Outcome != TCRecognized {
		return "", stats, ErrTCPassthrough
	}
	tcRecognized.Add(1)

	var cond string
	var ok bool
	family := ""
	switch {
	case tcIsTestInvocation(reco):
		cond, ok = TCFamilyTestRunner(exitCode, raw)
		family = "test"
	case tcIsDiagnosticsInvocation(reco):
		cond, ok = TCFamilyDiagnostics(exitCode, raw)
		family = "diag"
	default:
		return "", stats, ErrTCPassthrough
	}
	if !ok {
		return "", stats, ErrTCPassthrough // the filter declined
	}

	// Material-gain gate: require a real shrink, else the spill round-trip costs
	// more than it saves.
	rawLen, condLen := len(raw), len(cond)
	if rawLen-condLen < 200 || condLen*100 > rawLen*85 {
		return "", stats, ErrTCPassthrough
	}

	// Lossless-on-demand: no spill directory means no backstop, so no condense.
	if spillDir == "" {
		return "", stats, ErrTCPassthrough
	}
	ref := tcHashRef(cmdline, raw)
	tcSpillEvict(spillDir, TCSpillMaxBytes)
	if err := tcSpillWrite(spillDir, ref, raw); err != nil {
		return "", stats, ErrTCPassthrough
	}

	final := cond + fmt.Sprintf(
		"\n[output condensed by aimee — %d bytes total; retrieve the full, "+
			"unfiltered output with the tool_output_get tool, ref %q, if you need "+
			"a passing case or elided detail]", rawLen, ref)

	stats.FinalBytes = len(final)
	stats.Spilled = true
	stats.Family = family
	stats.SpillRef = ref

	tcApplied.Add(1)
	tcAppliedRaw.Add(int64(rawLen))
	tcAppliedFinal.Add(int64(len(final)))
	switch family {
	case "test":
		tcFamilyTest.Add(1)
	case "diag":
		tcFamilyDiag.Add(1)
	}
	return final, stats, nil
}

// TCRecall resolves a spill ref to its full content — the first-class handle
// that makes lossless-on-demand real.
func TCRecall(spillDir, ref string) (string, error) {
	if spillDir == "" || !TCRefValid(ref) {
		return "", errors.New("invalid ref")
	}
	// Match the retired C reader's O_NOFOLLOW boundary. A spill directory can be
	// writable by the server user; following a planted symlink would turn the
	// recall handle into an arbitrary-file reader despite the strict ref grammar.
	fd, err := unix.Open(filepath.Join(spillDir, ref+".out"), unix.O_RDONLY|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0)
	if err != nil {
		return "", errors.New("spill expired")
	}
	f := os.NewFile(uintptr(fd), "economizer-spill")
	if f == nil {
		_ = unix.Close(fd)
		return "", errors.New("spill expired")
	}
	defer f.Close()

	// Bounded read with one sentinel byte. Refuse an oversized legacy spill
	// rather than returning a successful but silently truncated recovery.
	data, err := io.ReadAll(io.LimitReader(f, TCCeiling+1))
	if err != nil || len(data) > TCCeiling {
		return "", errors.New("spill expired")
	}
	out := string(data)

	// Recovery-cost telemetry: a successful recall is a page-back, so count it
	// and the bytes re-injected. Without this the ledger would show only the
	// saving and never its cost.
	tcRecovered.Add(1)
	tcRecoveredBytes.Add(int64(len(data)))
	return out, nil
}
