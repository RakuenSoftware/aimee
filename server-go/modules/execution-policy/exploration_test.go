package executionpolicy

import (
	"encoding/json"
	"errors"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func ceiling(n int64) *int64 { return &n }
func testContract(now time.Time) explorationContract {
	return explorationContract{ID: "host-issued", Revision: 1, Binding: explorationBinding{Principal: "alice", Session: "session", Task: "task", Project: "project", WorktreeGeneration: "worktree-v1", IndexGeneration: "index-v1", MemoryOwner: "owner-1"}, PlanDigest: "plan", SourceVersionsDigest: "sources", QueryClass: "symbol", CoverageComplete: true, ConfidenceProvenance: "held-out", CalibrationReceipt: "measurement", SupportedClasses: []string{"raw_scan", "file_read", "graph_expand"}, Created: now.Add(-time.Second), Expires: now.Add(time.Hour), Limits: explorationLimits{Enabled: true, RawScans: ceiling(2)}, Tier: "enforce"}
}
func testLedger(t *testing.T, c explorationContract) *explorationLedger {
	t.Helper()
	l := newExplorationLedger(explorationSnapshot{}, func(explorationSnapshot) error { return nil })
	if err := l.issue(c, time.Now()); err != nil {
		t.Fatal(err)
	}
	return l
}
func measured(q, r string) bool { return q == "symbol" && r == "measurement" }
func attempt(c explorationContract, id string) explorationAttempt {
	return explorationAttempt{ID: id, Binding: c.Binding, Class: "raw_scan", Path: c.Binding.WorkingDirectory}
}

func TestExplorationDoesNotRestrictUnobservedOrOtherProjectPaths(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Binding.WorkingDirectory = "/project"
	c.Limits.RawScans = ceiling(0)
	for _, path := range []string{"", "/other-project", "/project-other"} {
		l := testLedger(t, c)
		a := attempt(c, "first")
		a.Path = path
		d, err := l.reserve(a, explorationLimits{RawScans: ceiling(1)}, true, measured, now)
		if err != nil || d.Restricted || d.WouldRestrict || d.Reason != "adaptive_scope_unavailable" {
			t.Fatal(path, d, err)
		}
		a.ID = "second"
		d, err = l.reserve(a, explorationLimits{RawScans: ceiling(1)}, true, measured, now)
		if err != nil || !d.Restricted || d.Reason != "operator_exploration_budget_exhausted" {
			t.Fatal("scope invalidation reset operator ceiling", path, d, err)
		}
	}
}
func TestExplorationLiteralZeroAndLegacyAdapter(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.RawScans = ceiling(0)
	l := testLedger(t, c)
	d, err := l.reserve(attempt(c, "zero"), explorationLimits{RawScans: legacyRawScanLimit(0)}, true, measured, now)
	if err != nil || !d.Restricted {
		t.Fatalf("literal zero: %+v %v", d, err)
	}
	for _, n := range []int64{-1, 0} {
		if legacyRawScanLimit(n) != nil {
			t.Fatal("legacy disabled zero became literal zero")
		}
	}
	c.Limits.Enabled = false
	c.Revision++
	if err := l.issue(c, now); err != nil {
		t.Fatal(err)
	}
	d, err = l.reserve(attempt(c, "disabled"), explorationLimits{}, true, measured, now)
	if err != nil || d.Restricted {
		t.Fatalf("disabled: %+v %v", d, err)
	}
}
func TestExplorationConcurrentRetriesAndRestart(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	var durable []byte
	l := newExplorationLedger(explorationSnapshot{}, func(s explorationSnapshot) error { var err error; durable, err = json.Marshal(s); return err })
	if err := l.issue(c, now); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	var admitted atomic.Int64
	for i := 0; i < 64; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			d, err := l.reserve(attempt(c, fmt.Sprint(i)), explorationLimits{}, true, measured, now)
			if err != nil {
				t.Error(err)
			}
			if !d.Restricted {
				admitted.Add(1)
			}
		}(i)
	}
	wg.Wait()
	if admitted.Load() != 2 || l.state.TaskUsage.RawScans != 2 {
		t.Fatalf("overspent: %d %+v", admitted.Load(), l.state.TaskUsage)
	}
	var restored explorationSnapshot
	if err := json.Unmarshal(durable, &restored); err != nil {
		t.Fatal(err)
	}
	reopened := newExplorationLedger(restored, func(explorationSnapshot) error { return nil })
	for id, r := range restored.Attempts {
		d, err := reopened.reserve(attempt(c, id), explorationLimits{}, true, measured, now)
		if err != nil || d.Restricted != r.Decision.Restricted {
			t.Fatal("retry changed decision", err)
		}
	}
	if reopened.state.TaskUsage.RawScans != 2 {
		t.Fatal("restart/retry double charged")
	}
	c.Revision++
	if err := reopened.issue(c, now); err != nil {
		t.Fatal(err)
	}
	d, err := reopened.reserve(attempt(c, "after-revision"), explorationLimits{}, true, measured, now)
	if err != nil || !d.Restricted {
		t.Fatal("revision reset budget", err)
	}
}
func TestExplorationInvalidationAndCalibration(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.RawScans = ceiling(0)
	for _, field := range []string{"principal", "session", "task", "project", "worktree", "index", "owner", "expiry", "calibration", "optin", "coverage"} {
		t.Run(field, func(t *testing.T) {
			current := c
			optin := true
			check := measured
			if field == "coverage" {
				current.CoverageComplete = false
			}
			l := testLedger(t, current)
			a := attempt(current, field)
			at := now
			switch field {
			case "principal":
				a.Binding.Principal = "other"
			case "session":
				a.Binding.Session = "other"
			case "task":
				a.Binding.Task = "other"
			case "project":
				a.Binding.Project = "other"
			case "worktree":
				a.Binding.WorktreeGeneration = "other"
			case "index":
				a.Binding.IndexGeneration = "other"
			case "owner":
				a.Binding.MemoryOwner = "other"
			case "expiry":
				at = now.Add(2 * time.Hour)
			case "calibration":
				check = nil
			case "optin":
				optin = false
			}
			d, err := l.reserve(a, explorationLimits{}, optin, check, at)
			if err != nil || d.Restricted || d.Mode != "observe" {
				t.Fatalf("%+v %v", d, err)
			}
		})
	}
	bad := c
	bad.Limits.RawScans = ceiling(-1)
	l := newExplorationLedger(explorationSnapshot{}, nil)
	if l.issue(bad, now) == nil {
		t.Fatal("negative ceiling accepted")
	}
}
func TestExplorationRefundAndDistinctFiles(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.Files = ceiling(1)
	c.Limits.Bytes = ceiling(12)
	l := testLedger(t, c)
	a := attempt(c, "read1")
	a.Class = "file_read"
	a.File = "canonical:a"
	a.Bytes = 5
	if d, e := l.reserve(a, explorationLimits{}, true, measured, now); e != nil || d.Restricted {
		t.Fatal(d, e)
	}
	b := a
	b.ID = "read2"
	if d, e := l.reserve(b, explorationLimits{}, true, measured, now); e != nil || d.Restricted {
		t.Fatal(d, e)
	}
	if len(l.state.TaskUsage.Files) != 1 || l.state.TaskUsage.Bytes != 10 {
		t.Fatal("reread accounting")
	}
	b.ID = "read3"
	if d, e := l.reserve(b, explorationLimits{}, true, measured, now); e != nil || !d.Restricted {
		t.Fatal("byte cap", d, e)
	}
	if err := l.cancelBeforeDispatch("read1"); err != nil {
		t.Fatal(err)
	}
	if len(l.state.TaskUsage.Files) != 1 || l.state.TaskUsage.Bytes != 5 {
		t.Fatal("refund removed another read")
	}
	if err := l.started("read2"); err != nil {
		t.Fatal(err)
	}
	if l.cancelBeforeDispatch("read2") == nil {
		t.Fatal("refunded dispatch")
	}
	if _, err := l.reserve(a, explorationLimits{}, true, measured, now); err == nil {
		t.Fatal("reused cancelled attempt")
	}
}
func TestExplorationStorageFailureAndIdentityMutation(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	l := testLedger(t, c)
	l.commit = func(explorationSnapshot) error { return errors.New("disk failed") }
	if _, err := l.reserve(attempt(c, "first"), explorationLimits{}, true, measured, now); err == nil {
		t.Fatal("lost durable reservation")
	}
	if l.state.TaskUsage.RawScans != 0 || len(l.state.Attempts) != 0 {
		t.Fatal("failed commit changed memory")
	}
	l.commit = func(explorationSnapshot) error { return nil }
	if _, err := l.reserve(attempt(c, "first"), explorationLimits{}, true, measured, now); err != nil {
		t.Fatal(err)
	}
	a := attempt(c, "first")
	a.Bytes = 1
	if _, err := l.reserve(a, explorationLimits{}, true, measured, now); err == nil {
		t.Fatal("mutated attempt accepted")
	}
	*c.Limits.RawScans = 0
	if *l.state.Revisions[0].Limits.RawScans != 2 {
		t.Fatal("caller mutated issued limit")
	}
}
func TestExplorationStarvationPreservesOperatorAndConfidence(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.RawScans = ceiling(0)
	l := testLedger(t, c)
	for _, turn := range []uint64{1, 1} {
		if err := l.completedTurn(turn, true, true, true, false); err != nil {
			t.Fatal(err)
		}
	}
	if len(l.state.Revisions) != 1 {
		t.Fatal("retry counted as a new turn")
	}
	if err := l.completedTurn(2, true, true, true, false); err != nil {
		t.Fatal(err)
	}
	if len(l.state.Revisions) != 2 || l.state.Revisions[0].Tier != "enforce" || l.state.Revisions[1].Tier != "observe" || l.state.Revisions[1].ConfidenceProvenance != c.ConfidenceProvenance {
		t.Fatal("invalid relaxation history")
	}
	if d, e := l.reserve(attempt(c, "recovery"), explorationLimits{}, true, measured, now); e != nil || d.Restricted {
		t.Fatal("starvation deadlock", d, e)
	}
	if d, e := l.reserve(attempt(c, "operator-denied"), explorationLimits{RawScans: ceiling(1)}, true, measured, now); e != nil || !d.Restricted || d.Reason != "operator_exploration_budget_exhausted" {
		t.Fatal("relaxation widened operator policy", d, e)
	}
}

