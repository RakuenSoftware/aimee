package db2

import (
	"math"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEdgeListsSeeBothEndsOfAnEdge(t *testing.T) {
	// An edge is a fact about a pair, and the entity asked about can be either
	// end of it. Reading one direction would hide half the graph from every
	// node.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "aimee", "depends_on", "postgres", int64(9)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgesForEntityRequest("postgres", 32)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgesForEntity), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	edges, decodeErr := db2contract.DecodeEntityEdgesForEntityReply(body)
	if decodeErr != nil || len(edges) != 1 || edges[0].EdgeID != 4 ||
		edges[0].EdgeWeight != 9 {
		t.Fatalf("edges = %+v", edges)
	}
	if !strings.Contains(store.lastSQL, "WHERE source = $1") ||
		!strings.Contains(store.lastSQL, "WHERE target = $1") {
		t.Errorf("only one direction is searched: %q", store.lastSQL)
	}
	// UNION ALL, not UNION: the edge identifier is selected, so the two arms
	// cannot produce the same row and there is nothing to deduplicate.
	if !strings.Contains(store.lastSQL, "UNION ALL") {
		t.Errorf("the union now pays to deduplicate rows that cannot collide: %q",
			store.lastSQL)
	}
}

func TestTokenSearchIsCaseInsensitiveOnEveryColumn(t *testing.T) {
	// A token comes from a person rather than from the index, and nothing in it
	// says whether they meant an endpoint or a relation.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgesByTokenRequest("PostgreSQL", 32)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityEdgesByToken), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	for _, clause := range []string{
		"LOWER(source) = LOWER($1)", "LOWER(target) = LOWER($1)",
		"LOWER(relation) = LOWER($1)",
	} {
		if !strings.Contains(store.lastSQL, clause) {
			t.Errorf("missing %s: %q", clause, store.lastSQL)
		}
	}
	if !strings.Contains(store.lastSQL, "ORDER BY weight DESC") {
		t.Errorf("the heaviest edges no longer lead: %q", store.lastSQL)
	}
}

func TestEveryEdgeReadHidesInferredAndUnpublishedEdges(t *testing.T) {
	// Semantic edges are inferred rather than observed, and mixing them in
	// would let a guess look like a recorded fact. A code-projection edge is
	// visible only while its generation is.
	for _, statement := range []struct {
		name  string
		query string
	}{
		{name: "for-entity", query: entityEdgesForEntityQuery},
		{name: "by-token", query: entityEdgesByTokenQuery},
		{name: "weighted", query: entityNeighborsWeightedQuery},
		{name: "filtered", query: entityNeighborsFilteredQuery},
	} {
		t.Run(statement.name, func(t *testing.T) {
			if !strings.Contains(statement.query, "edge_class <> 'semantic'") {
				t.Errorf("inferred edges would surface: %q", statement.query)
			}
			if !strings.Contains(statement.query, "cpg.state='visible'") {
				t.Errorf("edges from a hidden generation would surface: %q",
					statement.query)
			}
		})
	}
}

func TestUtilityFadesWithTheAgeOfItsStamp(t *testing.T) {
	// A connection that mattered once and has not been confirmed since should
	// fade rather than stand forever.
	stamp := "2026-01-01 00:00:00"
	halfLife := entityUtilityHalfLifeDays
	if got := decayedUtility(2, stamp, &halfLife); math.Abs(got-1) > 1e-9 {
		t.Errorf("one half-life old scored %v, want half", got)
	}
	fresh := 0.0
	if got := decayedUtility(2, stamp, &fresh); math.Abs(got-2) > 1e-9 {
		t.Errorf("a fresh stamp scored %v, want the raw score", got)
	}
	// Clamped to the same bounds a bump is allowed to move a score between, so
	// a decayed score can never exceed one that was never decayed.
	if got := decayedUtility(50, stamp, &fresh); got != entityUtilityBound {
		t.Errorf("an out-of-range score scored %v, want the bound", got)
	}
	if got := decayedUtility(-50, stamp, &fresh); got != -entityUtilityBound {
		t.Errorf("an out-of-range negative scored %v, want the bound", got)
	}
}

