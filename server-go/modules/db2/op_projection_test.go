package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestBuildVersionIsNotAPlausibleVersion(t *testing.T) {
	// Every generation records it, and the Go build does not stamp it yet. The
	// default must be something nobody mistakes for a real extractor version,
	// because the snapshot-diff route refuses to compare across versions and a
	// wrong-but-plausible one would let it compare graphs it should not.
	if BuildVersion == "" {
		t.Fatal("BuildVersion is empty; a generation would record nothing")
	}
	for _, plausible := range []string{"0.0.0", "dev", "1.0.0", "unknown"} {
		if BuildVersion == plausible {
			t.Fatalf("BuildVersion is %q, which reads like a real version", plausible)
		}
	}
}

func TestProjectionGenerationCreateInsertsFromSelect(t *testing.T) {
	// INSERT ... SELECT, so a project that is not current inserts nothing: the
	// generation cannot exist for a project that cannot receive it.
	store := &fakeStore{row: &fakeRow{values: []any{int64(4)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionGenerationCreateRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectionGenerationCreate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	generation, decodeErr := db2contract.DecodeProjectionGenerationCreateReply(body)
	if decodeErr != nil || generation != 4 {
		t.Fatalf("generation = %d", generation)
	}
	if !strings.Contains(store.lastSQL, "FROM projects p WHERE p.name=$1") ||
		!strings.Contains(store.lastSQL, "lifecycle_state='current'") {
		t.Error("the insert is not gated on a current project")
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != BuildVersion {
		t.Fatalf("args = %v -- the build version is not stamped", store.lastArgs)
	}
}

func TestProjectionGenerationCreateAnswersZeroForAnAbsentProject(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeProjectionGenerationCreateRequest("not-current")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectionGenerationCreate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	generation, decodeErr := db2contract.DecodeProjectionGenerationCreateReply(body)
	if decodeErr != nil || generation != 0 {
		t.Fatalf("generation = %d, want 0 rather than an identifier that cannot publish",
			generation)
	}
}

func TestGenerationPublishSupersedesThenPublishesThenStamps(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeGenerationPublishRequest(4, "replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageGenerationPublish), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeGenerationPublishReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) != 3 {
		t.Fatalf("statements = %d, want supersede, publish and stamp", len(store.sqlLog))
	}
	if !strings.Contains(store.sqlLog[0], "'superseded'") ||
		!strings.Contains(store.sqlLog[1], "'visible'") ||
		!strings.Contains(store.sqlLog[2], "entity_edges") {
		t.Error("the three statements are not the ones expected, or are out of order")
	}
}

func TestGenerationPublishRollsBackWhenNothingWasPublishable(t *testing.T) {
	// The supersede runs first, so a publish that matches no row has already
	// retired the project's visible generation. Rolling back is what stops a
	// stale generation from removing the last visible graph.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeGenerationPublishRequest(4, "replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageGenerationPublish), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeGenerationPublishReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
	// The stamp must not have run: the generation is not visible.
	if len(store.sqlLog) != 2 {
		t.Fatalf("statements = %d, want the supersede and the failed publish only",
			len(store.sqlLog))
	}
}

func TestGenerationAbortOnlyTouchesAPendingGeneration(t *testing.T) {
	// A visible generation cannot be aborted, which is what stops a late failure
	// from retracting a graph that has already been published.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeGenerationAbortRequest(4, "extractor crashed")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageGenerationAbort), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "state = 'pending'") {
		t.Error("abort is not restricted to a pending generation")
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "extractor crashed" {
		t.Fatalf("args = %v -- the reason is not recorded", store.lastArgs)
	}
}

func TestGenerationSetSourceHashIsNotStateRestricted(t *testing.T) {
	// Unlike its neighbours: it can be set on a generation that has already
	// published. Pinned because adding a state predicate would look like making
	// it consistent with the others.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeGenerationSetSourceHashRequest(4, "abc123")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageGenerationSetSourceHash), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "state") {
		t.Error("the update gained a state predicate the C statement does not have")
	}
}

func TestProjectDeleteNamesOnlyTheProjectRow(t *testing.T) {
	// What it takes with it is the schema's business -- the referencing tables
	// carry their own cascade rules. A statement that deleted from them here
	// would be a second, competing definition of what a project owns.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectDeleteRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageProjectDelete), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d, want one", store.execCalls)
	}
	if !strings.Contains(store.lastSQL, "DELETE FROM projects WHERE name = $1") {
		t.Errorf("unexpected statement: %q", store.lastSQL)
	}
}
