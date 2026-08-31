package db2

import "context"

// fakeLifecycle records the sweep primitives, so a test can assert the order
// and arguments the orchestration used rather than only its final counts.
type fakeLifecycle struct {
	kindsByTier map[string][]string
	expireDays  map[string]uint32
	demoteDays  map[string]uint32
	demoteConf  map[string]float64

	l0Provenance    uint32
	l0Deleted       uint32
	staleByKind     map[string]uint32
	demotedByKind   map[string]uint32
	cascaded        uint32
	promoted        uint32
	reclassified    uint32
	memoriesCounted uint32
	conflictsCount  uint32
	prunedHealth    uint32
	prunedConflicts uint32

	// Recorded calls, in order, so provenance-before-rows is checkable.
	calls []string

	gotStamps         []string
	gotWindows        []string
	gotRequireApprove bool
	gotSnapshot       HealthSnapshot
	gotApproval       [3]string
	gotConflictDays   uint32

	err error
}

func (f *fakeMemory) record(name string) { f.lifecycle.calls = append(f.lifecycle.calls, name) }

func (f *fakeMemory) DeleteL0Provenance(context.Context) (uint32, error) {
	f.record("l0-provenance")
	return f.lifecycle.l0Provenance, f.err
}

func (f *fakeMemory) DeleteL0(context.Context) (uint32, error) {
	f.record("l0")
	return f.lifecycle.l0Deleted, f.err
}

func (f *fakeMemory) ListKindsInTier(_ context.Context, tier string, max uint32) ([]string, error) {
	kinds := f.lifecycle.kindsByTier[tier]
	if uint32(len(kinds)) > max {
		kinds = kinds[:max]
	}
	return kinds, f.err
}

func (f *fakeMemory) KindExpireDays(_ context.Context, kind string) (uint32, error) {
	return f.lifecycle.expireDays[kind], f.err
}

func (f *fakeMemory) DeleteStaleL1Provenance(_ context.Context, kind, window string) (uint32, error) {
	f.record("stale-provenance:" + kind)
	f.lifecycle.gotWindows = append(f.lifecycle.gotWindows, window)
	return 0, f.err
}

func (f *fakeMemory) DeleteStaleL1(_ context.Context, kind, _ string) (uint32, error) {
	f.record("stale:" + kind)
	return f.lifecycle.staleByKind[kind], f.err
}

func (f *fakeMemory) KindDemotePolicy(_ context.Context, kind string) (float64, uint32, error) {
	return f.lifecycle.demoteConf[kind], f.lifecycle.demoteDays[kind], f.err
}

func (f *fakeMemory) DemoteKind(_ context.Context, stamp, kind string, _ float64, window string) (uint32, error) {
	f.record("demote:" + kind)
	f.lifecycle.gotStamps = append(f.lifecycle.gotStamps, stamp)
	f.lifecycle.gotWindows = append(f.lifecycle.gotWindows, window)
	return f.lifecycle.demotedByKind[kind], f.err
}

func (f *fakeMemory) DemoteCascade(_ context.Context, stamp string) (uint32, error) {
	f.record("cascade")
	f.lifecycle.gotStamps = append(f.lifecycle.gotStamps, stamp)
	return f.lifecycle.cascaded, f.err
}

func (f *fakeMemory) PromoteStable(_ context.Context, stamp string) (uint32, error) {
	f.lifecycle.gotStamps = append(f.lifecycle.gotStamps, stamp)
	return f.lifecycle.promoted, f.err
}

func (f *fakeMemory) ReclassifyDirectives(_ context.Context, requireApproval bool) (uint32, error) {
	f.lifecycle.gotRequireApprove = requireApproval
	return f.lifecycle.reclassified, f.err
}

func (f *fakeMemory) RecordL4Approval(_ context.Context, memoryID uint64, approver, note string) error {
	f.lifecycle.gotApproval = [3]string{itoa(memoryID), approver, note}
	return f.err
}

func (f *fakeMemory) CountMemories(context.Context) (uint32, error) {
	return f.lifecycle.memoriesCounted, f.err
}

func (f *fakeMemory) CountRecentConflicts(_ context.Context, days uint32) (uint32, error) {
	f.lifecycle.gotConflictDays = days
	return f.lifecycle.conflictsCount, f.err
}

func (f *fakeMemory) HealthRecord(_ context.Context, snapshot HealthSnapshot) error {
	f.lifecycle.gotSnapshot = snapshot
	return f.err
}

func (f *fakeMemory) PruneHealth(context.Context, uint32) (uint32, error) {
	return f.lifecycle.prunedHealth, f.err
}

func (f *fakeMemory) PruneContradictions(context.Context, uint32) (uint32, error) {
	return f.lifecycle.prunedConflicts, f.err
}

func itoa(v uint64) string {
	if v == 0 {
		return "0"
	}
	var digits [20]byte
	index := len(digits)
	for v > 0 {
		index--
		digits[index] = byte('0' + v%10)
		v /= 10
	}
	return string(digits[index:])
}
