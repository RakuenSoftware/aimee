package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestMarkMergedIntoNarrowsFiveWays(t *testing.T) {
	// Each predicate limits what a session cleanup may touch, and the
	// already-merged one is what makes the operation safe to run twice: without
	// it a second pass would re-point memories merged somewhere else.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryMarkMergedIntoRequest(4, "session-1", 0.3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryMarkMergedInto), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	for _, predicate := range []string{
		"tier = 'L1'", "confidence <= $2", "kind = 'fact'",
		"source_session = $3", "merged_into = 0",
	} {
		if !strings.Contains(store.lastSQL, predicate) {
			t.Errorf("%q is gone, widening what a cleanup touches: %q",
				predicate, store.lastSQL)
		}
	}
	if len(store.lastArgs) != 3 || store.lastArgs[1] != 0.3 {
		t.Fatalf("args = %v -- the caller's ceiling is not bound", store.lastArgs)
	}
}

func TestSupersededKeysRepeatTheCountInHaving(t *testing.T) {
	// PostgreSQL evaluates HAVING before the select list and rejects an alias
	// there, so the expression has to be written twice. Referring to the alias
	// would make the statement unrunnable -- the same class of fault as the
	// DISTINCT ordering, and invisible to a fake.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySupersededKeysRequest(2, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemorySupersededKeys), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "HAVING COUNT(*) >= $1") {
		t.Errorf("the having clause refers to an alias it cannot: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "HAVING versions") {
		t.Errorf("the alias is used in HAVING; PostgreSQL will refuse it: %q",
			store.lastSQL)
	}
	// Most rewritten first: this is a maintenance read looking for churn.
	if !strings.Contains(store.lastSQL, "ORDER BY COUNT(*) DESC") {
		t.Errorf("the busiest keys no longer come first: %q", store.lastSQL)
	}
}

func TestSessionMemoriesReadForwards(t *testing.T) {
	// Ascending by id, unlike almost every other read here: a caller replaying
	// what a session did wants it in the order it happened.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeSessionMemoriesRequest("session-1", 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageSessionMemories), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id ASC") {
		t.Errorf("the session no longer reads forwards: %q", store.lastSQL)
	}
}

func TestZeroLimitsDifferBetweenTheTwoLists(t *testing.T) {
	// The contract floors these two differently, and the difference is real
	// rather than an oversight to smooth over.
	//
	// session_memories floors its limit at 1, so zero never crosses the wire:
	// a caller asking for a session's memories wants some.
	//
	// memory_superseded_keys admits zero, because it is a maintenance sweep
	// where "no limit I care about" is a sensible thing to say -- and there the
	// clamp is what turns zero into the reply's ceiling rather than an empty
	// answer.
	if db2contract.SessionMemoriesLimitMin != 1 {
		t.Fatalf("session_memories now admits a limit of %d; the clamp below is "+
			"the only thing standing between that and an empty reply",
			db2contract.SessionMemoriesLimitMin)
	}
	if _, err := db2contract.EncodeSessionMemoriesRequest("session-1", 0); err == nil {
		t.Error("a zero limit encoded for session_memories")
	}

	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySupersededKeysRequest(
		2, db2contract.MemorySupersededKeysRowLimitMin)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemorySupersededKeys), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	bound, ok := store.lastArgs[1].(int64)
	if !ok || bound != int64(db2contract.MemorySupersededKeysMaxRows) {
		t.Fatalf("limit = %v, want the reply ceiling %d -- a zero limit would "+
			"otherwise return nothing", store.lastArgs[1],
			db2contract.MemorySupersededKeysMaxRows)
	}
}

func TestProspectiveCountsNameTheirFourStates(t *testing.T) {
	// The reply has four fixed fields, and a grouped read fills only the states
	// it happens to find. These are MEMORY_PROSPECTIVE_STATE_*, and a
	// prospective memory in any other state is counted in none of them.
	store := &fakeStore{row: &fakeRow{values: []any{
		idPtr(4), idPtr(0), idPtr(9), idPtr(1),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveCountsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	armed, triggered, completed, expired, decodeErr :=
		db2contract.DecodeProspectiveCountsReply(body)
	if decodeErr != nil || armed != 4 || triggered != 0 || completed != 9 || expired != 1 {
		t.Fatalf("armed = %d, triggered = %d, completed = %d, expired = %d",
			armed, triggered, completed, expired)
	}
	for _, state := range []string{"'armed'", "'triggered'", "'completed'", "'expired'"} {
		if !strings.Contains(store.lastSQL, state) {
			t.Errorf("%s is no longer counted: %q", state, store.lastSQL)
		}
	}
}

func TestScopeTagAndLinkAreRepeatable(t *testing.T) {
	// Both are observations rather than assertions of uniqueness: re-tagging a
	// memory or re-linking two is a caller re-running work.
	for _, testCase := range []struct {
		name   string
		stage  uint32
		build  func() ([]byte, error)
		decode func([]byte) (uint32, error)
	}{
		{
			"memory_link_create",
			db2contract.StageMemoryLinkCreate,
			func() ([]byte, error) {
				return db2contract.EncodeMemoryLinkCreateRequest(4, 9, "depends_on")
			},
			db2contract.DecodeMemoryLinkCreateReply,
		},
		{
			"memory_scope_tag_insert",
			db2contract.StageMemoryScopeTagInsert,
			func() ([]byte, error) {
				return db2contract.EncodeMemoryScopeTagInsertRequest(4, "project", "aimee")
			},
			db2contract.DecodeMemoryScopeTagInsertReply,
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{execRowsAt: true, execRows: 0}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, decodeErr := testCase.decode(body)
			if decodeErr != nil || acknowledged != 1 {
				t.Fatalf("acknowledged = %d", acknowledged)
			}
			if !strings.Contains(store.lastSQL, "ON CONFLICT DO NOTHING") {
				t.Errorf("a repeat would now fail: %q", store.lastSQL)
			}
		})
	}
}

func TestTemporalRefsTieBreakOnInsertionOrder(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryTemporalRefsListRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryTemporalRefsList), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY weight DESC, id ASC") {
		t.Errorf("equally weighted references would shuffle between reads: %q",
			store.lastSQL)
	}
}
