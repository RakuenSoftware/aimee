package db2

import (
	"context"
	"errors"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestMemoryHandlerHealthCountersUsesContractPolicy(t *testing.T) {
	backend := &fakeMemory{healthCounters: db2contract.HealthCounters{
		Cycles: 2, TotalPromotions: 5, L2Total: 9,
	}}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeHealthCountersRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotPromoteUseCount != db2contract.HealthCountersPromoteUseCount {
		t.Errorf("use count = %d", backend.gotPromoteUseCount)
	}
	want := math.Float64frombits(db2contract.HealthCountersPromoteConfidenceBits)
	if backend.gotPromoteConfidence != want {
		t.Errorf("confidence = %v, want %v", backend.gotPromoteConfidence, want)
	}
	counters, err := db2contract.DecodeHealthCountersReply(reply)
	if err != nil || counters != backend.healthCounters {
		t.Fatalf("counters = %+v, err = %v", counters, err)
	}
}

func TestMemoryHandlerStatsCounts(t *testing.T) {
	stats := db2contract.MemoryStats{Total: 12, Conflicts: 1}
	stats.TierCounts[2] = 7
	stats.KindCounts[0] = 5
	backend := &fakeMemory{memoryStats: stats}

	reply, status := NewMemoryHandler(backend)(memoryInvocation(),
		db2contract.EncodeStatsCountsRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	got, err := db2contract.DecodeStatsCountsReply(reply)
	if err != nil || got != stats {
		t.Fatalf("stats = %+v, err = %v", got, err)
	}
}

// --- backend ---

// groupedRows answers a two-column label/count result set.
type groupedRows struct {
	labels []string
	counts []int64
	cursor int
	closed bool
}

func (r *groupedRows) Next() bool {
	if r.cursor >= len(r.labels) {
		return false
	}
	r.cursor++
	return true
}

func (r *groupedRows) Scan(dest ...any) error {
	if len(dest) != 2 {
		return errors.New("scan arity mismatch")
	}
	label, ok := dest[0].(**string)
	if !ok {
		return errors.New("unsupported label target")
	}
	count, ok := dest[1].(*int64)
	if !ok {
		return errors.New("unsupported count target")
	}
	value := r.labels[r.cursor-1]
	if value == "" {
		*label = nil
	} else {
		copied := value
		*label = &copied
	}
	*count = r.counts[r.cursor-1]
	return nil
}

func (r *groupedRows) Err() error { return nil }
func (r *groupedRows) Close()     { r.closed = true }

func TestPGMemoryBackendStatsCountsMapsSlots(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{
		Query: func(_ context.Context, query string, _ ...any) (Rows, error) {
			switch query {
			case sqlStatsByTier:
				return &groupedRows{labels: []string{"L0", "L2", "L5"}, counts: []int64{1, 2, 3}}, nil
			case sqlStatsByKind:
				return &groupedRows{labels: []string{"fact", "opinion"}, counts: []int64{4, 5}}, nil
			}
			return nil, errors.New("unexpected query")
		},
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return fakeRow{values: []any{int64(9)}}
		},
	})

	stats, err := backend.StatsCounts(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if stats.TierCounts[0] != 1 || stats.TierCounts[2] != 2 || stats.TierCounts[5] != 3 {
		t.Errorf("tier counts = %v", stats.TierCounts)
	}
	// fact is slot 0 and opinion is the last slot; a shifted order here would
	// reattribute every count.
	if stats.KindCounts[0] != 4 || stats.KindCounts[db2contract.StatsCountsKinds-1] != 5 {
		t.Errorf("kind counts = %v", stats.KindCounts)
	}
	// Distinct from the conflict count on purpose: equal values would let a
	// total sourced from the wrong query pass.
	if stats.Total != 6 {
		t.Errorf("total = %d, want the summed tier counts", stats.Total)
	}
	if stats.Conflicts != 9 {
		t.Errorf("conflicts = %d", stats.Conflicts)
	}
}

// A tier the contract has no slot for still belongs in the total, which is what
// callers reconcile the breakdown against.
func TestPGMemoryBackendStatsCountsTotalsUnknownTiers(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{
		Query: func(_ context.Context, query string, _ ...any) (Rows, error) {
			if query == sqlStatsByTier {
				return &groupedRows{labels: []string{"L0", "L9"}, counts: []int64{2, 40}}, nil
			}
			return &groupedRows{}, nil
		},
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return fakeRow{values: []any{int64(0)}}
		},
	})
	stats, err := backend.StatsCounts(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if stats.Total != 42 {
		t.Errorf("total = %d, want 42 including the unslotted tier", stats.Total)
	}
	if stats.TierCounts[0] != 2 {
		t.Errorf("tier counts = %v", stats.TierCounts)
	}
}

// A counter beyond the wire's range fails the operation rather than clamping:
// a clamped counter is a wrong rate presented as a right one.
func TestNarrowCounterRefusesOutOfRange(t *testing.T) {
	if _, err := narrowCounter(-1); err == nil {
		t.Error("expected an error for a negative counter")
	}
	if _, err := narrowCounter(int64(db2contract.HealthCountersMax) + 1); err == nil {
		t.Error("expected an error for an over-large counter")
	}
	if got, err := narrowCounter(7); err != nil || got != 7 {
		t.Errorf("narrowCounter(7) = %d, %v", got, err)
	}
}

// SUM over an empty tier is NULL, which is zero stale rows rather than a fault.
func TestPGMemoryBackendHealthCountersToleratesNullStaleSum(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{
		QueryRow: func(_ context.Context, query string, _ ...any) HealthRow {
			switch query {
			case sqlHealthCycles:
				return fakeRow{values: []any{int64(1), int64(2), int64(3), int64(4), int64(5)}}
			case sqlHealthL2Totals:
				return nullStaleRow{}
			}
			return fakeRow{values: []any{int64(0)}}
		},
	})
	counters, err := backend.HealthCounters(context.Background(), 3, 0.7)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if counters.Cycles != 1 || counters.TotalExpirations != 5 {
		t.Errorf("counters = %+v", counters)
	}
	if counters.L2Stale30Days != 0 {
		t.Errorf("stale = %d, want 0 for a NULL sum", counters.L2Stale30Days)
	}
}

// nullStaleRow reports a real COUNT beside a NULL SUM.
type nullStaleRow struct{}

func (nullStaleRow) Scan(dest ...any) error {
	if len(dest) != 2 {
		return errors.New("scan arity mismatch")
	}
	total, ok := dest[0].(*int64)
	if !ok {
		return errors.New("unsupported total target")
	}
	*total = 4
	return nil
}
