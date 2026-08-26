package db2

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func expireBackend() *fakeMemory {
	return &fakeMemory{lifecycle: fakeLifecycle{
		kindsByTier: map[string][]string{db2contract.ExpireStaleTier: {"fact", "task"}},
		expireDays:  map[string]uint32{"fact": 30, "task": 7},
		staleByKind: map[string]uint32{"fact": 4, "task": 5},
		l0Deleted:   3,
	}}
}

func TestExpireSweepSumsEveryKind(t *testing.T) {
	backend := expireBackend()
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeExpireRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	level0, stale, err := db2contract.DecodeExpireReply(reply)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if level0 != 3 {
		t.Errorf("level0 = %d, want 3", level0)
	}
	if stale != 9 {
		t.Errorf("stale = %d, want the 4+5 sum", stale)
	}
}

// Provenance goes first at each stage so no memory row outlives the record of
// where it came from.
func TestExpireSweepDeletesProvenanceFirst(t *testing.T) {
	backend := expireBackend()
	if _, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeExpireRequest()); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	calls := backend.lifecycle.calls
	assertOrder(t, calls, "l0-provenance", "l0")
	assertOrder(t, calls, "stale-provenance:fact", "stale:fact")
	assertOrder(t, calls, "stale-provenance:task", "stale:task")
}

// The expiry statements append the unit themselves, so the window they receive
// carries no unit. Feeding them the retention sweep's "-30 days" spelling would
// silently stop matching rows.
func TestExpireSweepPassesUnitlessWindow(t *testing.T) {
	backend := expireBackend()
	if _, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeExpireRequest()); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	for _, window := range backend.lifecycle.gotWindows {
		if strings.Contains(window, "days") {
			t.Errorf("window %q must not carry a unit", window)
		}
	}
	if backend.lifecycle.gotWindows[0] != "-30" {
		t.Errorf("window = %q, want -30", backend.lifecycle.gotWindows[0])
	}
}

// A kind with no expiry window would expire immediately. A missing policy is
// not licence to delete, so the whole sweep refuses.
func TestExpireSweepRefusesKindWithoutWindow(t *testing.T) {
	backend := expireBackend()
	backend.lifecycle.expireDays["task"] = 0
	_, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeExpireRequest())
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

func demoteBackend() *fakeMemory {
	return &fakeMemory{lifecycle: fakeLifecycle{
		kindsByTier:   map[string][]string{db2contract.DemoteTier: {"fact", "task"}},
		demoteDays:    map[string]uint32{"fact": 60, "task": 30},
		demoteConf:    map[string]float64{"fact": 0.7, "task": 0.5},
		demotedByKind: map[string]uint32{"fact": 2, "task": 1},
		cascaded:      6,
	}}
}

func TestDemoteSweepSumsAndCascades(t *testing.T) {
	backend := demoteBackend()
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeDemoteRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	demoted, cascaded, err := db2contract.DecodeDemoteReply(reply)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if demoted != 3 || cascaded != 6 {
		t.Fatalf("demoted = %d, cascaded = %d", demoted, cascaded)
	}
}

// One stamp covers the whole action so the cascade matches dependants of
// exactly the rows this call demoted.
func TestDemoteSweepUsesOneStampThroughout(t *testing.T) {
	backend := demoteBackend()
	if _, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeDemoteRequest()); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	stamps := backend.lifecycle.gotStamps
	if len(stamps) < 3 {
		t.Fatalf("expected a stamp per demotion plus the cascade, got %v", stamps)
	}
	for _, stamp := range stamps {
		if stamp != stamps[0] {
			t.Fatalf("stamps differ across the action: %v", stamps)
		}
	}
}

