package db2

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// hasValue == 0 must clear the score, not store zero. Zero is a measured score
// of nothing; NULL is the absence of a measurement, and the demotion sweep
// selects on IS NOT NULL to tell them apart.
func TestMemoryHandlerEffectivenessUpdateClearsRatherThanStoringZero(t *testing.T) {
	backend := &fakeMemory{}
	request, err := db2contract.EncodeEffectivenessUpdateRequest(7, 0, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !backend.cleared {
		t.Error("hasValue == 0 must clear the score")
	}
	if backend.gotEffectivenessID != 7 {
		t.Errorf("memory id = %d", backend.gotEffectivenessID)
	}
	result, err := db2contract.DecodeEffectivenessUpdateReply(reply)
	if err != nil || result != db2contract.ResultOK {
		t.Fatalf("result = %d, err = %v", result, err)
	}
}

func TestMemoryHandlerEffectivenessUpdateStoresValue(t *testing.T) {
	backend := &fakeMemory{}
	request, err := db2contract.EncodeEffectivenessUpdateRequest(9, 1, 0.75)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := NewMemoryHandler(backend)(memoryInvocation(), request); status != bus.ModuleStatusOK {
		t.Fatal("expected the update to succeed")
	}
	if backend.cleared {
		t.Error("a value update must not clear")
	}
	if backend.gotEffectivenessID != 9 || backend.gotEffectiveness != 0.75 {
		t.Errorf("id = %d, value = %v", backend.gotEffectivenessID, backend.gotEffectiveness)
	}
}

// A memory that is not there is a state the caller must be told about, not a
// fault of the module — so it is a closed result, not an internal status.
func TestMemoryHandlerEffectivenessUpdateReportsInvalidState(t *testing.T) {
	backend := &fakeMemory{err: errors.New("no such memory")}
	request, err := db2contract.EncodeEffectivenessUpdateRequest(7, 1, 0.5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK carrying a closed result", status)
	}
	result, err := db2contract.DecodeEffectivenessUpdateReply(reply)
	if err != nil || result != db2contract.ResultInvalidState {
		t.Fatalf("result = %d, err = %v", result, err)
	}
}

// Retention sweeps both sensitivities and answers their sum: the caller's
// question is how much retention removed, not how much of each.
func TestMemoryHandlerRetentionEnforceSweepsBothSensitivities(t *testing.T) {
	backend := &fakeMemory{}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeRetentionEnforceRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got := backend.retentionDeleted[db2contract.RetentionRestricted]; got != db2contract.RetentionRestrictedDays {
		t.Errorf("restricted days = %d, want %d", got, db2contract.RetentionRestrictedDays)
	}
	if got := backend.retentionDeleted[db2contract.RetentionSensitive]; got != db2contract.RetentionSensitiveDays {
		t.Errorf("sensitive days = %d, want %d", got, db2contract.RetentionSensitiveDays)
	}
	deleted, err := db2contract.DecodeRetentionEnforceReply(reply)
	if err != nil || deleted != 5 {
		t.Fatalf("deleted = %d, err = %v (want the 2+3 sum)", deleted, err)
	}
}

func TestMemoryHandlerEffectivenessDemoteUsesContractThreshold(t *testing.T) {
	backend := &fakeMemory{demoted: 11}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeEffectivenessDemoteRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotThreshold != effectivenessDemoteThreshold() {
		t.Errorf("threshold = %v, want the contract's", backend.gotThreshold)
	}
	demoted, err := db2contract.DecodeEffectivenessDemoteReply(reply)
	if err != nil || demoted != 11 {
		t.Fatalf("demoted = %d, err = %v", demoted, err)
	}
}

func TestMemoryHandlerEffectivenessStats(t *testing.T) {
	backend := &fakeMemory{effStats: db2contract.EffectivenessStats{
		AvgEffectiveness: 0.5, LowEffectivenessCount: 4, HighImpactCount: 2,
	}}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeEffectivenessStatsRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotLowThreshold != effectivenessStatsLowThreshold() {
		t.Errorf("low threshold = %v", backend.gotLowThreshold)
	}
	stats, err := db2contract.DecodeEffectivenessStatsReply(reply)
	if err != nil || stats != backend.effStats {
		t.Fatalf("stats = %+v, err = %v", stats, err)
	}
}

func TestMemoryHandlerL2MemoryIDs(t *testing.T) {
	backend := &fakeMemory{l2IDs: []uint64{1, 2, 3}}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeL2MemoryIDsRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	ids, err := db2contract.DecodeL2MemoryIDsReply(reply)
	if err != nil || len(ids) != 3 {
		t.Fatalf("ids = %v, err = %v", ids, err)
	}
}

// --- backend: mutations over the exec seam ---

func TestPGMemoryBackendClearEffectivenessWritesNull(t *testing.T) {
	var gotArgs []any
	backend := NewPGMemoryBackend(MemorySeams{
		Exec: func(_ context.Context, _ string, args ...any) (int64, error) {
			gotArgs = args
			return 1, nil
		},
	})
	if err := backend.ClearEffectiveness(context.Background(), 5); err != nil {
		t.Fatalf("err = %v", err)
	}
	if len(gotArgs) != 2 || gotArgs[0] != nil {
		t.Fatalf("args = %v, want a NULL first argument", gotArgs)
	}
}

func TestPGMemoryBackendSetEffectivenessWritesValue(t *testing.T) {
	var gotArgs []any
	backend := NewPGMemoryBackend(MemorySeams{
		Exec: func(_ context.Context, _ string, args ...any) (int64, error) {
			gotArgs = args
			return 1, nil
		},
	})
	if err := backend.SetEffectiveness(context.Background(), 5, 0.25); err != nil {
		t.Fatalf("err = %v", err)
	}
	if len(gotArgs) != 2 || gotArgs[0] != 0.25 {
		t.Fatalf("args = %v", gotArgs)
	}
}

// The demotion stamps updated_at client-side, exactly as the C implementation
// does, so the column keeps one grammar across the migration.
func TestPGMemoryBackendDemoteStampsInjectedClock(t *testing.T) {
	fixed := time.Date(2026, 8, 25, 7, 30, 0, 0, time.UTC)
	var gotArgs []any
	backend := NewPGMemoryBackend(MemorySeams{
		Now: func() time.Time { return fixed },
		Exec: func(_ context.Context, _ string, args ...any) (int64, error) {
			gotArgs = args
			return 4, nil
		},
	})
	demoted, err := backend.DemoteLowEffectiveness(context.Background(), 0.3)
	if err != nil || demoted != 4 {
		t.Fatalf("demoted = %d, err = %v", demoted, err)
	}
	if gotArgs[0] != "2026-08-25T07:30:00Z" {
		t.Errorf("timestamp = %v", gotArgs[0])
	}
}

func TestPGMemoryBackendRetentionDeleteBuildsWindow(t *testing.T) {
	var gotArgs []any
	backend := NewPGMemoryBackend(MemorySeams{
		Exec: func(_ context.Context, _ string, args ...any) (int64, error) {
			gotArgs = args
			return 6, nil
		},
	})
	deleted, err := backend.RetentionDelete(context.Background(), "sensitive", 90)
	if err != nil || deleted != 6 {
		t.Fatalf("deleted = %d, err = %v", deleted, err)
	}
	if gotArgs[1] != "-90 days" {
		t.Errorf("window = %v", gotArgs[1])
	}
}

// Empty sensitivity or a zero window must not reach the database: an unbounded
// DELETE is the failure this guard exists to prevent.
func TestPGMemoryBackendRetentionDeleteRefusesEmptyArguments(t *testing.T) {
	executed := false
	backend := NewPGMemoryBackend(MemorySeams{
		Exec: func(context.Context, string, ...any) (int64, error) {
			executed = true
			return 0, nil
		},
	})
	ctx := context.Background()
	if n, err := backend.RetentionDelete(ctx, "", 7); n != 0 || err != nil {
		t.Errorf("empty sensitivity = %d, %v", n, err)
	}
	if n, err := backend.RetentionDelete(ctx, "sensitive", 0); n != 0 || err != nil {
		t.Errorf("zero days = %d, %v", n, err)
	}
	if executed {
		t.Error("an unbounded retention delete must never reach the database")
	}
}

// AVG and SUM over an empty table are NULL, which is a legitimate state rather
// than a failure.
func TestPGMemoryBackendEffectivenessStatsToleratesNulls(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return nullStatsRow{}
		},
	})
	stats, err := backend.EffectivenessStats(context.Background(), 0.3)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if stats != (db2contract.EffectivenessStats{}) {
		t.Errorf("stats = %+v, want zeroes", stats)
	}
}