func TestUtilitySentinelsMeanOppositeThings(t *testing.T) {
	// A score with no stamp is a row written before the column existed: it is
	// worth something and cannot say when, and fading it to nothing would throw
	// away what is known because of what is not.
	if got := decayedUtility(2, "", nil); got != 2 {
		t.Errorf("an unstamped score scored %v, want the raw score", got)
	}
	// A stamp that will not parse is the same case: the C's parse fails and it
	// keeps the raw score.
	if got := decayedUtility(2, "yesterday afternoon", nil); got != 2 {
		t.Errorf("an unparseable stamp scored %v, want the raw score", got)
	}
	// The epoch year is the opposite sentinel -- a backfill that could not find
	// a real time wrote it deliberately.
	age := 0.0
	if got := decayedUtility(2, "1970-01-01 00:00:00", &age); got != 0 {
		t.Errorf("the epoch sentinel scored %v, want zero", got)
	}
	// A zero score has nothing to decay, whatever its age.
	if got := decayedUtility(0, "2026-01-01 00:00:00", &age); got != 0 {
		t.Errorf("a zero score scored %v", got)
	}
}

func TestWeightedNeighboursScoreOnlyWhenAsked(t *testing.T) {
	// A caller that is not scoring should not be handed a number it did not ask
	// to be charged for.
	rows := [][]any{
		{"postgres", int64(9), 2.0, "2026-01-01 00:00:00", ptrFloat(0)},
	}
	store := &fakeStore{rows: &fakeRows{values: rows}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNeighborsWeightedRequest("aimee", 32, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityNeighborsWeighted), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	neighbours, decodeErr := db2contract.DecodeEntityNeighborsWeightedReply(body)
	if decodeErr != nil || len(neighbours) != 1 {
		t.Fatalf("neighbours = %+v", neighbours)
	}
	if neighbours[0].UtilityScore != 2 || neighbours[0].EffectiveUtility != 0 {
		t.Fatalf("neighbour = %+v", neighbours[0])
	}

	store = &fakeStore{rows: &fakeRows{values: rows}}
	handler = NewDispatchHandler(store)
	request, err = db2contract.EncodeEntityNeighborsWeightedRequest("aimee", 32, 1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status = handler(invocation(db2contract.StageEntityNeighborsWeighted), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	neighbours, decodeErr = db2contract.DecodeEntityNeighborsWeightedReply(body)
	if decodeErr != nil || len(neighbours) != 1 || neighbours[0].EffectiveUtility != 2 {
		t.Fatalf("neighbour = %+v", neighbours)
	}
	// The age is computed where the stamp was written. The C parses it in C and
	// compares against the local clock, so the two agree only when the database
	// and the caller share a timezone.
	if !strings.Contains(store.lastSQL, "CURRENT_TIMESTAMP - touched_at::timestamp") {
		t.Errorf("the age is no longer computed by the database: %q", store.lastSQL)
	}
	// The guard is what stops an unparseable stamp failing the whole read
	// rather than the one row.
	if !strings.Contains(store.lastSQL, "touched_at ~ '^[0-9]{4}") {
		t.Errorf("a malformed stamp would fail the statement: %q", store.lastSQL)
	}
}

func TestFilteredNeighboursOrderOnlyWhenAsked(t *testing.T) {
	// Heaviest first costs a sort over the whole union, and a caller walking
	// every neighbour anyway does not need it.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNeighborsFilteredRequest(
		"aimee", "depends_on", "depends_on", 0, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityNeighborsFiltered), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "ORDER BY") {
		t.Errorf("the sort was paid for unasked: %q", store.lastSQL)
	}
	// One relation is matched by passing it twice, which IN collapses -- that
	// is what lets one statement serve both of the C's two.
	if !strings.Contains(store.lastSQL, "relation IN ($2, $3)") {
		t.Errorf("the relation filter changed shape: %q", store.lastSQL)
	}

	store = &fakeStore{rows: &fakeRows{}}
	handler = NewDispatchHandler(store)
	request, err = db2contract.EncodeEntityNeighborsFilteredRequest(
		"aimee", "depends_on", "used_by", 1, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityNeighborsFiltered), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY weight DESC LIMIT $4") {
		t.Errorf("the sort is missing or misplaced: %q", store.lastSQL)
	}
}
