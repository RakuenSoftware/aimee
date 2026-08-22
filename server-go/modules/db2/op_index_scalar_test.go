package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestVisibleReadsRequireACurrentProject(t *testing.T) {
	// A generation left visible on a retired project is not visible. Both reads
	// join projects rather than trusting the generation's own state, because
	// reading it would resurrect a retired project's graph.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
	}{
		{
			"visible_source_hash",
			db2contract.StageVisibleSourceHash,
			func() ([]byte, error) {
				return db2contract.EncodeVisibleSourceHashRequest("replay-project")
			},
		},
		{
			"projection_visible_id",
			db2contract.StageProjectionVisibleID,
			func() ([]byte, error) {
				return db2contract.EncodeProjectionVisibleIDRequest("replay-project")
			},
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if !strings.Contains(store.lastSQL, "p.lifecycle_state='current'") {
				t.Error("the read does not require a current project")
			}
			if !strings.Contains(store.lastSQL, "g.state='visible'") {
				t.Error("the read does not require a visible generation")
			}
		})
	}
}

func TestVisibleSourceHashAbsenceIsEmpty(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeVisibleSourceHashRequest("never-published")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageVisibleSourceHash), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hash, decodeErr := db2contract.DecodeVisibleSourceHashReply(body)
	if decodeErr != nil || hash != "" {
		t.Fatalf("hash = %q", hash)
	}
}

func TestEntityProfileCardMatchesEitherCase(t *testing.T) {
	// LOWER on both sides, so a caller need not know which spelling was stored
	// -- and two entities differing only in case are one entity to this read.
	store := &fakeStore{row: &fakeRow{values: []any{(*string)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityProfileCardRequest("Postgres")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityProfileCard), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Count(store.lastSQL, "LOWER(") != 2 {
		t.Errorf("the comparison is not case-insensitive on both sides: %q", store.lastSQL)
	}
}

func TestCodeFileHashIsBoundToTheCurrentGeneration(t *testing.T) {
	// A file that existed in an older scan and not in this one answers empty:
	// the hash of what is indexed now, not the last hash anybody recorded.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("abc123")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeFileHashRequest("replay-project", "src/main.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeFileHash), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hash, decodeErr := db2contract.DecodeCodeFileHashReply(body)
	if decodeErr != nil || hash != "abc123" {
		t.Fatalf("hash = %q, err = %v", hash, decodeErr)
	}
	if !strings.Contains(store.lastSQL, "f.generation = p.current_generation") {
		t.Error("the read is not bound to the current generation")
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "src/main.c" {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestMinhashDeleteClearsBothTablesInOneTransaction(t *testing.T) {
	// Buckets that name signatures which no longer exist are not a half-cleared
	// index, they are a wrong one, and the next similarity read would use them.
	// The C implementation runs the two deletes unwrapped.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMinhashDeleteCurrentGenerationRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMinhashDeleteCurrentGeneration), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMinhashDeleteCurrentGenerationReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if store.execCalls != 2 {
		t.Fatalf("statements = %d, want both deletes", store.execCalls)
	}
	if !strings.Contains(store.sqlLog[0], "kb_minhash_signatures") ||
		!strings.Contains(store.sqlLog[1], "kb_lsh_buckets") {
		t.Error("the two deletes are not the ones expected, or are out of order")
	}
}

func TestMinhashDeleteRollsBackWhenTheSecondDeleteFails(t *testing.T) {
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMinhashDeleteCurrentGenerationRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMinhashDeleteCurrentGeneration), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMinhashDeleteCurrentGenerationReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
}

func TestDeletesScopeThemselvesToTheCurrentGeneration(t *testing.T) {
	// Neither takes a generation as an argument. A caller cannot ask them to
	// clear a published one, which is the point: these run before a rescan
	// repopulates.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFileIndexDeleteCurrentGenerationRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageFileIndexDeleteCurrentGeneration), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "SELECT current_generation FROM projects") {
		t.Error("the delete does not scope itself to the current generation")
	}
	if len(store.lastArgs) != 1 {
		t.Fatalf("args = %v -- the generation must not come from the caller", store.lastArgs)
	}
}

func ptr(value string) *string { return &value }
