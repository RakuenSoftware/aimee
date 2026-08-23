package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestConflictListShowsOnlyWhatIsStillOpen(t *testing.T) {
	// A resolved conflict is history rather than work. This list is what a
	// resolution pass reads to decide what to do next, so settled rows would
	// push the open ones off the end of a bounded reply.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(10), int64(11), "2026-01-01T00:00:00Z", int64(0), (*string)(nil)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryConflictListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryConflictList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	conflicts, decodeErr := db2contract.DecodeMemoryConflictListReply(body)
	if decodeErr != nil || len(conflicts) != 1 {
		t.Fatalf("conflicts = %+v", conflicts)
	}
	if conflicts[0].ConflictID != 4 || conflicts[0].MemoryA != 10 ||
		conflicts[0].MemoryB != 11 || conflicts[0].ConflictResolved != 0 {
		t.Fatalf("conflict = %+v", conflicts[0])
	}
	// An unsettled conflict has no resolution, and the column is nullable --
	// which pgx will not scan into a plain string.
	if conflicts[0].ConflictResolution != "" {
		t.Errorf("resolution = %q", conflicts[0].ConflictResolution)
	}
	if !strings.Contains(store.lastSQL, "WHERE resolved = 0") {
		t.Errorf("settled conflicts would be listed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY detected_at DESC") {
		t.Errorf("the newest conflicts no longer lead: %q", store.lastSQL)
	}
}

func TestEventFramesReadInExtractionOrder(t *testing.T) {
	// event_time is free text and frequently empty, so the extraction order is
	// the only ordering these frames have. Ordering by the recorded time would
	// sort most of them under the empty string.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"the deploy script", "wrote", "the release notes", "", "last tuesday"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryEventFramesListRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryEventFramesList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	frames, decodeErr := db2contract.DecodeMemoryEventFramesListReply(body)
	if decodeErr != nil || len(frames) != 1 {
		t.Fatalf("frames = %+v", frames)
	}
	// An unfilled slot is an empty string rather than an absent one: every
	// column is NOT NULL with an empty default, and the extractor fills what
	// the text supports.
	if frames[0].FrameActor != "the deploy script" || frames[0].FrameLocation != "" ||
		frames[0].FrameEventTime != "last tuesday" {
		t.Fatalf("frame = %+v", frames[0])
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id ASC") {
		t.Errorf("the frames no longer read in extraction order: %q", store.lastSQL)
	}
}

func TestProvenanceReadsForwards(t *testing.T) {
	// Provenance is a narrative -- created, then reinforced, then merged -- and
	// reading it forwards is what makes it one.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(7), "session-1", "created", (*string)(nil), "2026-01-01T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryProvenanceListRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryProvenanceList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	entries, decodeErr := db2contract.DecodeMemoryProvenanceListReply(body)
	if decodeErr != nil || len(entries) != 1 {
		t.Fatalf("entries = %+v", entries)
	}
	if entries[0].ProvenanceID != 7 || entries[0].SessionID != "session-1" ||
		entries[0].ProvenanceAction != "created" ||
		entries[0].ProvenanceDetails != "" {
		t.Fatalf("entry = %+v", entries[0])
	}
	if !strings.Contains(store.lastSQL, "ORDER BY created_at ASC") {
		t.Errorf("the narrative no longer reads forwards: %q", store.lastSQL)
	}
	// The C selects memory_id and never uses it -- the caller supplied it.
	if strings.Contains(store.lastSQL, "memory_id, session_id") {
		t.Errorf("a column nothing reads is still selected: %q", store.lastSQL)
	}
}

func TestLookupByKeyDoesNotChooseBetweenMatches(t *testing.T) {
	// Nothing constrains a key to one memory. Picking the newest or the most
	// confident would be a retrieval policy, and this operation is the one a
	// caller reaches for when it believes the key is unique -- so it says
	// nothing about which row it wants, exactly as the C does.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(4), "the build is green", 0.9, "L2",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryLookupByKeyRequest("build-state")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryLookupByKey), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, id, content, confidence, tier, decodeErr :=
		db2contract.DecodeMemoryLookupByKeyReply(body)
	if decodeErr != nil || found != 1 || id != 4 ||
		content != "the build is green" || confidence != 0.9 || tier != "L2" {
		t.Fatalf("found = %d, id = %d, content = %q, confidence = %v, tier = %q",
			found, id, content, confidence, tier)
	}
	if strings.Contains(store.lastSQL, "ORDER BY") {
		t.Errorf("the lookup now picks a winner: %q", store.lastSQL)
	}
	// The LIMIT does not decide which row comes back -- there is no ordering --
	// it stops the server materialising every memory under the key.
	if !strings.Contains(store.lastSQL, "LIMIT 1") {
		t.Errorf("every match would be materialised: %q", store.lastSQL)
	}
}

func TestLookupByKeyReportsAMissingKey(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryLookupByKeyRequest("nothing-here")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryLookupByKey), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, id, content, confidence, tier, decodeErr :=
		db2contract.DecodeMemoryLookupByKeyReply(body)
	if decodeErr != nil || found != 0 || id != 0 || content != "" ||
		confidence != 0 || tier != "" {
		t.Fatalf("found = %d, id = %d, content = %q, confidence = %v, tier = %q",
			found, id, content, confidence, tier)
	}
}

func TestLineageAnswersTheRecordItWrote(t *testing.T) {
	// The identifier is the whole reply, so a caller can cite the lineage row
	// it just created. Zero means no record was written -- no row has it.
	store := &fakeStore{row: &fakeRow{values: []any{int64(31)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryLineageInsertRequest(
		"memory", 4, "ingest", "docs/a.md", 0.8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryLineageInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	lineageID, decodeErr := db2contract.DecodeMemoryLineageInsertReply(body)
	if decodeErr != nil || lineageID != 31 {
		t.Fatalf("lineage id = %d", lineageID)
	}
	if !strings.Contains(store.lastSQL, "RETURNING id") {
		t.Errorf("the record's identifier is not read back: %q", store.lastSQL)
	}
	// The object is named by type and identifier rather than by a foreign key,
	// because lineage is recorded for several kinds of object.
	if store.lastArgs[0] != "memory" || store.lastArgs[1] != int64(4) {
		t.Errorf("args = %v", store.lastArgs)
	}
}

func TestRelationInsertDoesNotDeduplicate(t *testing.T) {
	// memory_relations has no uniqueness constraint, unlike the entity and
	// temporal extraction tables. The difference is the schema's, so an ON
	// CONFLICT here would have nothing to name.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryRelationInsertRequest(
		4, "aimee", "depends_on", "postgres", "aimee stores memories in postgres")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryRelationInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMemoryRelationInsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if strings.Contains(store.lastSQL, "ON CONFLICT") {
		t.Errorf("the insert claims a constraint the table does not have: %q",
			store.lastSQL)
	}
	// The fact text is stored alongside the triple rather than derived from it:
	// the triple is what a graph query matches on, the text is what a person
	// reads when asking why the graph believes it.
	if store.lastArgs[4] != "aimee stores memories in postgres" {
		t.Errorf("the fact text was dropped: %v", store.lastArgs)
	}
}