// nullStatsRow leaves every pointer target nil, the way a driver reports SQL
// NULL into a **float64 / **int64.
type nullStatsRow struct{}

func (nullStatsRow) Scan(dest ...any) error {
	if len(dest) != 3 {
		return errors.New("scan arity mismatch")
	}
	return nil
}

func TestPGMemoryBackendListL2MemoryIDs(t *testing.T) {
	rows := &fakeRows{ids: []int64{1, 2, 0, 3}}
	backend := NewPGMemoryBackend(MemorySeams{
		Query: func(context.Context, string, ...any) (Rows, error) {
			return rows, nil
		},
	})
	ids, err := backend.ListL2MemoryIDs(context.Background(), 10)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	// A leaked result set holds its pooled connection, which is how a pool
	// starves without any single caller looking at fault.
	if !rows.closed {
		t.Error("the result set must be closed")
	}
	// The zero id is below the contract's floor and is dropped rather than
	// encoded as something the wire would refuse.
	want := []uint64{1, 2, 3}
	if len(ids) != len(want) {
		t.Fatalf("ids = %v, want %v", ids, want)
	}
	for i := range want {
		if ids[i] != want[i] {
			t.Fatalf("ids = %v, want %v", ids, want)
		}
	}
}

func TestPGMemoryBackendListL2MemoryIDsHonoursMax(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{
		Query: func(context.Context, string, ...any) (Rows, error) {
			return &fakeRows{ids: []int64{1, 2, 3, 4, 5}}, nil
		},
	})
	ids, err := backend.ListL2MemoryIDs(context.Background(), 2)
	if err != nil || len(ids) != 2 {
		t.Fatalf("ids = %v, err = %v", ids, err)
	}
}

type fakeRows struct {
	ids    []int64
	cursor int
	closed bool
}

func (r *fakeRows) Next() bool {
	if r.cursor >= len(r.ids) {
		return false
	}
	r.cursor++
	return true
}

func (r *fakeRows) Scan(dest ...any) error {
	if len(dest) != 1 {
		return errors.New("scan arity mismatch")
	}
	target, ok := dest[0].(*int64)
	if !ok {
		return errors.New("unsupported scan target")
	}
	*target = r.ids[r.cursor-1]
	return nil
}

func (r *fakeRows) Err() error { return nil }
func (r *fakeRows) Close()     { r.closed = true }
