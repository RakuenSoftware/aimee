package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestNodeUpsertRefreshesEverythingButTheKey(t *testing.T) {
	// A node seen again in a new generation is the same node, described by
	// whatever saw it last -- and the generation is how a sweep tells a node
	// the current scan found from one a scan that no longer runs left behind.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNodeUpsertRequest(
		"node:main", 1, "aimee", "main", "src/a.c:main", "src/a.c", "main",
		"scan", 7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityNodeUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeEntityNodeUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	update := store.lastSQL[strings.Index(store.lastSQL, "DO UPDATE SET"):]
	if strings.Contains(update, "node_key =") {
		t.Errorf("the key is rewritten on conflict: %q", update)
	}
	if !strings.Contains(update, "last_seen_generation_id = excluded.last_seen_generation_id") {
		t.Errorf("a re-seen node keeps an old generation: %q", update)
	}
	// Every timestamp in this port is the canonical UTC spelling; the C writes
	// a local one here, and two spellings in one table make a comparison
	// between rows depend on which writer wrote them.
	if !strings.Contains(store.lastSQL, "pg_now_text()") {
		t.Errorf("the stamp is no longer canonical: %q", store.lastSQL)
	}
}

func TestNodeGetAnswersWhatPutItThere(t *testing.T) {
	// The origin is what a caller needs before deciding whether it may rewrite
	// the node.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(1), "aimee", "main", "src/a.c:main", "src/a.c", "main", "scan",
		int64(7),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityNodeGetRequest("node:main")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityNodeGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, kind, generation, project, display, fullKey, path, symbol, origin, decodeErr :=
		db2contract.DecodeEntityNodeGetReply(body)
	if decodeErr != nil || found != 1 || kind != 1 || generation != 7 ||
		project != "aimee" || display != "main" || fullKey != "src/a.c:main" ||
		path != "src/a.c" || symbol != "main" || origin != "scan" {
		t.Fatalf("node = %d %d %d %q %q %q %q %q %q", found, kind, generation,
			project, display, fullKey, path, symbol, origin)
	}
	// The C selects node_key and never reads it back -- the caller supplied it.
	if strings.Contains(store.lastSQL, "SELECT node_key") {
		t.Errorf("a column nothing reads is still selected: %q", store.lastSQL)
	}
}

func TestEdgeExplainRanksByBothWeights(t *testing.T) {
	// An edge that is structurally important and an edge that has been observed
	// often are both worth explaining, and neither number alone ranks them
	// against each other.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "aimee", "depends_on", "postgres", int64(9), int64(3), 0.5,
			"code_projection"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeExplainRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgeExplain), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	edges, decodeErr := db2contract.DecodeEntityEdgeExplainReply(body)
	if decodeErr != nil || len(edges) != 1 || edges[0].EdgeWeight != 9 ||
		edges[0].StructuralWeight != 3 || edges[0].UtilityScore != 0.5 ||
		edges[0].EdgeOrigin != "code_projection" {
		t.Fatalf("edges = %+v", edges)
	}
	if !strings.Contains(store.lastSQL,
		"ORDER BY (COALESCE(structural_weight, 0) + weight) DESC") {
		t.Errorf("the ranking changed: %q", store.lastSQL)
	}
	// The COALESCEs are over columns added after the table, so rows written
	// before they existed answer their neutral values rather than failing.
	for _, fallback := range []string{
		"COALESCE(structural_weight, 0)", "COALESCE(utility_score, 0.0)",
		"COALESCE(edge_origin, '')",
	} {
		if !strings.Contains(store.lastSQL, fallback) {
			t.Errorf("missing %s", fallback)
		}
	}
	if !strings.Contains(store.lastSQL, "edge_class <> 'semantic'") {
		t.Errorf("inferred edges would be explained as observed: %q", store.lastSQL)
	}
}

func TestRegionsReadInReadingOrder(t *testing.T) {
	// A caller drawing a citation's regions on a page wants them in reading
	// order, which is what line_index records.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(2), 10.5, 20.5, 100.5, 30.5, "the quoted line", int64(4), "text"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocRegionsForChunkRequest(900027)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocRegionsForChunk), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	regions, decodeErr := db2contract.DecodeKBDocRegionsForChunkReply(body)
	if decodeErr != nil || len(regions) != 1 || regions[0].PageNo != 2 ||
		regions[0].X0 != 10.5 || regions[0].Y1 != 30.5 ||
		regions[0].RegionQuote != "the quoted line" || regions[0].LineIndex != 4 {
		t.Fatalf("regions = %+v", regions)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY line_index") {
		t.Errorf("the regions no longer read in order: %q", store.lastSQL)
	}
}

func TestDocumentFetchWithoutAProjectResolvesByIdentifier(t *testing.T) {
	// A whole-corpus search returns rows from every project, and the caller
	// resolving them has no project to name. A named project still scopes it.
	store := &fakeStore{rows: &fakeRows{}, row: &fakeRow{values: []any{
		"aimee", "docs/a.md", "abc123", "Title > Section", int64(10), int64(20),
		"the chunk text", "markdown",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentFetchRequest(900027, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocumentFetch), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, project, path, hash, heading, start, end, content, kind, decodeErr :=
		db2contract.DecodeKBDocumentFetchReply(body)
	if decodeErr != nil || found != 1 || project != "aimee" ||
		path != "docs/a.md" || hash != "abc123" || heading != "Title > Section" ||
		start != 10 || end != 20 || content != "the chunk text" ||
		kind != "markdown" {
		t.Fatalf("document = %d %q %q %q %q %d %d %q %q", found, project, path,
			hash, heading, start, end, content, kind)
	}
	if !strings.Contains(store.lastSQL, "($2 = '' OR d.project = $2)") {
		t.Errorf("an empty project no longer resolves by identifier: %q",
			store.lastSQL)
	}
	// The generation pinning applies either way: an identifier survives a
	// reindex but the row it names is no longer the current text.
	if !strings.Contains(store.lastSQL, "d.generation = p.current_generation") {
		t.Errorf("a superseded chunk could be fetched: %q", store.lastSQL)
	}
}

func TestDocumentFetchReportsAMissingChunk(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocumentFetchRequest(2147483000, "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocumentFetch), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, project, _, _, _, _, _, content, _, decodeErr :=
		db2contract.DecodeKBDocumentFetchReply(body)
	if decodeErr != nil || found != 0 || project != "" || content != "" {
		t.Fatalf("found = %d, project = %q", found, project)
	}
}
