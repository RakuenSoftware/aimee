package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestCountEmbeddingsCountsForTheVersionItIsGiven(t *testing.T) {
	// The count gates an embedder rollback: zero refuses, anything else makes
	// the requested version active. It used to ignore the version and count
	// every indexed memory unit, so a rollback to a version nothing had been
	// embedded at passed the gate and left vector search reading a version with
	// no vectors.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(12)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCountEmbeddingsForVersionRequest("embedder-v2")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCountEmbeddingsForVersion), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	count, decodeErr := db2contract.DecodeCountEmbeddingsForVersionReply(body)
	if decodeErr != nil || count != 12 {
		t.Fatalf("count = %d", count)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != "embedder-v2" {
		t.Fatalf("args = %v -- the version never reached the statement", store.lastArgs)
	}
	if !strings.Contains(store.lastSQL, "embedding_version = $1") {
		t.Errorf("the statement does not filter by version: %q", store.lastSQL)
	}
	// Still restricted to vectors that were actually written. A failed attempt
	// produced no vector, so counting it would be the same lie in a new place.
	if !strings.Contains(store.lastSQL, "status = 'ok'") {
		t.Errorf("the count admits rows that never produced a vector: %q", store.lastSQL)
	}
}

func TestResetStuckCannotBeAskedForAZeroThreshold(t *testing.T) {
	// attempts >= 0 matches every failed row, including ones never tried, so a
	// zero threshold would reset the entire queue rather than the stuck part of
	// it. The contract floors the field at 1, which means the envelope is
	// refused before it reaches the module at all.
	if _, err := db2contract.EncodeResetStuckVectorOpsRequest(0); err == nil {
		t.Fatal("a zero threshold encoded; it would reset every failed row")
	}
	// The guard in the operation is defence behind that, for a caller reaching
	// the Op directly rather than through the wire.
	store := &fakeStore{}
	body, status := resetStuckVectorOps(t.Context(), store, mustEncode(t,
		func() ([]byte, error) { return db2contract.EncodeResetStuckVectorOpsRequest(1) }))
	if status != bus.ModuleStatusOK || body == nil {
		t.Fatalf("status = %v", status)
	}
}

func mustEncode(t *testing.T, build func() ([]byte, error)) []byte {
	t.Helper()
	payload, err := build()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	return payload
}

func TestResetStuckClearsBothQueuesAndSumsThem(t *testing.T) {
	// One operation, two queues: `memory repair --reset-stuck` is meant to retry
	// orphaned code embeds too. A port that reset only the vector ops would look
	// correct and quietly leave the code-chunk queue stuck.
	store := &fakeStore{execRowsAt: true, execRows: 3}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeResetStuckVectorOpsRequest(5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageResetStuckVectorOps), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	reset, decodeErr := db2contract.DecodeResetStuckVectorOpsReply(body)
	if decodeErr != nil || reset != 6 {
		t.Fatalf("reset = %d, want both queues summed", reset)
	}
	if len(store.sqlLog) != 2 ||
		!strings.Contains(store.sqlLog[0], "vector_index_ops") ||
		!strings.Contains(store.sqlLog[1], "code_index_ops") {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if store.txCalls != 0 {
		t.Error("the two queues were wrapped together; a failure on one would " +
			"discard a reset on the other that had already worked")
	}
}

func TestPromoteRefusesAnIdentifierNothingHolds(t *testing.T) {
	// The retire and the promote are both UPDATEs that succeed when they match
	// nothing, so without the existence check this would retire the active
	// release, report success, and leave the installation with none.
	store := &fakeStore{} // No row: the release does not exist.
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReleasePromoteRequest(900)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReleasePromote), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBReleasePromoteReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.execCalls != 0 {
		t.Fatalf("statements = %d -- something was written for a release that "+
			"does not exist", store.execCalls)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}

func TestPromoteRetiresTheCurrentThenPointsAtTheNew(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(900)}}, // the release exists
		{values: []any{ptr("400")}}, // the current active pointer
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReleasePromoteRequest(900)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReleasePromote), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBReleasePromoteReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if store.execCalls != 3 {
		t.Fatalf("statements = %d, want the retire, the promote and the pointer",
			store.execCalls)
	}
	// Order matters: the old release is retired before the new one is promoted,
	// so there is never a moment with two active releases.
	writes := store.sqlLog[2:]
	if !strings.Contains(writes[0], "'retired'") ||
		!strings.Contains(writes[1], "'active'") ||
		!strings.Contains(writes[2], "kb_runtime_state") {
		t.Fatalf("statements out of order: %v", writes)
	}
}

