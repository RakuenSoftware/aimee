package db2

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// resetHarness builds a backend over a scripted schema.
type resetHarness struct {
	recorded     uint32
	vectorTables []string
	rowsPerTable int64
	fkTables     map[string]bool
	executed     *DimensionResetPlan
	executeErr   error
}

func (h *resetHarness) seams() LifecycleSeams {
	return LifecycleSeams{
		QueryRow: func(_ context.Context, query string, args ...any) HealthRow {
			switch {
			case query == sqlKBMetaExists:
				return fakeRow{values: []any{int64(1)}}
			case query == sqlRecordedDimension:
				return textRow{value: itoaU32(h.recorded)}
			case query == sqlInboundForeignKeys:
				table, _ := args[0].(string)
				if h.fkTables[table] {
					return fakeRow{values: []any{int64(1)}}
				}
				return fakeRow{values: []any{int64(0)}}
			case strings.HasPrefix(query, "SELECT count(*) FROM "):
				return fakeRow{values: []any{h.rowsPerTable}}
			}
			return fakeRow{err: errors.New("unexpected query: " + query)}
		},
		Query: func(_ context.Context, query string, _ ...any) (Rows, error) {
			if query != sqlDiscoverVectorTables {
				return nil, errors.New("unexpected query")
			}
			return &nameRows{names: h.vectorTables}, nil
		},
		ResetExecutor: func(_ context.Context, plan DimensionResetPlan) error {
			if h.executeErr != nil {
				return h.executeErr
			}
			copied := plan
			h.executed = &copied
			return nil
		},
	}
}

// nameRows answers a one-column name result set.
type nameRows struct {
	names  []string
	cursor int
	closed bool
}

func (r *nameRows) Next() bool {
	if r.cursor >= len(r.names) {
		return false
	}
	r.cursor++
	return true
}

func (r *nameRows) Scan(dest ...any) error {
	if len(dest) != 1 {
		return errors.New("scan arity mismatch")
	}
	target, ok := dest[0].(**string)
	if !ok {
		return errors.New("unsupported scan target")
	}
	value := r.names[r.cursor-1]
	*target = &value
	return nil
}

func (r *nameRows) Err() error { return nil }
func (r *nameRows) Close()     { r.closed = true }

