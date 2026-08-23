package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestFenceTakesTheProjectLockBeforeTheRowLock(t *testing.T) {
	// The C is explicit about why a conditional UPDATE is not enough: under
	// READ COMMITTED, row re-evaluation can act on an old snapshot, so a
	// heartbeat could refresh a fence another purge had already taken. Taking
	// the project lock, then the row FOR UPDATE, then comparing, is what
	// serialises the decision against the mutation -- and the order is the part
	// that matters.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(1)}},             // the advisory lock
		{values: []any{ptr("gen-1 purge-1")}}, // the fence identity row
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFenceHeartbeatRequest(
		"replay-project", "gen-1", "purge-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StagePurgeFenceHeartbeat), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	applied, decodeErr := db2contract.DecodePurgeFenceHeartbeatReply(body)
	if decodeErr != nil || applied != 1 {
		t.Fatalf("applied = %d", applied)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) < 3 {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if !strings.Contains(store.sqlLog[0], "pg_advisory_xact_lock") {
		t.Errorf("the project lock is not taken first: %q", store.sqlLog[0])
	}
	if !strings.Contains(store.sqlLog[1], "FOR UPDATE") {
		t.Errorf("the identity row is not locked before the compare: %q", store.sqlLog[1])
	}
}

func TestHeartbeatWritesTheCanonicalUTCStamp(t *testing.T) {
	// The heartbeat is read back by a liveness check that casts the text to a
	// timestamp and compares it against now in UTC. A stamp written in the
	// server's local zone, or in any other format, would make the fence read as
	// stale or live by the size of the offset -- and nothing about the write
	// itself would look wrong.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(1)}},
		{values: []any{ptr("gen-1 purge-1")}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFenceHeartbeatRequest(
		"replay-project", "gen-1", "purge-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StagePurgeFenceHeartbeat), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "pg_now_text()") {
		t.Errorf("the heartbeat no longer stamps in canonical UTC: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (state_key)") {
		t.Errorf("a second heartbeat would fail rather than refresh: %q", store.lastSQL)
	}
}

func TestFenceRefusesAnotherPurgesClaim(t *testing.T) {
	// A slow purge must not release or refresh the fence out from under the one
	// that displaced it.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(1)}},
		{values: []any{ptr("gen-2 purge-2")}}, // held by someone else
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFenceClearRequest(
		"replay-project", "gen-1", "purge-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StagePurgeFenceClear), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	applied, decodeErr := db2contract.DecodePurgeFenceClearReply(body)
	if decodeErr != nil || applied != 0 {
		t.Fatalf("applied = %d for another purge's fence, want 0", applied)
	}
	if store.execCalls != 0 {
		t.Fatalf("statements = %d -- another purge's fence was mutated",
			store.execCalls)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}

func TestFenceAbsenceIsNotAFailure(t *testing.T) {
	// No fence at all and someone else's fence are the same answer to this
	// caller: it does not hold the project.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{idPtr(1)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFenceHeartbeatRequest(
		"replay-project", "gen-1", "purge-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StagePurgeFenceHeartbeat), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	applied, decodeErr := db2contract.DecodePurgeFenceHeartbeatReply(body)
	if decodeErr != nil || applied != 0 {
		t.Fatalf("applied = %d, want 0", applied)
	}
}

func TestFenceClearRemovesTheIdentityRowFirst(t *testing.T) {
	// The reverse of the write order, deliberately. A torn clear then leaves an
	// orphan heartbeat -- harmless without an identity row -- rather than an
	// identity row with no heartbeat, which is a fence held by nobody that
	// never expires.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{idPtr(1)}},
		{values: []any{ptr("gen-1 purge-1")}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFenceClearRequest(
		"replay-project", "gen-1", "purge-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StagePurgeFenceClear), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	applied, decodeErr := db2contract.DecodePurgeFenceClearReply(body)
	if decodeErr != nil || applied != 1 {
		t.Fatalf("applied = %d", applied)
	}
	if store.execCalls != 2 {
		t.Fatalf("deletes = %d, want both rows", store.execCalls)
	}
	if !strings.Contains(store.argsLog[2][0].(string), "project_purging:") ||
		!strings.Contains(store.argsLog[3][0].(string), "project_purging_ts:") {
		t.Fatalf("the identity row was not cleared first: %v then %v",
			store.argsLog[2], store.argsLog[3])
	}
}

func TestDocumentHashLooksInBothTables(t *testing.T) {
	// A file can be indexed without being chunked into documents, so either
	// table is enough to say the content is present.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("docs/a.md")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDocumentHashExistsRequest("replay-project", "abc123")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDocumentHashExists), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	exists, sample, decodeErr := db2contract.DecodeDocumentHashExistsReply(body)
	if decodeErr != nil || exists != 1 || sample != "docs/a.md" {
		t.Fatalf("exists = %d, sample = %q", exists, sample)
	}
	if !strings.Contains(store.lastSQL, "kb_documents") ||
		!strings.Contains(store.lastSQL, "kb_file_index") {
		t.Errorf("only one table is consulted: %q", store.lastSQL)
	}
	// Both halves pin the generation, so a hash matched in a superseded
	// generation does not read as already ingested.
	if strings.Count(store.lastSQL, "current_generation") != 2 {
		t.Errorf("a superseded generation could answer here: %q", store.lastSQL)
	}
}

func TestSummariesReadOldestFirst(t *testing.T) {
	// Ascending, unlike most reads here. Reading a memory's summaries in the
	// order they were written is what makes the sequence legible.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySummariesListRequest(4, 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemorySummariesList), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id ASC") {
		t.Errorf("the summaries no longer read forwards: %q", store.lastSQL)
	}
}

func TestSessionClustersExcludeTheCallerAndTheUnnamed(t *testing.T) {
	// A session does not consolidate itself while it is still running, and
	// memories with no session are not a session -- pooling them would make one
	// enormous phantom cluster.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryL1SessionClustersRequest("session-mine", 3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryL1SessionClusters), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "source_session != ''") {
		t.Errorf("untracked memories would pool into one cluster: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "source_session != $1") {
		t.Errorf("the caller's own session is no longer excluded: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "HAVING COUNT(*) >= $2") {
		t.Errorf("the minimum cluster size is gone: %q", store.lastSQL)
	}
}

func TestArtifactHashedListReadsFromMemories(t *testing.T) {
	// The artifact columns live on memories rather than in a table of their
	// own, and both are nullable -- IS NOT NULL is the filter, so a memory with
	// no artifact is not a row here at all.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{idPtr(4), ptr("file"), ptr("docs/a.md"), ptr("abc123")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryArtifactHashedListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryArtifactHashedList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoryArtifactHashedListReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].ArtifactHash != "abc123" {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "FROM memories") {
		t.Errorf("the read moved off memories: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "artifact_type IS NOT NULL") ||
		!strings.Contains(store.lastSQL, "artifact_hash IS NOT NULL") {
		t.Errorf("a memory with no artifact could be returned: %q", store.lastSQL)
	}
}
