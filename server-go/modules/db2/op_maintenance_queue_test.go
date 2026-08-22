package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestRetryableFailuresGroupRatherThanDistinct(t *testing.T) {
	// PostgreSQL refuses to order a SELECT DISTINCT by a column that is not
	// selected. It is an error, not a warning, so the C form never ran and this
	// retry queue always came back empty -- every failed embed stayed failed.
	// A statement test, because a fake will happily return rows for SQL no
	// database would accept.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRetryableIndexFailuresRequest(3, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRetryableIndexFailures), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "SELECT DISTINCT") {
		t.Errorf("the statement is back to a form PostgreSQL will not run: %q",
			store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "GROUP BY memory_id") ||
		!strings.Contains(store.lastSQL, "ORDER BY MIN(updated_at)") {
		t.Errorf("the queue is no longer one row per memory, oldest first: %q",
			store.lastSQL)
	}
	// Bounded by attempts, which is what stops a permanently broken embed from
	// being retried forever.
	if !strings.Contains(store.lastSQL, "attempts < $1") {
		t.Errorf("the attempt ceiling is gone: %q", store.lastSQL)
	}
}

func TestRetryableFailuresTreatZeroAsNoLimit(t *testing.T) {
	// The C appends its LIMIT clause only when the limit is positive, so zero
	// means every retryable memory rather than none.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRetryableIndexFailuresRequest(3, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRetryableIndexFailures), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "LIMIT NULLIF($2, 0)") {
		t.Errorf("a zero limit would truncate to nothing: %q", store.lastSQL)
	}
}

func TestLinkNeighboursWritesBothDirectionsTogether(t *testing.T) {
	// A chain with only one direction is worse than no chain: context expansion
	// walks it one way and stops the other, so a caller reading around a chunk
	// silently gets half the neighbourhood. The C issues the two updates
	// separately and ignores both results.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentsLinkNeighboursRequest(9, 4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocumentsLinkNeighbours), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBDocumentsLinkNeighboursReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) != 2 ||
		!strings.Contains(store.sqlLog[0], "prev_chunk_id") ||
		!strings.Contains(store.sqlLog[1], "next_chunk_id") {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	// The back link is set on the later chunk and the forward link on the
	// earlier one, so the two arguments are swapped between the statements.
	if store.argsLog[0][0] != int64(4) || store.argsLog[0][1] != int64(9) ||
		store.argsLog[1][0] != int64(9) || store.argsLog[1][1] != int64(4) {
		t.Fatalf("the link points the wrong way: %v then %v",
			store.argsLog[0], store.argsLog[1])
	}
}

func TestLinkNeighboursRollsBackARoundHalfDone(t *testing.T) {
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentsLinkNeighboursRequest(9, 4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocumentsLinkNeighbours), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBDocumentsLinkNeighboursReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
}

func TestIngestQueueFailStampsCompletion(t *testing.T) {
	// completed_at means "stopped running", not "succeeded". The dedup index
	// admits one pending-or-running row per project, so a failed job that never
	// completed would hold that slot forever.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeIngestQueueFailRequest(4, "extractor crashed")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageIngestQueueFail), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	// No row check: a job already recorded failed takes the newer message,
	// which is the right answer when a retry fails differently.
	acknowledged, decodeErr := db2contract.DecodeIngestQueueFailReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "completed_at =") {
		t.Errorf("a failed job would hold its queue slot forever: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "extractor crashed" {
		t.Fatalf("args = %v -- the reason is not recorded", store.lastArgs)
	}
}

func TestWitnessFreshnessSeparatesEmptyFromFresh(t *testing.T) {
	// An age of zero means either a checkpoint written this second or no
	// checkpoints at all, and those are opposite states. Without the read flag
	// a monitor would take "perfectly fresh" from an empty table.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(0), idPtr(0)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeWitnessCheckpointFreshnessRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageWitnessCheckpointFreshness), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	read, count, age, decodeErr := db2contract.DecodeWitnessCheckpointFreshnessReply(body)
	if decodeErr != nil || read != 1 {
		t.Fatalf("read = %d -- the reply cannot be told apart from a failed read", read)
	}
	if count != 0 || age != 0 {
		t.Fatalf("count = %d, age = %d", count, age)
	}
	// The count is what distinguishes the two: zero checkpoints, not a fresh
	// one. A caller reading only the age would have no way to tell.
	if !strings.Contains(store.lastSQL, "count(*)") {
		t.Errorf("the count is gone, leaving the age unreadable: %q", store.lastSQL)
	}
}

func TestDocumentDeleteStaysInTheCurrentGeneration(t *testing.T) {
	// A re-ingest must not be able to empty a published generation.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentsDeleteForFileRequest("replay-project", "a.md")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageKBDocumentsDeleteForFile), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL,
		"generation = (SELECT current_generation FROM projects") {
		t.Errorf("the delete is not scoped to the current generation: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 {
		t.Fatalf("args = %v -- the generation must not come from the caller",
			store.lastArgs)
	}
}