func TestExplorationInvalidContractStillChargesOperator(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	for _, kind := range []string{"missing", "expired", "wrong-project", "unsupported-class"} {
		t.Run(kind, func(t *testing.T) {
			l := testLedger(t, c)
			at := now
			a := attempt(c, "first")
			switch kind {
			case "missing":
				l.state.Revisions = nil
			case "expired":
				at = now.Add(2 * time.Hour)
			case "wrong-project":
				l.state.Revisions[0].Binding.Project = "old-project"
			case "unsupported-class":
				l.state.Revisions[0].SupportedClasses = []string{"graph_expand"}
			}
			operator := explorationLimits{RawScans: ceiling(1)}
			d, e := l.reserve(a, operator, true, measured, at)
			if e != nil || d.Restricted || d.Mode != "observe" {
				t.Fatal(d, e)
			}
			a.ID = "second"
			d, e = l.reserve(a, operator, true, measured, at)
			if e != nil || !d.Restricted || d.Reason != "operator_exploration_budget_exhausted" {
				t.Fatal("invalid adaptive state widened baseline", d, e)
			}
		})
	}
}
func TestToolJSONCannotIssueOrBypassContract(t *testing.T) {
	h := newHandler(func() (*operatorPolicy, error) { return &operatorPolicy{ForbiddenCommands: []string{"secret"}}, nil })
	req := base("bash", map[string]any{"command": "rg secret", "bypass": true, "contract": map[string]any{"tier": "observe"}})
	if invoke(t, h, req).Allowed {
		t.Fatal("tool arguments bypassed policy")
	}
	req["contract"] = map[string]any{"tier": "enforce"}
	body, _ := json.Marshal(req)
	if _, status := h(bus.ModuleInvocation{StageID: StageTool}, body); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("tool JSON issued a contract", status)
	}
}
