package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestUnitsReadInExtractionOrder(t *testing.T) {
	// A memory's units are a decomposition of one text. The C leaves the order
	// to the planner, which can hand the same memory back differently on the
	// next call -- and a decomposition that reorders itself is not one.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "claim", "build-state", "the build is green", 1.0},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryUnitsListRequest(9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryUnitsList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	units, decodeErr := db2contract.DecodeMemoryUnitsListReply(body)
	if decodeErr != nil || len(units) != 1 || units[0].UnitID != 4 ||
		units[0].UnitText != "the build is green" || units[0].UnitWeight != 1 {
		t.Fatalf("units = %+v", units)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id ASC") {
		t.Errorf("the units are no longer stably ordered: %q", store.lastSQL)
	}
}

func TestTaskEdgesSeeBothDirections(t *testing.T) {
	// A caller asking what a task is connected to means both what it depends on
	// and what depends on it.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(1), int64(2), "depends_on"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskEdgesRequest(1, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskEdges), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	edges, decodeErr := db2contract.DecodeTaskEdgesReply(body)
	if decodeErr != nil || len(edges) != 1 || edges[0].TaskEdgeID != 4 ||
		edges[0].SourceTaskID != 1 || edges[0].TargetTaskID != 2 ||
		edges[0].TaskRelation != "depends_on" {
		t.Fatalf("edges = %+v", edges)
	}
	if !strings.Contains(store.lastSQL, "source_id = $1 OR target_id = $1") {
		t.Errorf("one direction is missing: %q", store.lastSQL)
	}
	// One parameter where the C binds the task twice: the same predicate said
	// once.
	if len(store.lastArgs) != 2 {
		t.Errorf("args = %v, want the task and the limit", store.lastArgs)
	}
}

func TestAntiPatternsLeadWithWhatHasCaughtSomething(t *testing.T) {
	// A pattern that has caught something repeatedly is worth showing before
	// one that merely looks likely, and confidence breaks the tie between
	// patterns nothing has hit yet.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "retry without backoff", "hammers the service", "review",
			"pr/1", int64(12), 0.9},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAntiPatternListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAntiPatternList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	patterns, decodeErr := db2contract.DecodeAntiPatternListReply(body)
	if decodeErr != nil || len(patterns) != 1 {
		t.Fatalf("patterns = %+v", patterns)
	}
	if patterns[0].AntiPatternID != 4 || patterns[0].HitCount != 12 ||
		patterns[0].Confidence != 0.9 ||
		patterns[0].Pattern != "retry without backoff" ||
		patterns[0].SourceRef != "pr/1" {
		t.Fatalf("pattern = %+v", patterns[0])
	}
	if !strings.Contains(store.lastSQL, "ORDER BY hit_count DESC, confidence DESC") {
		t.Errorf("the ordering changed: %q", store.lastSQL)
	}
}

func TestConsoleSettingsSayWhetherAnyoneSetThemUp(t *testing.T) {
	// A console nobody has configured and one configured with every field left
	// blank encode identically, and only one of them is a mistake worth telling
	// an operator about.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeConsoleOidcGetRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageConsoleOidcGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	configured, issuer, _, _, _, _, _, decodeErr :=
		db2contract.DecodeConsoleOidcGetReply(body)
	if decodeErr != nil || configured != 0 || issuer != "" {
		t.Fatalf("configured = %d, issuer = %q", configured, issuer)
	}

	store = &fakeStore{row: &fakeRow{values: []any{"", "", "", "", "", ""}}}
	handler = NewDispatchHandler(store)
	body, status = handler(invocation(db2contract.StageConsoleOidcGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	configured, issuer, _, _, _, _, _, decodeErr =
		db2contract.DecodeConsoleOidcGetReply(body)
	if decodeErr != nil || configured != 1 || issuer != "" {
		t.Fatalf("a blank row answered configured = %d", configured)
	}
	if !strings.Contains(store.lastSQL, "WHERE id = 1") {
		t.Errorf("the settings are no longer read as a singleton: %q", store.lastSQL)
	}
}

func TestGenerationMetaCarriesBothVersions(t *testing.T) {
	// A snapshot diff checks these before comparing generations: a parser
	// change would read as a structural change, so comparing across extractor
	// versions says something about the extractor rather than about the code.
	store := &fakeStore{row: &fakeRow{values: []any{
		"aimee", "visible", "abc123", "1.4.0", "2.0.0",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionGenerationMetaRequest(7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageProjectionGenerationMeta), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, project, state, hash, extractor, pipeline, decodeErr :=
		db2contract.DecodeProjectionGenerationMetaReply(body)
	if decodeErr != nil || found != 1 || project != "aimee" || state != "visible" ||
		hash != "abc123" || extractor != "1.4.0" || pipeline != "2.0.0" {
		t.Fatalf("meta = %d %q %q %q %q %q",
			found, project, state, hash, extractor, pipeline)
	}
	// The C selects the identifier and never reads it -- the caller supplied
	// it.
	if strings.Contains(store.lastSQL, "SELECT id,") {
		t.Errorf("a column nothing reads is still selected: %q", store.lastSQL)
	}
}

func TestGenerationMetaReportsAnUnknownGeneration(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionGenerationMetaRequest(7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageProjectionGenerationMeta), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, project, _, _, extractor, _, decodeErr :=
		db2contract.DecodeProjectionGenerationMetaReply(body)
	if decodeErr != nil || found != 0 || project != "" || extractor != "" {
		t.Fatalf("found = %d, project = %q", found, project)
	}
}
