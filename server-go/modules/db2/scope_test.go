package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestDecodeScopeUnpacksTheFlagBits(t *testing.T) {
	for _, testCase := range []struct {
		flags      uint32
		active     bool
		includeAll bool
	}{
		{0, false, false},
		{1, true, false},
		{2, false, true},
		{3, true, true},
	} {
		got := DecodeScope(testCase.flags, "w", "p")
		if got.Active != testCase.active || got.IncludeAll != testCase.includeAll {
			t.Errorf("flags %d gave active=%v include-all=%v, want %v and %v",
				testCase.flags, got.Active, got.IncludeAll,
				testCase.active, testCase.includeAll)
		}
	}
}

func TestScopeFilterNumbersItsOwnPlaceholders(t *testing.T) {
	// The four scope placeholders follow whatever the caller has already spent.
	// Getting this wrong binds the caller's arguments to the scope and still
	// runs, which is the failure mode worth a test rather than a comment.
	scope := DecodeScope(3, "w", "p")
	predicate, args := scope.filter("m.id", 2)
	if len(args) != 4 {
		t.Fatalf("args = %d, want the four scope values", len(args))
	}
	for _, placeholder := range []string{"$3", "$4", "$5", "$6"} {
		if !strings.Contains(predicate, placeholder) {
			t.Errorf("the predicate does not use %s: %q", placeholder, predicate)
		}
	}
	for _, placeholder := range []string{"$1", "$2"} {
		if strings.Contains(predicate, placeholder) {
			t.Errorf("the predicate reused the caller's %s: %q", placeholder, predicate)
		}
	}
}

func TestScopeFilterAdmitsEverythingWhenInactive(t *testing.T) {
	// This is the whole reason the scope has to travel on the request. An
	// inactive scope does not mean "no rows", it means "all rows", so an
	// operation that reached this filter without being told the scope would
	// return every workspace's memories rather than none.
	scope := DecodeScope(0, "", "")
	predicate, args := scope.filter("m.id", 0)
	if !strings.Contains(predicate, "$1 = 0 OR") {
		t.Errorf("an inactive scope no longer short-circuits the filter: %q", predicate)
	}
	if args[0] != 0 {
		t.Fatalf("active = %v, want 0 for an inactive scope", args[0])
	}
}

func TestScopeFilterHonoursIncludeAll(t *testing.T) {
	scope := DecodeScope(3, "w", "p")
	predicate, args := scope.filter("m.id", 0)
	if !strings.Contains(predicate, "$2 = 1 OR") {
		t.Errorf("include-all no longer admits every row: %q", predicate)
	}
	if args[1] != 1 {
		t.Fatalf("include-all = %v, want 1", args[1])
	}
}

func TestScopeRankIsLocalFirst(t *testing.T) {
	// Project outranks workspace outranks shared. The numbers are the contract
	// with the C, which orders by this expression, so a reordering here would
	// change which memories a caller sees first without changing which they may
	// see at all -- a difference no filter test would catch.
	scope := DecodeScope(3, "w", "p")
	rank := scope.rankExpression("m.id", 0)
	project := strings.Index(rank, "THEN 3")
	workspace := strings.Index(rank, "THEN 2")
	shared := strings.Index(rank, "THEN 1")
	if project < 0 || workspace < 0 || shared < 0 {
		t.Fatalf("the three ranks are not all present: %q", rank)
	}
	if !(project < workspace && workspace < shared) {
		t.Error("the rank arms are out of order; the first match wins in a CASE")
	}
	if !strings.Contains(rank, "scope_type = 'project'") ||
		!strings.Contains(rank, "scope_type = 'workspace'") ||
		!strings.Contains(rank, "scope_value = '_global'") {
		t.Errorf("the rank no longer reads the scope tables it is defined over: %q", rank)
	}
}

func TestScopeRankTreatsAnUntaggedMemoryAsShared(t *testing.T) {
	// Rows written before scoping existed carry no tags at all. Ranking them
	// zero would hide every one of them the day scoping was switched on.
	scope := DecodeScope(3, "w", "p")
	rank := scope.rankExpression("m.id", 0)
	if !strings.Contains(rank, "NOT EXISTS") ||
		!strings.Contains(rank, "scope_type IN ('global','workspace','project')") {
		t.Errorf("an untagged memory is no longer treated as shared: %q", rank)
	}
}

