package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestScopedTermSearchFallsBackOnlyWhenItFindsNothing(t *testing.T) {
	// Exact first, then prefix, then substring, and each tier fires only when
	// the one before it found nothing. Running the substring tier first would
	// bury an exact prefix match under everything that merely contains the
	// word.
	store := &fakeStore{rowsQueue: []*fakeRows{
		{},
		{},
		{values: [][]any{{int64(10), int64(12), "aimee", "src/a.c", "definition"}}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTermFindInProjectRequest("", "qdrant")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTermFindInProject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hits, decodeErr := db2contract.DecodeTermFindInProjectReply(body)
	if decodeErr != nil || len(hits) != 1 || hits[0].HitFilePath != "src/a.c" {
		t.Fatalf("hits = %+v", hits)
	}
	if len(store.sqlLog) != 3 {
		t.Fatalf("statements = %d, want exact then two patterns", len(store.sqlLog))
	}
	// The path filters use LIKE too, so the check is on the name predicate.
	if strings.Contains(store.sqlLog[0], "t.name LIKE") {
		t.Errorf("the first search is not exact: %q", store.sqlLog[0])
	}
	if !strings.Contains(store.sqlLog[1], "t.name LIKE") {
		t.Errorf("the second search is not a pattern: %q", store.sqlLog[1])
	}
	// Prefix before substring: the second search anchors at the start, the
	// third does not.
	if store.argsLog[1][0] != "qdrant%" {
		t.Errorf("second tier matched %v, want a prefix", store.argsLog[1][0])
	}
	if store.argsLog[2][0] != "%qdrant%" {
		t.Errorf("third tier matched %v, want a substring", store.argsLog[2][0])
	}
}

func TestScopedTermSearchStopsAtTheFirstTierThatAnswers(t *testing.T) {
	store := &fakeStore{rowsQueue: []*fakeRows{
		{values: [][]any{{int64(10), int64(12), "aimee", "src/a.c", "definition"}}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTermFindInProjectRequest("aimee", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageTermFindInProject), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v, want only the exact search", store.sqlLog)
	}
	if store.lastArgs[2] != "aimee" {
		t.Errorf("the project was not passed as a filter: %v", store.lastArgs)
	}
}

func TestPatternEscapingKeepsAnIdentifierLiteral(t *testing.T) {
	// An identifier containing an underscore is the common case here, and
	// unescaped it would match any character -- so "aimee_db" would find
	// "aimeexdb". The escape character is the backslash the statements name.
	if got := likePattern("aimee_db", false); got != `aimee\_db%` {
		t.Errorf("prefix pattern = %q", got)
	}
	if got := likePattern("50%", true); got != `%50\%%` {
		t.Errorf("substring pattern = %q", got)
	}
	if got := likePattern(`a\b`, false); got != `a\\b%` {
		t.Errorf("backslash pattern = %q", got)
	}
	for _, query := range []string{
		termFindInProjectLikeQuery, termFindExcludingProjectLikeQuery,
	} {
		if !strings.Contains(query, `ESCAPE '\'`) {
			t.Errorf("the escape character is not declared: %q", query)
		}
	}
}

func TestScopedTermSearchesDifferOnlyInTheProjectClause(t *testing.T) {
	// The C keeps four copies of this statement and they had already drifted.
	// Building them from one shape is what stops the next change landing on
	// three of the four.
	if !strings.Contains(termFindInProjectQuery, "($3 = '' OR p.name = $3)") {
		t.Errorf("an empty project no longer means every project: %q",
			termFindInProjectQuery)
	}
	if !strings.Contains(termFindExcludingProjectQuery, "p.name <> $3") {
		t.Errorf("the excluding search filters to rather than excludes: %q",
			termFindExcludingProjectQuery)
	}
	for _, query := range []string{
		termFindInProjectQuery, termFindInProjectLikeQuery,
		termFindExcludingProjectQuery, termFindExcludingProjectLikeQuery,
	} {
		if !strings.Contains(query,
			"ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END") {
			t.Errorf("definitions no longer lead: %q", query)
		}
		if !strings.Contains(query, "f.generation = p.current_generation") {
			t.Errorf("superseded rows would surface: %q", query)
		}
	}
}

func TestExcludingSearchAnswersWhatElseUsesTheName(t *testing.T) {
	store := &fakeStore{rowsQueue: []*fakeRows{
		{values: [][]any{{int64(4), int64(4), "other", "src/b.c", "reference"}}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTermFindExcludingProjectRequest("aimee", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageTermFindExcludingProject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hits, decodeErr := db2contract.DecodeTermFindExcludingProjectReply(body)
	if decodeErr != nil || len(hits) != 1 || hits[0].HitProject != "other" {
		t.Fatalf("hits = %+v", hits)
	}
}
