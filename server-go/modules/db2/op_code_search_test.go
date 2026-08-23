package db2

import (
	"strings"
	"sync"
	"testing"

	"github.com/jackc/pgx/v5"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestCodeSearchSelectsContentOnlyWhenEnriching(t *testing.T) {
	// The content does not cross the wire; it is read to locate the matched
	// line, and the line number is what the reply carries. Selecting it when
	// nobody asked would transfer whole files for nothing.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"aimee", "src/a.c", "int >>>main<<< (void)", 0.8, "abc123"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeSearchRequest("main", "", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeSearch), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hits, decodeErr := db2contract.DecodeCodeSearchReply(body)
	if decodeErr != nil || len(hits) != 1 || hits[0].FilePath != "src/a.c" ||
		hits[0].Rank != 0.8 || hits[0].ContentHash != "abc123" {
		t.Fatalf("hits = %+v", hits)
	}
	if hits[0].Line != 0 {
		t.Errorf("line = %d, want zero when not enriching", hits[0].Line)
	}
	if strings.Contains(store.lastSQL, "fc.content,\n") ||
		strings.Contains(store.lastSQL, ", fc.content\n") {
		t.Errorf("the content is selected without enrichment: %q", store.lastSQL)
	}
}

func TestCodeSearchLocatesTheMatchedLine(t *testing.T) {
	// The snippet says what matched and the content says where it is; neither
	// alone gives a line number.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"aimee", "src/a.c", "int >>>main<<< (void)", 0.8, "abc123",
			"#include <stdio.h>\n\nint main(void)\n{\n}\n"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeSearchRequest("main", "", 1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeSearch), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hits, decodeErr := db2contract.DecodeCodeSearchReply(body)
	if decodeErr != nil || len(hits) != 1 || hits[0].Line != 3 {
		t.Fatalf("hits = %+v", hits)
	}
	if !strings.Contains(store.lastSQL, "fc.content") {
		t.Errorf("the content is not selected for enrichment: %q", store.lastSQL)
	}
}

func TestMatchedLineIsZeroWhenItCannotBeFound(t *testing.T) {
	// A headline can elide across a boundary, so the first occurrence of the
	// marked text is the best guess available rather than a certainty. Zero is
	// a real answer.
	if line := matchedLine("int main(void)\n", "no markers here"); line != 0 {
		t.Errorf("unmarked snippet located line %d", line)
	}
	if line := matchedLine("int main(void)\n", "int >>><<< main"); line != 0 {
		t.Errorf("an empty span located line %d", line)
	}
	if line := matchedLine("int main(void)\n", ">>>absent<<<"); line != 0 {
		t.Errorf("a span the content lacks located line %d", line)
	}
	// The delimiters have to agree with what ts_headline is told to mark with.
	if !strings.Contains(codeSearchSelect, "StartSel="+codeSearchMatchOpen) ||
		!strings.Contains(codeSearchSelect, "StopSel="+codeSearchMatchClose) {
		t.Errorf("the marker and the locator disagree: %q", codeSearchSelect)
	}
}

func TestCodeSearchUsesAPlainQuery(t *testing.T) {
	// plainto_tsquery rather than to_tsquery, so a caller typing operators gets
	// them as words rather than a syntax error -- a code search is something a
	// person types.
	for _, query := range []string{
		codeSearchAllQuery, codeSearchInProjectQuery, codeSearchExcludingQuery,
	} {
		if strings.Contains(query, "to_tsquery('simple', $1)") &&
			!strings.Contains(query, "plainto_tsquery('simple', $1)") {
			t.Errorf("the query is parsed as an expression: %q", query)
		}
		if !strings.Contains(query, "f.generation = p.current_generation") {
			t.Errorf("superseded files would surface: %q", query)
		}
	}
}

func TestCrossRepoCodeSearchRefusesAnEmptyExclusion(t *testing.T) {
	// A caller that meant to name a project and passed an empty string would
	// otherwise get its own code back in an answer it asked to have it left
	// out of.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeSearchExcludingProjectRequest("main", "", 0)
	if err != nil {
		// The envelope may refuse it first, which is the same answer earlier.
		return
	}
	if _, status := handler(
		invocation(db2contract.StageCodeSearchExcludingProject), request); status !=
		bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want the request refused", status)
	}
	if store.lastSQL != "" {
		t.Errorf("the search ran anyway: %q", store.lastSQL)
	}
}

