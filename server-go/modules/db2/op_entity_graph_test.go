package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEveryGraphReadExcludesSemanticEdgesAndHiddenProjections(t *testing.T) {
	// Semantic edges are inferred from similarity rather than asserted, so a
	// walk over them follows resemblance and reports it as a relationship. The
	// projection check keeps code edges out unless their generation is visible
	// and their project current.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		// How many times the projection predicate should appear: once per
		// branch, since an edge visible on one side and not the other would be
		// a graph that disagrees with itself.
		projections int
	}{
		{
			"entity_neighbors",
			db2contract.StageEntityNeighbors,
			func() ([]byte, error) {
				return db2contract.EncodeEntityNeighborsRequest("postgres", 8)
			},
			2,
		},
		{
			"entity_outbound_neighbors",
			db2contract.StageEntityOutboundNeighbors,
			func() ([]byte, error) {
				return db2contract.EncodeEntityOutboundNeighborsRequest("postgres", 8)
			},
			1,
		},
		{
			"entity_top_targets",
			db2contract.StageEntityTopTargets,
			func() ([]byte, error) {
				return db2contract.EncodeEntityTopTargetsRequest("postgres", "depends_on")
			},
			1,
		},
		{
			"entity_top_partners",
			db2contract.StageEntityTopPartners,
			func() ([]byte, error) {
				return db2contract.EncodeEntityTopPartnersRequest("postgres", "depends_on")
			},
			2,
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
			if strings.Count(store.lastSQL, "edge_class <> 'semantic'") !=
				testCase.projections {
				t.Errorf("semantic edges are not excluded from every branch: %q",
					store.lastSQL)
			}
			if strings.Count(store.lastSQL, "cpg.state='visible'") != testCase.projections {
				t.Errorf("the projection visibility check is missing from a branch: %q",
					store.lastSQL)
			}
		})
	}
}

func TestNeighborsKeepBothDirectionsSeparate(t *testing.T) {
	// UNION ALL, not UNION. An entity connected both ways appears twice with
	// its two weights: the edges are separate assertions, and folding them
	// would hide that the relationship is reciprocated.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("kafka"), idPtr(3)}, {ptr("kafka"), idPtr(5)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNeighborsRequest("postgres", 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityNeighbors), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeEntityNeighborsReply(body)
	if decodeErr != nil || len(found) != 2 {
		t.Fatalf("rows = %+v -- a reciprocated edge was folded into one", found)
	}
	if !strings.Contains(store.lastSQL, "UNION ALL") ||
		strings.Contains(store.lastSQL, "UNION\n") {
		t.Errorf("the union deduplicates: %q", store.lastSQL)
	}
}

func TestNeighborsDefaultTheLimitRatherThanEmptying(t *testing.T) {
	// A caller that omits a limit wants a neighbourhood, not nothing. The C
	// defaults it to fifty before building the statement.
	store := &fakeStore{rows: &fakeRows{}}
	if _, status := entityNeighbors(t.Context(), store, mustEncode(t,
		func() ([]byte, error) {
			return db2contract.EncodeEntityNeighborsRequest("postgres",
				db2contract.EntityNeighborsLimitMin)
		})); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 2 {
		t.Fatalf("args = %v", store.lastArgs)
	}
	if limit, ok := store.lastArgs[1].(int64); !ok || limit <= 0 {
		t.Fatalf("limit = %v, want a positive default", store.lastArgs[1])
	}
	// And never above what the reply can carry.
	if limit := store.lastArgs[1].(int64); limit > int64(db2contract.EntityNeighborsMaxRows) {
		t.Fatalf("limit = %d exceeds the reply ceiling %d",
			limit, db2contract.EntityNeighborsMaxRows)
	}
}

func TestAggregatingReadsMatchTheEntityCaseInsensitively(t *testing.T) {
	// These answer a question a person asked about a named thing, where the
	// neighbour reads walk a graph from a node whose spelling the caller
	// already holds. The difference is the C's and it is kept.
	for _, testCase := range []struct {
		name             string
		stage            uint32
		build            func() ([]byte, error)
		caseInsensitive  bool
		expectedGrouping string
	}{
		{
			"entity_top_targets",
			db2contract.StageEntityTopTargets,
			func() ([]byte, error) {
				return db2contract.EncodeEntityTopTargetsRequest("Postgres", "depends_on")
			},
			true,
			"GROUP BY target",
		},
		{
			"entity_top_partners",
			db2contract.StageEntityTopPartners,
			func() ([]byte, error) {
				return db2contract.EncodeEntityTopPartnersRequest("Postgres", "depends_on")
			},
			true,
			"GROUP BY partner",
		},
		{
			"entity_neighbors",
			db2contract.StageEntityNeighbors,
			func() ([]byte, error) {
				return db2contract.EncodeEntityNeighborsRequest("Postgres", 8)
			},
			false,
			"",
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
			folded := strings.Contains(store.lastSQL, "LOWER(")
			if folded != testCase.caseInsensitive {
				t.Errorf("case folding = %v, want %v: %q",
					folded, testCase.caseInsensitive, store.lastSQL)
			}
			if testCase.expectedGrouping != "" {
				if !strings.Contains(store.lastSQL, testCase.expectedGrouping) {
					t.Errorf("the weights are no longer summed per node: %q", store.lastSQL)
				}
				if !strings.Contains(store.lastSQL, "SUM(") {
					t.Errorf("this is a listing rather than a ranking: %q", store.lastSQL)
				}
			}
		})
	}
}

func TestTopPartnersSumAcrossBothDirections(t *testing.T) {
	// The subquery is the difference from top_targets: both directions are
	// collected first and summed over the union, so a partner that is both
	// pointed at and pointing back has its weights added rather than ranked
	// twice.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityTopPartnersRequest("postgres", "depends_on")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityTopPartners), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "UNION ALL") ||
		!strings.Contains(store.lastSQL, ") sub GROUP BY partner") {
		t.Errorf("the two directions are not summed together: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "LOWER(source) = LOWER($1)") ||
		!strings.Contains(store.lastSQL, "LOWER(target) = LOWER($1)") {
		t.Errorf("the entity is not matched on both sides: %q", store.lastSQL)
	}
}
