package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestTaskStartsTodoAndCertain(t *testing.T) {
	// Both literals are fixed at creation: a task nobody has done yet, that
	// nothing has cast doubt on. Taking either from the caller would let a task
	// be opened already finished.
	store := &fakeStore{row: &fakeRow{values: []any{int64(41)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskCreateRequest("write it down", "session-1", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskCreate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	taskID, decodeErr := db2contract.DecodeTaskCreateReply(body)
	if decodeErr != nil || taskID != 41 {
		t.Fatalf("task id = %d", taskID)
	}
	if !strings.Contains(store.lastSQL, "'todo', 1.0") {
		t.Errorf("a task can now be opened in some other state: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "RETURNING id") {
		t.Errorf("the new task's identifier is no longer read back: %q", store.lastSQL)
	}
}

func TestTaskCreationFailureAnswersZero(t *testing.T) {
	// Zero is "no task", which is what the adapter answers for the C's failure
	// return -- and no task ever has that identifier, so it cannot be mistaken
	// for one.
	store := &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskCreateRequest("write it down", "session-1", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskCreate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	taskID, decodeErr := db2contract.DecodeTaskCreateReply(body)
	if decodeErr != nil || taskID != 0 {
		t.Fatalf("task id = %d, want 0", taskID)
	}
}

func TestTaskEdgeDoesNotDeduplicate(t *testing.T) {
	// task_edges has no uniqueness constraint, so the same edge added twice is
	// two rows. Adding an ON CONFLICT here would be a schema decision dressed
	// up as a port -- and it would need a constraint to name that does not
	// exist.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskAddEdgeRequest(1, 2, "depends_on")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskAddEdge), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeTaskAddEdgeReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if strings.Contains(store.lastSQL, "ON CONFLICT") {
		t.Errorf("the insert claims a constraint the table does not have: %q", store.lastSQL)
	}
}

func TestToolLookupSeparatesUnregisteredFromDisabled(t *testing.T) {
	// A caller can register the first and cannot enable the second, so
	// collapsing them would send someone looking for a switch that is not
	// there.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeToolRegistryLookupRequest("nothing-here")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageToolRegistryLookup), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, _, _, enabled, decodeErr := db2contract.DecodeToolRegistryLookupReply(body)
	if decodeErr != nil || found != 0 || enabled != 0 {
		t.Fatalf("found = %d, enabled = %d", found, enabled)
	}

	store = &fakeStore{row: &fakeRow{values: []any{"{}", "write", int64(0)}}}
	handler = NewDispatchHandler(store)
	body, status = handler(invocation(db2contract.StageToolRegistryLookup), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, schema, sideEffect, enabled, decodeErr :=
		db2contract.DecodeToolRegistryLookupReply(body)
	if decodeErr != nil || found != 1 || enabled != 0 {
		t.Fatalf("a registered, disabled tool answered found = %d, enabled = %d",
			found, enabled)
	}
	if schema != "{}" || sideEffect != "write" {
		t.Fatalf("schema = %q, side effect = %q", schema, sideEffect)
	}
}

func TestToolEnabledIsAFlagNotACount(t *testing.T) {
	// The column is a BIGINT, and any non-zero means enabled. Passing the value
	// through would overflow the reply's one-bit field for anything but zero
	// and one.
	store := &fakeStore{row: &fakeRow{values: []any{"{}", "read", int64(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeToolRegistryLookupRequest("odd-one")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageToolRegistryLookup), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	_, _, _, enabled, decodeErr := db2contract.DecodeToolRegistryLookupReply(body)
	if decodeErr != nil || enabled != 1 {
		t.Fatalf("enabled = %d, want 1", enabled)
	}
}

func TestBlockedSymbolsRebuildsInOneTransaction(t *testing.T) {
	// Clear and refill together, so no reader sees a partial list. The whole
	// point of the table is that a lookup against it is complete; a half-filled
	// one would let symbols through for as long as the refill took.
	store := &fakeStore{
		rowQueue: []*fakeRow{{values: []any{int64(12)}}},
		execRows: 41, execRowsAt: true,
	}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRecomputeBlockedSymbolsRequest(3, 2, 4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRecomputeBlockedSymbols), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	blocked, decodeErr := db2contract.DecodeRecomputeBlockedSymbolsReply(body)
	if decodeErr != nil || blocked != 41 {
		t.Fatalf("blocked = %d", blocked)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) != 3 {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if !strings.Contains(store.sqlLog[0], "blocked_symbols_version + 1") {
		t.Errorf("the version is not bumped first: %q", store.sqlLog[0])
	}
	if !strings.Contains(store.sqlLog[1], "DELETE FROM blocked_symbols") {
		t.Errorf("the old list is not cleared before the refill: %q", store.sqlLog[1])
	}
	// The version read back from the bump is what the new rows are stamped
	// with. Stamping them with anything else would leave a list nobody can
	// match against the version a reader holds.
	if store.argsLog[2][2] != int64(12) {
		t.Errorf("rows stamped %v, want the bumped version", store.argsLog[2][2])
	}
}

func TestBlockedSymbolsFailureLeavesNothingHalfDone(t *testing.T) {
	// Without a meta row there is no version to bump, and inventing one would
	// hand out a version another recompute may already have used. Failing means
	// the old list survives, which is the safe half of the trade.
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRecomputeBlockedSymbolsRequest(3, 2, 4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRecomputeBlockedSymbols), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	blocked, decodeErr := db2contract.DecodeRecomputeBlockedSymbolsReply(body)
	if decodeErr != nil || blocked != 0 {
		t.Fatalf("blocked = %d, want 0", blocked)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}