// Nothing demoted means nothing to cascade to, and the contract refuses a reply
// carrying cascaded rows with none demoted — so the call must be skipped, not
// merely ignored.
func TestDemoteSweepSkipsCascadeWhenNothingDemoted(t *testing.T) {
	backend := demoteBackend()
	backend.lifecycle.demotedByKind = map[string]uint32{}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeDemoteRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	for _, call := range backend.lifecycle.calls {
		if call == "cascade" {
			t.Fatal("the cascade must not run when nothing demoted")
		}
	}
	demoted, cascaded, err := db2contract.DecodeDemoteReply(reply)
	if err != nil || demoted != 0 || cascaded != 0 {
		t.Fatalf("demoted = %d, cascaded = %d, err = %v", demoted, cascaded, err)
	}
}

func TestPromoteStableStamps(t *testing.T) {
	backend := &fakeMemory{lifecycle: fakeLifecycle{promoted: 8}}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodePromoteStableRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(backend.lifecycle.gotStamps) != 1 || backend.lifecycle.gotStamps[0] == "" {
		t.Errorf("stamps = %v", backend.lifecycle.gotStamps)
	}
	promoted, err := db2contract.DecodePromoteStableReply(reply)
	if err != nil || promoted != 8 {
		t.Fatalf("promoted = %d, err = %v", promoted, err)
	}
}

func TestReclassifyDirectivesPassesApprovalFlag(t *testing.T) {
	for _, requireApproval := range []uint32{0, 1} {
		backend := &fakeMemory{lifecycle: fakeLifecycle{reclassified: 2}}
		request, err := db2contract.EncodeReclassifyDirectivesRequest(requireApproval)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		if _, status := NewMemoryHandler(backend)(memoryInvocation(), request); status != bus.ModuleStatusOK {
			t.Fatalf("status for %d", requireApproval)
		}
		if backend.lifecycle.gotRequireApprove != (requireApproval == 1) {
			t.Errorf("requireApproval %d reached the backend as %v",
				requireApproval, backend.lifecycle.gotRequireApprove)
		}
	}
}

func TestRecordL4Approval(t *testing.T) {
	backend := &fakeMemory{}
	request, err := db2contract.EncodeRecordL4ApprovalRequest(12, "jb", "looks right")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.lifecycle.gotApproval != [3]string{"12", "jb", "looks right"} {
		t.Errorf("approval = %v", backend.lifecycle.gotApproval)
	}
	if err := db2contract.DecodeRecordL4ApprovalReply(reply); err != nil {
		t.Fatalf("decode: %v", err)
	}
}