func TestDimensionResetAppliesThePlan(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings", "memory_embeddings"},
		rowsPerTable: 100,
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, status, err := backend.DimensionReset(context.Background(), 1536, false, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetApplied {
		t.Fatalf("outcome = %v", outcome)
	}
	if harness.executed == nil {
		t.Fatal("the executor was never called")
	}
	if len(harness.executed.Tables) != 2 || harness.executed.TargetDimension != 1536 {
		t.Errorf("plan = %+v", harness.executed)
	}
	if status.TablesDropped != 2 || status.RowsCleared != 200 {
		t.Errorf("status = %+v", status)
	}
}

// Asking for the width the corpus already has must not destroy a correct
// corpus.
func TestDimensionResetIsANoOpAtTheRecordedWidth(t *testing.T) {
	harness := &resetHarness{recorded: 1536, vectorTables: []string{"kb_embeddings"}}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, status, err := backend.DimensionReset(context.Background(), 1536, false, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetNoChange {
		t.Fatalf("outcome = %v", outcome)
	}
	if harness.executed != nil {
		t.Fatal("nothing may be dropped when the width already matches")
	}
	if status.TablesDropped != 0 {
		t.Errorf("status = %+v", status)
	}
}

// A vector table outside the derived set may hold the only copy of something.
// Refusing is the safe answer.
func TestDimensionResetRefusesUnknownVectorTable(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings", "someone_elses_vectors"},
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, status, err := backend.DimensionReset(context.Background(), 1536, false, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetRefused {
		t.Fatalf("outcome = %v, want refused", outcome)
	}
	if harness.executed != nil {
		t.Fatal("an unknown table must stop the reset before anything is dropped")
	}
	if status.TablesDiscovered != 2 {
		t.Errorf("the operator must see what was found, got %+v", status)
	}
}

// Force does not license dropping a table this reset cannot rebuild. Force
// covers cascading foreign keys, not unknown data.
func TestDimensionResetRefusesUnknownTableEvenWithForce(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"someone_elses_vectors"},
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, _, err := backend.DimensionReset(context.Background(), 1536, true, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetRefused {
		t.Fatalf("outcome = %v, want refused even under force", outcome)
	}
	if harness.executed != nil {
		t.Fatal("force must not drop a table the reset cannot rebuild")
	}
}

// The foreign-key guard runs before anything is dropped, so a refusal leaves
// the schema untouched rather than half-reset.
func TestDimensionResetNeedsForceForInboundForeignKeys(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings", "memory_embeddings"},
		fkTables:     map[string]bool{"memory_embeddings": true},
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, _, err := backend.DimensionReset(context.Background(), 1536, false, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetNeedsForce {
		t.Fatalf("outcome = %v, want needs-force", outcome)
	}
	if harness.executed != nil {
		t.Fatal("the guard must run before any drop")
	}
}

func TestDimensionResetForceProceedsPastForeignKeys(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings"},
		fkTables:     map[string]bool{"kb_embeddings": true},
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, _, err := backend.DimensionReset(context.Background(), 1536, true, false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetApplied {
		t.Fatalf("outcome = %v", outcome)
	}
	if harness.executed == nil {
		t.Fatal("force must proceed past the foreign-key guard")
	}
}

// A dry run reports what would go without dropping anything.
func TestDimensionResetDryRunDropsNothing(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings", "memory_embeddings"},
		rowsPerTable: 50,
	}
	backend := NewPGLifecycleBackend(harness.seams())

	outcome, status, err := backend.DimensionReset(context.Background(), 1536, false, true)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if outcome != DimensionResetApplied {
		t.Fatalf("outcome = %v", outcome)
	}
	if harness.executed != nil {
		t.Fatal("a dry run must not execute")
	}
	if status.TablesDropped != 0 {
		t.Errorf("a dry run drops nothing, got %d", status.TablesDropped)
	}
	if status.RowsCleared != 100 {
		t.Errorf("a dry run must still report the cost, got %d", status.RowsCleared)
	}
}

// The table name is interpolated into the count, which is only safe because it
// came from the catalog and was matched against the derived set first.
func TestTableRowsRefusesANameOutsideTheDerivedSet(t *testing.T) {
	backend := &pgLifecycleBackend{queryRow: func(context.Context, string, ...any) HealthRow {
		return fakeRow{values: []any{int64(0)}}
	}}
	if _, err := backend.tableRows(context.Background(), "memories; DROP TABLE memories"); err == nil {
		t.Fatal("a name outside the derived set must never be interpolated")
	}
}

// Without an executor the reset must fail rather than report a success it did
// not perform.
func TestDimensionResetWithoutAnExecutorFails(t *testing.T) {
	harness := &resetHarness{recorded: 768, vectorTables: []string{"kb_embeddings"}}
	seams := harness.seams()
	seams.ResetExecutor = nil
	backend := NewPGLifecycleBackend(seams)

	if _, _, err := backend.DimensionReset(context.Background(), 1536, false, false); err == nil {
		t.Fatal("expected a failure when no executor is installed")
	}
}

func TestDimensionResetPropagatesExecutorFailure(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings"},
		executeErr:   errors.New("rollback"),
	}
	backend := NewPGLifecycleBackend(harness.seams())

	if _, _, err := backend.DimensionReset(context.Background(), 1536, false, false); err == nil {
		t.Fatal("an executor failure must not read as success")
	}
}

// The outcomes map onto distinct wire results so a caller can tell "already
// done" from "refused" from "needs force".
func TestDimensionResetOutcomesMapToDistinctResults(t *testing.T) {
	if DimensionResetApplied.result() != db2contract.ResultOK ||
		DimensionResetNoChange.result() != db2contract.ResultOK {
		t.Error("a completed or unnecessary reset is a success")
	}
	if DimensionResetNeedsForce.result() != db2contract.ResultConflict {
		t.Error("a foreign-key refusal is a conflict the operator can override")
	}
	if DimensionResetRefused.result() != db2contract.ResultDenied {
		t.Error("an unknown vector table is a denial, not something force fixes")
	}
}

func TestLifecycleHandlerServesDimensionReset(t *testing.T) {
	harness := &resetHarness{
		recorded:     768,
		vectorTables: []string{"kb_embeddings"},
		rowsPerTable: 7,
	}
	handler := NewLifecycleHandler(NewPGLifecycleBackend(harness.seams()))

	request, err := db2contract.EncodeDimensionResetRequest(1536, 0, 1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := handler(lifecycleInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, decoded, err := db2contract.DecodeDimensionResetReply(reply)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if result != db2contract.ResultOK {
		t.Errorf("result = %d", result)
	}
	if decoded.TargetDimension != 1536 || decoded.RecordedDimension != 768 {
		t.Errorf("decoded = %+v", decoded)
	}
}
