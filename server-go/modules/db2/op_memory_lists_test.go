package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestMatchErrorKeysComparesTheErrorAgainstTheKey(t *testing.T) {
	// Backwards from the usual direction: the error text is the subject and the
	// memory key is the pattern. Reversing it would match a key that contains
	// the error, which is not the same question and is almost always empty.
	store := &fakeStore{rows: &fakeRows{values: [][]any{{idPtr(7)}, {idPtr(9)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMatchErrorKeysRequest("could not open socket")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMatchErrorKeys), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMatchErrorKeysReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if len(found) != 2 || found[0].MemoryID != 7 || found[1].MemoryID != 9 {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, `$1 LIKE '%' || LOWER(key) || '%'`) {
		t.Errorf("the comparison is not error-contains-key: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "confidence > 0.3") ||
		!strings.Contains(store.lastSQL, "tier IN ('L1', 'L2')") {
		t.Error("the read is not restricted to confident L1/L2 memories")
	}
}

func TestMemoryIdsByUpdatedTreatsZeroAsNoLimit(t *testing.T) {
	// The C builds its statement with a LIMIT clause only when the limit is
	// positive. NULLIF is how one statement covers both branches, since LIMIT
	// NULL is no limit; a plain LIMIT $1 would return nothing for zero.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryIdsByUpdatedRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryIdsByUpdated), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "LIMIT NULLIF($1, 0)") {
		t.Errorf("a zero limit would truncate to nothing: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY updated_at DESC") {
		t.Error("the ordering is not most-recently-updated first")
	}
}

func TestMemoryDependsOnKeysStopsAtThree(t *testing.T) {
	// Three is stated twice in the C -- the caller asks for it and the
	// implementation clamps to it -- and the request carries no limit, so the
	// number has to live in the port. The reply's own ceiling is 256, which is
	// what this would silently become.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("alpha")}, {ptr("beta")}, {ptr("gamma")}, {ptr("delta")}, {ptr("epsilon")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryDependsOnKeysRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryDependsOnKeys), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoryDependsOnKeysReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if len(found) != 3 {
		t.Fatalf("rows = %d, want 3 however many the database offers", len(found))
	}
	// And the database is asked for three as well, rather than being read and
	// discarded.
	if len(store.lastArgs) != 2 || store.lastArgs[1] != dependsOnKeysCeiling {
		t.Fatalf("args = %v -- the LIMIT is not bound to the ceiling", store.lastArgs)
	}
}

func TestMemoryRelationDatesMatchesRelationsCaseSensitively(t *testing.T) {
	// Both spellings of OCCURRED_AT are listed and only one of valid_from.
	// Pinned rather than tidied: widening it with LOWER() would pull in
	// relations this read has never returned.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryRelationDatesRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryRelationDates), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL,
		"mr.relation IN ('OCCURRED_AT', 'occurred_at', 'valid_from')") {
		t.Errorf("the relation list changed: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "LOWER(mr.relation)") {
		t.Error("the relation match was widened; that changes which rows come back")
	}
}

func TestSessionReadsAgreeOnTheirRowSet(t *testing.T) {
	// Two operations returning parallel lists, which a caller pairs by index.
	// They only line up while both statements select the same rows in the same
	// order, so the filter and the ordering are pinned together.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
	}{
		{
			"memory_session_content",
			db2contract.StageMemorySessionContent,
			func() ([]byte, error) {
				return db2contract.EncodeMemorySessionContentRequest("session-1")
			},
		},
		{
			"memory_session_created_at",
			db2contract.StageMemorySessionCreatedAt,
			func() ([]byte, error) {
				return db2contract.EncodeMemorySessionCreatedAtRequest("session-1")
			},
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{rows: &fakeRows{}}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if !strings.Contains(store.lastSQL, "tier = 'L1' AND source_session = $1") {
				t.Errorf("the filter differs from its partner's: %q", store.lastSQL)
			}
			if !strings.Contains(store.lastSQL, "ORDER BY created_at ASC") {
				t.Errorf("the ordering differs from its partner's: %q", store.lastSQL)
			}
		})
	}
}

func TestSingleColumnReadsSpellNullAsAbsent(t *testing.T) {
	// A NULL column is not a failure to any of these reads. The shared helpers
	// scan into pointers so that a NULL arrives as empty rather than as a scan
	// error that would fail the whole read.
	store := &fakeStore{rows: &fakeRows{values: [][]any{{(*string)(nil)}, {ptr("2026-01-01")}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryRelationDatesRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryRelationDates), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoryRelationDatesReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if len(found) != 2 || found[0].RelationDate != "" || found[1].RelationDate != "2026-01-01" {
		t.Fatalf("rows = %+v", found)
	}
}

func TestUnitIdsForMemoryImposesNoOrdering(t *testing.T) {
	// The C promises none. Adding one here would be inventing a guarantee that
	// callers would then depend on.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeUnitIdsForMemoryRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageUnitIdsForMemory), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "ORDER BY") {
		t.Errorf("an ordering the C never promised was added: %q", store.lastSQL)
	}
}

// idPtr scripts a non-NULL integer column. The shared readIntColumn helper
// scans into a pointer, because a NULL is an absent value rather than a failed
// read; a fixture holding a bare int64 therefore arrives as NULL, and the
// generated encoders floor an identifier at 1, so it would be rejected.
func idPtr(value int64) *int64 { return &value }