// The caller reports what its cycle did; the module measures the corpus itself.
// Trusting a caller's totals would let one cycle's bad arithmetic become the
// recorded history.
func TestHealthRecordMeasuresCorpusItself(t *testing.T) {
	backend := &fakeMemory{lifecycle: fakeLifecycle{memoriesCounted: 40, conflictsCount: 2}}
	request, err := db2contract.EncodeHealthRecordRequest(1, 2, 3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := NewMemoryHandler(backend)(memoryInvocation(), request); status != bus.ModuleStatusOK {
		t.Fatal("expected the record to succeed")
	}
	want := HealthSnapshot{
		TotalMemories: 40, ContradictionsDetected: 2,
		Promotions: 1, Demotions: 2, Expirations: 3,
	}
	if backend.lifecycle.gotSnapshot != want {
		t.Errorf("snapshot = %+v, want %+v", backend.lifecycle.gotSnapshot, want)
	}
	if backend.lifecycle.gotConflictDays != db2contract.HealthRecordConflictWindowDays {
		t.Errorf("conflict window = %d", backend.lifecycle.gotConflictDays)
	}
}

func TestHealthRetentionPrunesBoth(t *testing.T) {
	backend := &fakeMemory{lifecycle: fakeLifecycle{prunedHealth: 4, prunedConflicts: 5}}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeHealthRetentionRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	snapshots, contradictions, err := db2contract.DecodeHealthRetentionReply(reply)
	if err != nil || snapshots != 4 || contradictions != 5 {
		t.Fatalf("snapshots = %d, contradictions = %d, err = %v", snapshots, contradictions, err)
	}
}

func TestSweepFailurePropagates(t *testing.T) {
	backend := expireBackend()
	backend.err = errors.New("database is on fire")
	_, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeExpireRequest())
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

// --- window spellings ---

// The two window spellings are not interchangeable: either statement fed the
// other's spelling silently stops matching rows rather than failing.
func TestWindowSpellingsAreDistinct(t *testing.T) {
	if sweepWindow(30) != "-30" {
		t.Errorf("sweepWindow(30) = %q", sweepWindow(30))
	}
	if retentionWindow(30) != "-30 days" {
		t.Errorf("retentionWindow(30) = %q", retentionWindow(30))
	}
}

// --- kind lifecycle policy ---

// A kind the table does not describe still has to age, so a miss yields the
// defaults rather than a refusal to sweep.
func TestKindLifecycleDefaultsOnMiss(t *testing.T) {
	backend := &pgMemoryBackend{queryRow: func(context.Context, string, ...any) HealthRow {
		return fakeRow{err: errors.New("no such kind")}
	}}
	days, err := backend.KindExpireDays(context.Background(), "unknown")
	if err != nil || days != defaultExpireDays {
		t.Fatalf("days = %d, err = %v", days, err)
	}
	confidence, demoteDays, err := backend.KindDemotePolicy(context.Background(), "unknown")
	if err != nil || confidence != defaultDemoteConfidence || demoteDays != defaultDemoteDays {
		t.Fatalf("confidence = %v, days = %d, err = %v", confidence, demoteDays, err)
	}
}

// Resistance stretches the idle window a kind must sit through, and it is
// applied where the thresholds it scales live.
func TestKindDemotePolicyAppliesResistance(t *testing.T) {
	backend := &pgMemoryBackend{queryRow: func(context.Context, string, ...any) HealthRow {
		return lifecycleRow{demoteDays: 10, demoteConfidence: 0.4, resistance: 2.5}
	}}
	_, days, err := backend.KindDemotePolicy(context.Background(), "fact")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if days != 25 {
		t.Errorf("days = %d, want 25", days)
	}
}

// A zero or nonsense resistance would collapse the window to nothing. Neither
// is a policy, so the unscaled window is used.
func TestKindDemotePolicyIgnoresNonsenseResistance(t *testing.T) {
	for _, resistance := range []float64{0, -1} {
		backend := &pgMemoryBackend{queryRow: func(context.Context, string, ...any) HealthRow {
			return lifecycleRow{demoteDays: 10, resistance: resistance}
		}}
		_, days, err := backend.KindDemotePolicy(context.Background(), "fact")
		if err != nil || days != 10 {
			t.Errorf("resistance %v gave days = %d, err = %v", resistance, days, err)
		}
	}
}

// lifecycleRow answers the six-column kind_lifecycle row.
type lifecycleRow struct {
	demoteDays       int64
	demoteConfidence float64
	resistance       float64
}

func (r lifecycleRow) Scan(dest ...any) error {
	if len(dest) != 6 {
		return errors.New("scan arity mismatch")
	}
	*(dest[0].(*int64)) = defaultPromoteUseCount
	*(dest[1].(*float64)) = defaultPromoteConfid
	*(dest[2].(*int64)) = r.demoteDays
	*(dest[3].(*float64)) = r.demoteConfidence
	*(dest[4].(*int64)) = defaultExpireDays
	*(dest[5].(*float64)) = r.resistance
	return nil
}

// assertOrder fails unless first appears before second in calls.
func assertOrder(t *testing.T, calls []string, first, second string) {
	t.Helper()
	firstIndex, secondIndex := -1, -1
	for index, call := range calls {
		if call == first && firstIndex < 0 {
			firstIndex = index
		}
		if call == second && secondIndex < 0 {
			secondIndex = index
		}
	}
	if firstIndex < 0 || secondIndex < 0 {
		t.Fatalf("expected both %q and %q in %v", first, second, calls)
	}
	if firstIndex > secondIndex {
		t.Errorf("%q must precede %q, got %v", first, second, calls)
	}
}
