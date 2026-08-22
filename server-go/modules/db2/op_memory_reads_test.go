package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestUnitEdgeExistsAsksBothDirections(t *testing.T) {
	// The table stores a direction and this read ignores it. A caller checking
	// whether two units are already linked must not create a second edge just
	// because the existing one points the other way.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(1)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeUnitEdgeExistsRequest(4, 9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageUnitEdgeExists), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	connected, decodeErr := db2contract.DecodeUnitEdgeExistsReply(body)
	if decodeErr != nil || connected != 1 {
		t.Fatalf("connected = %d", connected)
	}
	if !strings.Contains(store.lastSQL, "src_unit_id = $1 AND dst_unit_id = $2") ||
		!strings.Contains(store.lastSQL, "src_unit_id = $2 AND dst_unit_id = $1") {
		t.Errorf("only one direction is checked: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 {
		t.Fatalf("args = %v, want the pair bound once each", store.lastArgs)
	}
}

func TestExistenceReadsStopAtTheFirstMatch(t *testing.T) {
	// The question is existence. Counting would invite a caller to start
	// reading meaning into the number.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
	}{
		{
			"scene_member_exists",
			db2contract.StageSceneMemberExists,
			func() ([]byte, error) { return db2contract.EncodeSceneMemberExistsRequest(4, 9) },
		},
		{
			"unit_edge_exists",
			db2contract.StageUnitEdgeExists,
			func() ([]byte, error) { return db2contract.EncodeUnitEdgeExistsRequest(4, 9) },
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
			if !strings.Contains(store.lastSQL, "SELECT 1 FROM") ||
				!strings.Contains(store.lastSQL, "LIMIT 1") {
				t.Errorf("the read no longer stops at the first match: %q", store.lastSQL)
			}
			if strings.Contains(store.lastSQL, "COUNT(") {
				t.Errorf("existence became a count: %q", store.lastSQL)
			}
		})
	}
}

func TestSceneMemberAbsenceIsNotAnError(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeSceneMemberExistsRequest(4, 9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageSceneMemberExists), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	member, decodeErr := db2contract.DecodeSceneMemberExistsReply(body)
	if decodeErr != nil || member != 0 {
		t.Fatalf("member = %d, want 0", member)
	}
}

func TestMemoriesByKeyMatchesExactly(t *testing.T) {
	// No normalisation and no case folding, unlike the entity reads. The caller
	// is scanning one key's duplicates for contradictions, so a key differing by
	// a character is a different key and a different question.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{idPtr(4), ptr("first")}, {idPtr(9), ptr("second")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoriesByKeyRequest("Deploy:Target")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoriesByKey), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoriesByKeyReply(body)
	if decodeErr != nil || len(found) != 2 {
		t.Fatalf("rows = %+v -- several rows for one key is the expected case", found)
	}
	if strings.Contains(store.lastSQL, "LOWER(") {
		t.Errorf("the key match was made case-insensitive: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != "Deploy:Target" {
		t.Fatalf("args = %v -- the key was altered", store.lastArgs)
	}
}

func TestConfidenceByKeySeparatesFoundFromZero(t *testing.T) {
	// Zero is a real confidence. A memory nobody believes and a key nothing
	// holds would otherwise be the same answer, and they lead to opposite
	// decisions.
	t.Run("a memory nobody believes", func(t *testing.T) {
		zero := 0.0
		store := &fakeStore{row: &fakeRow{values: []any{&zero}}}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeMemoryConfidenceByKeyRequest("known")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageMemoryConfidenceByKey), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, confidence, decodeErr := db2contract.DecodeMemoryConfidenceByKeyReply(body)
		if decodeErr != nil || found != 1 || confidence != 0 {
			t.Fatalf("found = %d, confidence = %v", found, confidence)
		}
	})

	t.Run("a key nothing holds", func(t *testing.T) {
		handler := NewDispatchHandler(&fakeStore{})
		request, err := db2contract.EncodeMemoryConfidenceByKeyRequest("unknown")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageMemoryConfidenceByKey), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, confidence, decodeErr := db2contract.DecodeMemoryConfidenceByKeyReply(body)
		if decodeErr != nil || found != 0 || confidence != 0 {
			t.Fatalf("found = %d, confidence = %v", found, confidence)
		}
	})
}

func TestMemoryScopesListIsLocalFirst(t *testing.T) {
	// The same precedence the visibility rank uses. A caller rendering a
	// memory's scopes shows the narrowest first, because that is the one that
	// explains why they can see it.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryScopesListRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryScopesList), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	project := strings.Index(store.lastSQL, "WHEN 'project' THEN 0")
	workspace := strings.Index(store.lastSQL, "WHEN 'workspace' THEN 1")
	global := strings.Index(store.lastSQL, "WHEN 'global' THEN 2")
	if project < 0 || workspace < 0 || global < 0 {
		t.Fatalf("the precedence is gone: %q", store.lastSQL)
	}
	if !(project < workspace && workspace < global) {
		t.Error("the precedence no longer runs project, workspace, global")
	}
}

func TestTierKindCountsCoverEveryMemory(t *testing.T) {
	// Including archived and superseded ones: this is the shape of the store
	// rather than the shape of what is visible. Adding a lifecycle filter would
	// quietly turn it into a different report.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("L2"), ptr("fact"), idPtr(12)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryTierKindCountsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryTierKindCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoryTierKindCountsReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].MemoryCount != 12 {
		t.Fatalf("rows = %+v", found)
	}
	if strings.Contains(store.lastSQL, "lifecycle_state") {
		t.Errorf("the count was narrowed to live memories: %q", store.lastSQL)
	}
}

func TestSceneMembershipsImposeNoOrdering(t *testing.T) {
	// A caller wanting the strongest sorts for itself; the C promises nothing.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySceneMembershipsRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemorySceneMemberships), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "ORDER BY") {
		t.Errorf("an ordering the C never promised was added: %q", store.lastSQL)
	}
}