func TestPromoteWithNothingActiveRetiresNothing(t *testing.T) {
	// An unset or unparseable pointer means nothing is active, which is an
	// ordinary state for a fresh installation. The C reaches the same place
	// through atoll, which answers zero for anything it cannot read.
	for _, pointer := range []any{(*string)(nil), ptr(""), ptr("not-a-number"), ptr("0")} {
		store := &fakeStore{rowQueue: []*fakeRow{
			{values: []any{idPtr(900)}},
			{values: []any{pointer}},
		}}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeKBReleasePromoteRequest(900)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		if _, status := handler(
			invocation(db2contract.StageKBReleasePromote), request); status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		if store.execCalls != 2 {
			t.Fatalf("pointer %v: statements = %d, want the promote and the pointer "+
				"with no retire", pointer, store.execCalls)
		}
	}
}

func TestRollbackWithoutATargetTakesTheLastRetired(t *testing.T) {
	// "Go back one": the last release to be retired is the one that was active
	// before the current one, so promoting it undoes the last promotion.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(400)}}, // the last retired release
		{values: []any{idPtr(400)}}, // it exists
		{values: []any{ptr("900")}}, // the current active pointer
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReleaseRollbackRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReleaseRollback), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBReleaseRollbackReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.sqlLog[0], "ORDER BY retired_at DESC LIMIT 1") {
		t.Errorf("the target was not resolved to the most recently retired: %q",
			store.sqlLog[0])
	}
}

func TestRollbackWithNothingRetiredRefuses(t *testing.T) {
	// There is no previous state to return to.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReleaseRollbackRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReleaseRollback), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBReleaseRollbackReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.execCalls != 0 {
		t.Fatalf("statements = %d -- something was written with no target",
			store.execCalls)
	}
}

func TestVectorIndexOpRemoveAcknowledgesAnAbsentPoint(t *testing.T) {
	// A point already forgotten is the expected case for a caller cleaning up,
	// and the C ignores the outcome entirely.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request := mustEncode(t, func() ([]byte, error) {
		return db2contract.EncodeVectorIndexOpRemoveRequest(900)
	})
	body, status := handler(invocation(db2contract.StageVectorIndexOpRemove), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	recorded, decodeErr := db2contract.DecodeVectorIndexOpRemoveReply(body)
	if decodeErr != nil || recorded != 1 {
		t.Fatalf("recorded = %d -- a row count crept into the answer", recorded)
	}
}

func TestVectorIndexOpRemoveReportsFailureAsAStatus(t *testing.T) {
	// The reply field is bounded to exactly 1, so the payload cannot say "this
	// did not work". A failed statement has to become a module status, because
	// the alternative is encoding a success that did not happen.
	if db2contract.VectorIndexOpRemoveRecordedMin != 1 ||
		db2contract.VectorIndexOpRemoveRecordedMax != 1 {
		t.Fatal("the reply can now express something other than success; the " +
			"operation should report it in the payload rather than as a status")
	}
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request := mustEncode(t, func() ([]byte, error) {
		return db2contract.EncodeVectorIndexOpRemoveRequest(900)
	})
	body, status := handler(invocation(db2contract.StageVectorIndexOpRemove), request)
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want Internal", status)
	}
	if body != nil {
		t.Error("a reply body was returned alongside a failure status")
	}
}

func TestRuntimeStateGetAnswersEmptyForAnUnsetKey(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeRuntimeStateGetRequest("never-set")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRuntimeStateGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	value, decodeErr := db2contract.DecodeRuntimeStateGetReply(body)
	if decodeErr != nil || value != "" {
		t.Fatalf("value = %q", value)
	}
}
