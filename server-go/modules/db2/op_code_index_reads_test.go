package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEveryCodeIndexReadHidesSupersededAndHiddenRows(t *testing.T) {
	// The generation pair keeps a rescan's superseded rows out: a file deleted
	// and reindexed still has its old rows, and reporting a caller at a line
	// that no longer exists sends someone to the wrong place. The path filters
	// keep hidden files out, and they are here rather than only in the scanner
	// because rows written before the scanner grew its own guard are still in
	// the table.
	for _, statement := range []struct {
		name  string
		query string
	}{
		{name: "callers", query: callersFindAllQuery},
		{name: "callers-in-project", query: callersFindInProjectQuery},
		{name: "callers-elsewhere", query: callersFindExcludingProjectQuery},
		{name: "terms", query: termFindQuery},
	} {
		t.Run(statement.name, func(t *testing.T) {
			for _, clause := range []string{
				"p.lifecycle_state = 'current'",
				"f.generation = p.current_generation",
				"f.path NOT LIKE '.%'",
				"f.path NOT LIKE '%/.%'",
				"p.root NOT LIKE '%/.%'",
			} {
				if !strings.Contains(statement.query, clause) {
					t.Errorf("missing %s", clause)
				}
			}
		})
	}
}

func TestCallersWithoutAProjectSearchesEveryProject(t *testing.T) {
	// An empty project means every project, not no project. The alternative --
	// an empty answer -- would look like a symbol nobody calls.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(12), "aimee", "src/a.c", "main"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCallersFindRequest("", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCallersFind), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	callers, decodeErr := db2contract.DecodeCallersFindReply(body)
	if decodeErr != nil || len(callers) != 1 {
		t.Fatalf("callers = %+v", callers)
	}
	if callers[0].CallerLine != 12 || callers[0].CallerProject != "aimee" ||
		callers[0].CallerFilePath != "src/a.c" || callers[0].CallerSymbol != "main" {
		t.Fatalf("caller = %+v", callers[0])
	}
	if strings.Contains(store.lastSQL, "p.name = $2") {
		t.Errorf("an empty project was used as a filter: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 {
		t.Errorf("args = %v, want the callee and the limit", store.lastArgs)
	}
}

func TestCallersWithAProjectFiltersToIt(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCallersFindRequest("aimee", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCallersFind), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "p.name = $2") {
		t.Errorf("the project is not a filter: %q", store.lastSQL)
	}
	if store.lastArgs[1] != "aimee" {
		t.Errorf("args = %v", store.lastArgs)
	}
}

func TestScopedCallersRunTheSameStatementAsTheFilteredSearch(t *testing.T) {
	// The two operations answer the same question from different callers. A
	// scoped search that disagreed with the unscoped one filtered by hand would
	// be a bug nobody could see, so they share the statement.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCallersFindScopedRequest("aimee", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCallersFindScoped), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastSQL != callersFindInProjectQuery {
		t.Errorf("the scoped search has its own statement now:\n%q", store.lastSQL)
	}
}

func TestCallersElsewhereExcludeRatherThanFilter(t *testing.T) {
	// The cross-repository question: a symbol's callers everywhere except where
	// it lives. Getting the comparison the wrong way round would answer the
	// opposite question and still look plausible.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "other", "src/b.c", "helper"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCallersFindExcludingProjectRequest("aimee", "db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCallersFindExcludingProject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	callers, decodeErr := db2contract.DecodeCallersFindExcludingProjectReply(body)
	if decodeErr != nil || len(callers) != 1 || callers[0].CallerProject != "other" {
		t.Fatalf("callers = %+v", callers)
	}
	if !strings.Contains(store.lastSQL, "p.name <> $2") {
		t.Errorf("the project is filtered to rather than excluded: %q", store.lastSQL)
	}
}

func TestFileDefinitionsAreDefinitionsInFileOrder(t *testing.T) {
	// A file's terms include every reference it makes. A caller asking what a
	// file defines wants its outline, not its dependencies -- and in the order
	// the file is written, which is what makes it an outline.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"main", "definition", int64(10), int64(20)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFileDefinitionsRequest("aimee", "src/a.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFileDefinitions), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	definitions, decodeErr := db2contract.DecodeFileDefinitionsReply(body)
	if decodeErr != nil || len(definitions) != 1 {
		t.Fatalf("definitions = %+v", definitions)
	}
	if definitions[0].SymbolName != "main" || definitions[0].Line != 10 ||
		definitions[0].LineEnd != 20 {
		t.Fatalf("definition = %+v", definitions[0])
	}
	if !strings.Contains(store.lastSQL, "t.kind = 'definition'") {
		t.Errorf("references would be listed as definitions: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY t.line") {
		t.Errorf("the outline is no longer in file order: %q", store.lastSQL)
	}
	// The caller named the file, so the hidden-path filter the other reads
	// carry would be answering a different question here.
	if strings.Contains(store.lastSQL, "NOT LIKE") {
		t.Errorf("a file asked for by name is being hidden: %q", store.lastSQL)
	}
}

func TestFileDefinitionsResolveInOneStatement(t *testing.T) {
	// The C resolves the project, then the file, then reads the terms. The
	// joins say the same thing, and an unknown project or file is an empty
	// answer either way.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFileDefinitionsRequest("nothing-here", "src/a.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFileDefinitions), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	definitions, decodeErr := db2contract.DecodeFileDefinitionsReply(body)
	if decodeErr != nil || len(definitions) != 0 {
		t.Fatalf("definitions = %+v", definitions)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v, want one", store.sqlLog)
	}
}

func TestTermHitsLeadWithTheDefinition(t *testing.T) {
	// Someone looking up an identifier wants the place it is defined before the
	// places it is used. The CASE puts it there without a second read.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(10), int64(12), "aimee", "src/a.c", "definition"},
		{int64(40), int64(40), "aimee", "src/b.c", "reference"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTermFindRequest("db2_conn")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTermFind), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	hits, decodeErr := db2contract.DecodeTermFindReply(body)
	if decodeErr != nil || len(hits) != 2 {
		t.Fatalf("hits = %+v", hits)
	}
	if hits[0].TermKind != "definition" || hits[0].Line != 10 || hits[0].LineEnd != 12 {
		t.Fatalf("first hit = %+v", hits[0])
	}
	if !strings.Contains(store.lastSQL,
		"ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END") {
		t.Errorf("definitions no longer lead: %q", store.lastSQL)
	}
	// The GROUP BY has no aggregate over it: it is a DISTINCT written the long
	// way. A symbol both declared and defined on one line is recorded twice,
	// and without this the same location comes back twice.
	if !strings.Contains(store.lastSQL, "GROUP BY p.name, f.path, t.line, t.kind, t.line_end") {
		t.Errorf("duplicate locations would surface: %q", store.lastSQL)
	}
}

func TestTermFindMatchesExactly(t *testing.T) {
	// The C carries a second statement matching with LIKE and never prepares
	// it. Porting a branch no caller can reach would be inventing behaviour
	// rather than moving it.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTermFindRequest("db2_%")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageTermFind), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "LIKE $1") ||
		strings.Contains(store.lastSQL, "t.name LIKE") {
		t.Errorf("an identifier with a wildcard in it is matched as a pattern: %q",
			store.lastSQL)
	}
	if store.lastArgs[0] != "db2_%" {
		t.Errorf("the identifier was rewritten: %v", store.lastArgs[0])
	}
}
