package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestProfileRefreshKeepsTheNameAndTheBirthday(t *testing.T) {
	// The conflict clause names three columns and leaves two alone. created_at
	// stays so a profile refreshed a hundred times still says when it was first
	// built, and canonical_name stays because a name does not change on account
	// of being observed again -- a refresh carrying a worse one would overwrite
	// a better one.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityProfileUpsertRequest(
		"entity:postgres", "PostgreSQL", 12, `{"kind":"tool"}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityProfileUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeEntityProfileUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	update := store.lastSQL[strings.Index(store.lastSQL, "DO UPDATE SET"):]
	if strings.Contains(update, "created_at") {
		t.Errorf("a refresh rewrites the creation time: %q", update)
	}
	if strings.Contains(update, "canonical_name") {
		t.Errorf("a refresh rewrites the canonical name: %q", update)
	}
	for _, column := range []string{"observation_count", "card_json", "last_refreshed"} {
		if !strings.Contains(update, column) {
			t.Errorf("a refresh no longer updates %s: %q", column, update)
		}
	}
}

func TestProfileWithoutANameFallsBackToItsIdentifier(t *testing.T) {
	// A profile written with an empty name renders as a blank row. The
	// identifier is at least something a person can recognise.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityProfileUpsertRequest(
		"entity:postgres", "", 1, "{}")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityProfileUpsert), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[1] != "entity:postgres" {
		t.Errorf("canonical name = %v, want the identifier", store.lastArgs[1])
	}
}

func TestAliasUpsertKeysOnThePairing(t *testing.T) {
	// One alias can point at several nodes and one node can be reached by
	// several aliases, so the conflict target is the pair. Keying on the alias
	// alone would make every new node for a known alias overwrite the last.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNodeAliasUpsertRequest(
		"pg", "node:postgres", "abbreviation", "aimee", 7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityNodeAliasUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeEntityNodeAliasUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (alias, node_key)") {
		t.Errorf("the conflict target is no longer the pairing: %q", store.lastSQL)
	}
	// The generation is what makes an alias forgettable: a sweep drops aliases
	// last seen before the current one without knowing which scan wrote them.
	if !strings.Contains(store.lastSQL,
		"last_seen_generation_id = excluded.last_seen_generation_id") {
		t.Errorf("a re-seen alias would keep an old generation: %q", store.lastSQL)
	}
	if store.lastArgs[4] != int64(7) {
		t.Errorf("generation = %v", store.lastArgs[4])
	}
}

func TestTopTriplesDeduplicateOnTheTripleNotTheEdge(t *testing.T) {
	// The zero in the select list is what makes the DISTINCT mean anything.
	// Selecting the real id would make every row distinct by definition, and
	// the same triple recorded by several edges would come back once per edge.
	if !strings.Contains(entityTopTriplesQuery, "SELECT DISTINCT 0 AS id") {
		t.Errorf("the identifier is no longer neutralised: %q", entityTopTriplesQuery)
	}
	// weight is both selected and ordered by. PostgreSQL rejects a SELECT
	// DISTINCT ordered by a column it does not select, so dropping it from the
	// select list would make the statement unrunnable rather than merely
	// different.
	if !strings.Contains(entityTopTriplesQuery, "target, weight") ||
		!strings.Contains(entityTopTriplesQuery, "ORDER BY weight DESC") {
		t.Errorf("the ordering column is not selected: %q", entityTopTriplesQuery)
	}
	if !strings.Contains(entityTopTriplesQuery, "edge_class <> 'semantic'") {
		t.Errorf("inferred edges would read as observed ones: %q", entityTopTriplesQuery)
	}
	if !strings.Contains(entityTopTriplesQuery, "cpg.state='visible'") {
		t.Errorf("edges from a hidden projection generation would surface: %q",
			entityTopTriplesQuery)
	}
}

func TestTopTriplesReadTheirRows(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(0), "aimee", "depends_on", "postgres", int64(9)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityTopTriplesRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityTopTriples), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	triples, decodeErr := db2contract.DecodeEntityTopTriplesReply(body)
	if decodeErr != nil || len(triples) != 1 {
		t.Fatalf("triples = %+v", triples)
	}
	if triples[0].EdgeSource != "aimee" || triples[0].EdgeRelation != "depends_on" ||
		triples[0].EdgeTarget != "postgres" || triples[0].EdgeWeight != 9 {
		t.Fatalf("triple = %+v", triples[0])
	}
	// Every row carries edge zero, because the statement selects a literal
	// rather than the column. A caller cannot follow the identifier back to an
	// edge, and that is the price of deduplicating triples.
	if triples[0].EdgeID != 0 {
		t.Errorf("edge id = %d, want the neutralised zero", triples[0].EdgeID)
	}
}