func TestScopeRankDoesNotConsultIncludeAll(t *testing.T) {
	// Include-all admits rows; it does not change how local one is. Folding it
	// into the rank would reorder results for a caller who asked only to see
	// more of them.
	scope := DecodeScope(3, "w", "p")
	rank := scope.rankExpression("m.id", 0)
	if strings.Contains(rank, "$2") {
		t.Errorf("the rank reads the include-all placeholder: %q", rank)
	}
}

// scopedOperations is every ported operation whose statement is scope-filtered,
// with a request builder for each. The list is the point of the test below.
func scopedOperations() []struct {
	name  string
	stage uint32
	build func(flags uint32, workspace, project string) ([]byte, error)
} {
	return []struct {
		name  string
		stage uint32
		build func(flags uint32, workspace, project string) ([]byte, error)
	}{
		{"global_constraints", db2contract.StageGlobalConstraints,
			db2contract.EncodeGlobalConstraintsRequest},
		{"kv_section", db2contract.StageKvSection,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeKvSectionRequest(kvSectionActiveTasks, f, w, p)
			}},
		{"recall_section", db2contract.StageRecallSection,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeRecallSectionRequest(recallSectionIdentity, f, w, p)
			}},
		{"memory_candidates", db2contract.StageMemoryCandidates,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeMemoryCandidatesRequest(memoryCandidatesPrimary, f, w, p)
			}},
		{"briefing_active_entities", db2contract.StageBriefingActiveEntities,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeBriefingActiveEntitiesRequest(8, f, w, p)
			}},
		{"briefing_key_facts", db2contract.StageBriefingKeyFacts,
			db2contract.EncodeBriefingKeyFactsRequest},
		{"briefing_recent_activity", db2contract.StageBriefingRecentActivity,
			db2contract.EncodeBriefingRecentActivityRequest},
		{"memory_key_facts_provenance", db2contract.StageMemoryKeyFactsProvenance,
			db2contract.EncodeMemoryKeyFactsProvenanceRequest},
		{"lifecycle_stale_pending", db2contract.StageLifecycleStalePending,
			db2contract.EncodeLifecycleStalePendingRequest},
		{"lifecycle_newly_superseded", db2contract.StageLifecycleNewlySuperseded,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeLifecycleNewlySupersededRequest("", f, w, p)
			}},
		{"memory_search_by_pattern", db2contract.StageMemorySearchByPattern,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeMemorySearchByPatternRequest("%probe%", f, w, p)
			}},
		{"memory_episodes_search", db2contract.StageMemoryEpisodesSearch,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeMemoryEpisodesSearchRequest("probe", 8, f, w, p)
			}},
		{"relations_for_entity", db2contract.StageRelationsForEntity,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeRelationsForEntityRequest("probe", 8, f, w, p)
			}},
		{"relations_search", db2contract.StageRelationsSearch,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeRelationsSearchRequest("probe", 8, f, w, p)
			}},
		{"relations_search_as_of", db2contract.StageRelationsSearchAsOf,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeRelationsSearchAsOfRequest(
					"probe", "2026-01-01 00:00:00", 8, f, w, p)
			}},
		{"relations_supporting", db2contract.StageRelationsSupporting,
			func(f uint32, w, p string) ([]byte, error) {
				return db2contract.EncodeRelationsSupportingRequest("probe", 8, f, w, p)
			}},
	}
}

func TestEveryScopedOperationBindsTheScopeItWasGiven(t *testing.T) {
	// The defect this whole change exists to close: these sixteen read a
	// scope-filtered statement, and before the scope travelled on the request
	// there was nothing out of process for them to read it from. A statement
	// that ran without the four scope values bound would admit every
	// workspace's rows.
	for _, testCase := range scopedOperations() {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{rows: &fakeRows{}}
			handler := NewDispatchHandler(store)
			request, err := testCase.build(3, "live-workspace", "live-project")
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if !strings.Contains(store.lastSQL, "memory_scopes") {
				t.Fatalf("the statement is not scope-filtered: %q", store.lastSQL)
			}
			var boundWorkspace, boundProject bool
			for _, arg := range store.lastArgs {
				if arg == "live-workspace" {
					boundWorkspace = true
				}
				if arg == "live-project" {
					boundProject = true
				}
			}
			if !boundWorkspace || !boundProject {
				t.Fatalf("args = %v -- the scope was not bound", store.lastArgs)
			}
		})
	}
}