func TestEdgeUpsertRefusesToBumpAnInferredEdge(t *testing.T) {
	// The unique index covers the triple regardless of edge_class, so a
	// conflict can land on a semantic edge. The C's pre-migration path excludes
	// those explicitly and says why; its post-migration path bumps them anyway,
	// because ON CONFLICT takes no predicate on what it matched.
	if !strings.Contains(entityEdgeUpsertQuery,
		"WHERE entity_edges.edge_class <> 'semantic'") {
		t.Errorf("an observation would inflate an inference: %q", entityEdgeUpsertQuery)
	}
	for _, query := range []string{entityEdgeProbeQuery, entityEdgeBumpQuery} {
		if !strings.Contains(query, "edge_class <> 'semantic'") {
			t.Errorf("the pre-migration path lost its exclusion: %q", query)
		}
	}
}

func TestEdgeUpsertTakesThePreMigrationPathWithoutTheIndex(t *testing.T) {
	// Getting this wrong in the other direction means an ON CONFLICT naming an
	// index that is not there, which fails every call rather than one.
	entityEdgeIndexOnce = sync.Once{}
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows}, // no unique index
		{err: pgx.ErrNoRows}, // and no existing edge
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeUpsertRequest(
		"aimee", "depends_on", "postgres", 0, 12, 99, 99)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgeUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, added, decodeErr :=
		db2contract.DecodeEntityEdgeUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 || added != 1 {
		t.Fatalf("acknowledged = %d, added = %d", acknowledged, added)
	}
	if strings.Contains(store.lastSQL, "ON CONFLICT") {
		t.Errorf("the constraint path ran without the constraint: %q", store.lastSQL)
	}
}

func TestEdgeUpsertBumpsAnExistingEdgeWithoutAddingOne(t *testing.T) {
	// Weight is a count of observations, so a repeat is a bump rather than a
	// second row -- which is what makes weight mean anything when the graph is
	// read. edge_added says whether the graph changed shape or only degree.
	entityEdgeIndexOnce = sync.Once{}
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},      // no unique index
		{values: []any{int64(1)}}, // the edge is already there
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeUpsertRequest(
		"aimee", "depends_on", "postgres", 0, 12, 99, 99)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgeUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, added, decodeErr :=
		db2contract.DecodeEntityEdgeUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 || added != 0 {
		t.Fatalf("acknowledged = %d, added = %d", acknowledged, added)
	}
	if !strings.Contains(store.lastSQL, "SET weight = weight + 1") {
		t.Errorf("the repeat did not bump: %q", store.lastSQL)
	}
}

func TestTypedWalkAnswersUnknownRatherThanNothing(t *testing.T) {
	// The three defaults are the unknown members of their own enumerations, so
	// an edge written before typing existed answers unknown rather than
	// answering nothing.
	for _, fallback := range []string{
		"COALESCE(relation_id, 12)", "COALESCE(subject_kind, 99)",
		"COALESCE(object_kind, 99)",
	} {
		if !strings.Contains(entityWalkStepTypedQuery, fallback) {
			t.Errorf("missing %s", fallback)
		}
	}
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"aimee", "depends_on", "postgres", int64(12), int64(99), int64(99), int64(4)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityWalkStepTypedRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityWalkStepTyped), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	steps, decodeErr := db2contract.DecodeEntityWalkStepTypedReply(body)
	if decodeErr != nil || len(steps) != 1 || steps[0].RelationID != 12 ||
		steps[0].SubjectKind != 99 || steps[0].Weight != 4 {
		t.Fatalf("steps = %+v", steps)
	}
	// Either end, one parameter: a walk out from a node follows edges it is the
	// target of as readily as ones it is the source of.
	if !strings.Contains(store.lastSQL, "(source = $1 OR target = $1)") {
		t.Errorf("the walk is now one-directional: %q", store.lastSQL)
	}
}

func TestLowEffectivenessSkipsWhatNobodyMeasured(t *testing.T) {
	// NULL is not a low score -- it is a memory nothing has measured. Comparing
	// it would answer NULL and exclude it anyway; saying so makes the intent
	// readable rather than incidental.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "L2", "fact", "build-state", 0.1, int64(2)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryLowEffectivenessRequest(0.3, 32)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryLowEffectiveness), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeMemoryLowEffectivenessReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].MemoryID != 4 ||
		found[0].MemoryEffectiveness != 0.1 || found[0].UseCount != 2 {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "effectiveness IS NOT NULL") {
		t.Errorf("unmeasured memories would be trimmed: %q", store.lastSQL)
	}
	// Worst first, so a caller trimming the bottom takes the worst rather than
	// an arbitrary slice of everything below the line.
	if !strings.Contains(store.lastSQL, "ORDER BY effectiveness ASC") {
		t.Errorf("the worst no longer lead: %q", store.lastSQL)
	}
}
