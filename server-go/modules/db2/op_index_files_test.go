package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestCodeFileUpsertDerivesLanguageAndVendoring(t *testing.T) {
	// The request carries neither, so they are derived here from the path --
	// the same point the C derives them. Re-derived on every upsert rather than
	// only on insert, so a classifier change takes effect on the next scan.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeFileUpsertRequest(
		4, "vendor/pkg/main.go", "2026-01-01 00:00:00")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeFileUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeCodeFileUpsertReply(body)
	if decodeErr != nil || id != 7 {
		t.Fatalf("id = %d", id)
	}
	if len(store.lastArgs) != 5 {
		t.Fatalf("args = %v", store.lastArgs)
	}
	if store.lastArgs[3] != "go" {
		t.Errorf("language = %v, want go", store.lastArgs[3])
	}
	if store.lastArgs[4] != 1 {
		t.Errorf("vendored = %v for a path under vendor/", store.lastArgs[4])
	}
	// The update half re-derives too, or a reclassification would only reach
	// files nobody had scanned before.
	if !strings.Contains(store.lastSQL, "SET scanned_at = $3, language = $4, vendored = $5") {
		t.Errorf("the conflict path does not refresh the derived columns: %q",
			store.lastSQL)
	}
}

func TestCodeFileUpsertTakesTheGenerationFromTheProject(t *testing.T) {
	// INSERT ... SELECT, so a project that is not current inserts nothing and
	// the caller cannot name a generation of its own.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeFileUpsertRequest(
		4, "src/main.c", "2026-01-01 00:00:00")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCodeFileUpsert), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "SELECT $1, current_generation") ||
		!strings.Contains(store.lastSQL, "lifecycle_state = 'current'") {
		t.Errorf("the generation no longer comes from the project row: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (project_id, generation, path)") {
		t.Errorf("a re-scan would duplicate rather than update: %q", store.lastSQL)
	}
}

func TestFileModifiedSinceTreatsAnAbsentFileAsModified(t *testing.T) {
	// A file new to this generation has no row, and answering "unmodified"
	// would leave it unindexed.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeFileModifiedSinceRequest(4, "src/new.c", 1000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFileModifiedSince), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	modified, decodeErr := db2contract.DecodeFileModifiedSinceReply(body)
	if decodeErr != nil || modified != 1 {
		t.Fatalf("modified = %d for a file with no row, want 1", modified)
	}
}

func TestFileModifiedSinceGuardsItsCast(t *testing.T) {
	// PostgreSQL has no try-cast, so a stamp in neither spelling would raise
	// rather than answer. The regular expression is what keeps an unrecognised
	// stamp reading as modified -- the safe direction, and the C's.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(0)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFileModifiedSinceRequest(4, "src/main.c", 1000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFileModifiedSince), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	modified, decodeErr := db2contract.DecodeFileModifiedSinceReply(body)
	if decodeErr != nil || modified != 0 {
		t.Fatalf("modified = %d for a file scanned since, want 0", modified)
	}
	if !strings.Contains(store.lastSQL, "f.scanned_at ~ ") {
		t.Errorf("the cast is unguarded; a malformed stamp would raise: %q",
			store.lastSQL)
	}
	// Both spellings the column holds: the ISO form and the space-separated one
	// pg_now_text writes. Accepting only the first is what once re-indexed
	// every file on every scan.
	if !strings.Contains(store.lastSQL, "[T ]") {
		t.Errorf("only one timestamp spelling is accepted: %q", store.lastSQL)
	}
}

func TestQueueStatsTakeOneSnapshot(t *testing.T) {
	// The C issues three statements, so its pending count and its done count
	// need not describe the same moment -- a job finishing between them is
	// counted in neither or both. Filtered aggregates give all four from one
	// pass.
	store := &fakeStore{row: &fakeRow{values: []any{
		idPtr(3), idPtr(1), idPtr(12), idPtr(2),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBIngestQueueStatsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBIngestQueueStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	pending, running, done, failed, decodeErr :=
		db2contract.DecodeKBIngestQueueStatsReply(body)
	if decodeErr != nil || pending != 3 || running != 1 || done != 12 || failed != 2 {
		t.Fatalf("pending = %d, running = %d, done = %d, failed = %d",
			pending, running, done, failed)
	}
	if store.execCalls != 0 || len(store.sqlLog) != 1 {
		t.Fatalf("statements = %d, want one snapshot", len(store.sqlLog))
	}
	// The live counts are unbounded and the finished ones are not: a job
	// pending for a week is still pending, while one that finished last month
	// says nothing about the queue now.
	if strings.Count(store.lastSQL, "pg_now_text('-1 day')") != 2 {
		t.Errorf("the time bound is not applied to exactly the finished counts: %q",
			store.lastSQL)
	}
}

func TestAsyncEnqueueDoesNotRearmFinishedWork(t *testing.T) {
	// One job per kind and document. A row already marked done is left alone,
	// so re-running finished work needs it moved back to pending by something
	// else -- worth knowing before assuming this re-queues.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAsyncEnqueueRequest("extract_doc", 9, "replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAsyncEnqueue), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeAsyncEnqueueReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (kind, document_id) DO NOTHING") {
		t.Errorf("a duplicate enqueue would now insert or fail: %q", store.lastSQL)
	}
}

func TestTsrStateWriteCoversEveryChunk(t *testing.T) {
	// The state describes the document, and the read beside it takes whichever
	// chunk answers first. Keeping them equal is what makes that read
	// well-defined.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentsSetTsrStateRequest(
		"replay-project", "a.pdf", "done")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageKBDocumentsSetTsrState), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "LIMIT") {
		t.Errorf("only some chunks would be updated: %q", store.lastSQL)
	}
	// No quarantine check on the write, unlike the read: a document awaiting
	// review still has its recognition state recorded, because what is withheld
	// is telling a caller about it rather than doing the work.
	if strings.Contains(store.lastSQL, "quarantine_state") {
		t.Errorf("the write inherited the read's access rule: %q", store.lastSQL)
	}
}

func TestProjectionGenerationsListShowsEveryState(t *testing.T) {
	// The history a person reads to see what happened. Filtering to the visible
	// generation would answer a question they can already ask another way.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{idPtr(3), ptr("aborted"), ptr("2026-01-01")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionGenerationsListRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectionGenerationsList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeProjectionGenerationsListReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].State != "aborted" {
		t.Fatalf("rows = %+v", found)
	}
	if strings.Contains(store.lastSQL, "state =") {
		t.Errorf("the history was narrowed to one state: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id DESC") {
		t.Errorf("the newest generation is no longer first: %q", store.lastSQL)
	}
}