func TestEveryScopedOperationIsAccountedFor(t *testing.T) {
	// The catalogue and the C agree on which operations are scope-filtered, and
	// the generator now enforces that. This is the third leg: the Go port has
	// to cover the same set, so a newly scoped operation cannot be ported
	// without a scope test.
	const declaredScoped = 16
	if len(scopedOperations()) != declaredScoped {
		t.Fatalf("%d scoped operation(s) covered here; the catalogue declares %d",
			len(scopedOperations()), declaredScoped)
	}
	// And each one is really registered, so a rename cannot leave an entry here
	// pointing at nothing while the count still adds up.
	for _, entry := range scopedOperations() {
		request, err := entry.build(0, "", "")
		if err != nil {
			t.Fatalf("%s: encode: %v", entry.name, err)
		}
		if _, status := NewDispatchHandler(&fakeStore{rows: &fakeRows{}})(
			invocation(entry.stage), request); status == bus.ModuleStatusCapabilityAbsent {
			t.Errorf("%s is listed here but nothing implements it", entry.name)
		}
	}
}

func TestSectionSelectorsAreTheEnumValuesNotIotaOrder(t *testing.T) {
	// Every one of these C enums starts at 1 and the adapter casts the wire
	// value straight to the enum. An off-by-one here would not fail: it would
	// quietly serve the neighbouring section, so a caller asking for its active
	// tasks would get its recent context.
	if kvSectionActiveTasks != db2contract.KvSectionKvSectionMin ||
		kvSectionFailureWarnings != db2contract.KvSectionKvSectionMax {
		t.Errorf("the briefing sections span %d..%d, the contract says %d..%d",
			kvSectionActiveTasks, kvSectionFailureWarnings,
			db2contract.KvSectionKvSectionMin, db2contract.KvSectionKvSectionMax)
	}
	if recallSectionIdentity != db2contract.RecallSectionRecallSectionMin ||
		recallSectionOpenCommitments != db2contract.RecallSectionRecallSectionMax {
		t.Errorf("the recall sections span %d..%d, the contract says %d..%d",
			recallSectionIdentity, recallSectionOpenCommitments,
			db2contract.RecallSectionRecallSectionMin,
			db2contract.RecallSectionRecallSectionMax)
	}
	if memoryCandidatesPrimary != db2contract.MemoryCandidatesCandidateFilterMin ||
		memoryCandidatesFallback != db2contract.MemoryCandidatesCandidateFilterMax {
		t.Errorf("the candidate filters span %d..%d, the contract says %d..%d",
			memoryCandidatesPrimary, memoryCandidatesFallback,
			db2contract.MemoryCandidatesCandidateFilterMin,
			db2contract.MemoryCandidatesCandidateFilterMax)
	}

	// And every selector the contract admits maps to a statement, so no value
	// that can cross the wire falls through to an empty reply.
	for section := db2contract.KvSectionKvSectionMin; section <=
		db2contract.KvSectionKvSectionMax; section++ {
		if _, known := kvSectionBodies[section]; !known {
			t.Errorf("briefing section %d has no statement", section)
		}
	}
	for section := db2contract.RecallSectionRecallSectionMin; section <=
		db2contract.RecallSectionRecallSectionMax; section++ {
		if _, known := recallSectionBodies[section]; !known {
			t.Errorf("recall section %d has no statement", section)
		}
	}
	for filter := db2contract.MemoryCandidatesCandidateFilterMin; filter <=
		db2contract.MemoryCandidatesCandidateFilterMax; filter++ {
		if _, known := memoryCandidateBodies[filter]; !known {
			t.Errorf("candidate filter %d has no statement", filter)
		}
	}
}

func TestEverySectionStatementDiffersFromItsNeighbours(t *testing.T) {
	// A copy-paste that left two sections sharing a body would pass everything
	// above: both are in range, both have a statement, both bind the scope.
	briefing := map[string]uint32{}
	for section, chosen := range kvSectionBodies {
		if previous, duplicate := briefing[chosen.body+chosen.ordering]; duplicate {
			t.Errorf("briefing sections %d and %d share a statement", previous, section)
		}
		briefing[chosen.body+chosen.ordering] = section
	}
	recall := map[string]uint32{}
	for section, chosen := range recallSectionBodies {
		if previous, duplicate := recall[chosen.body+chosen.ordering]; duplicate {
			t.Errorf("recall sections %d and %d share a statement", previous, section)
		}
		recall[chosen.body+chosen.ordering] = section
	}
	candidates := map[string]uint32{}
	for filter, body := range memoryCandidateBodies {
		if previous, duplicate := candidates[body]; duplicate {
			t.Errorf("candidate filters %d and %d share a statement", previous, filter)
		}
		candidates[body] = filter
	}
}
