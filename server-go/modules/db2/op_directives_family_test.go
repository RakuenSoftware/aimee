package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func epistemicDirectiveRow() []any {
	return []any{
		int64(4), "which is true?", "vault rotation", "vault", "docs/a.md",
		"contradiction", int64(80), "open", int64(10), int64(11), int64(0),
		`["memory:10"]`, "session-1", int64(2), "2026-02-01T00:00:00Z", "",
		"2026-06-01T00:00:00Z", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
	}
}

func TestDirectivesLeadWithWhatMattersMost(t *testing.T) {
	// A directive is a question the system wants answered, and the one it most
	// wants answered should lead however long it has been waiting.
	for _, query := range []string{
		directiveListQuery, directiveByEntityQuery, directiveByFileQuery,
	} {
		if !strings.Contains(query, "ORDER BY priority DESC, created_at DESC, id DESC") {
			t.Errorf("the ordering changed: %q", query)
		}
	}
}

func TestAnchoredDirectiveReadsSeeOnlyOpenOnes(t *testing.T) {
	// The anchored reads surface directives to someone working; a resolved or
	// suppressed one is not a question any more. The list is the exception --
	// it is the review view.
	for _, query := range []string{directiveByEntityQuery, directiveByFileQuery} {
		if !strings.Contains(query, "state = 'open'") {
			t.Errorf("a settled directive could surface: %q", query)
		}
	}
	if !strings.Contains(directiveListQuery, "($2 = '' OR state = $2)") {
		t.Errorf("the list can no longer show every state: %q", directiveListQuery)
	}
	if !strings.Contains(directiveListQuery, "($3 = '' OR cause = $3)") {
		t.Errorf("the cause filter is gone: %q", directiveListQuery)
	}
}

func TestDirectiveRowCarriesEveryColumn(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{epistemicDirectiveRow()}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveListRequest("open", "", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeDirectiveListReply(body)
	if decodeErr != nil || len(found) != 1 {
		t.Fatalf("rows = %+v", found)
	}
	row := found[0]
	if row.DirectiveID != 4 || row.Question != "which is true?" ||
		row.Topic != "vault rotation" || row.AnchorEntity != "vault" ||
		row.AnchorFile != "docs/a.md" || row.Cause != "contradiction" ||
		row.Priority != 80 || row.State != "open" || row.MemoryAID != 10 ||
		row.MemoryBID != 11 || row.ResolutionMemoryID != 0 ||
		row.Evidence != `["memory:10"]` || row.SourceSession != "session-1" ||
		row.SurfacedCount != 2 || row.LastSurfacedAt == "" ||
		row.ResolvedAt != "" || row.ValidUntil == "" || row.CreatedAt == "" ||
		row.UpdatedAt == "" {
		t.Fatalf("row = %+v", row)
	}
	if store.lastArgs[1] != "open" || store.lastArgs[2] != "" {
		t.Errorf("the filters were not passed: %v", store.lastArgs)
	}
}

func TestDirectiveGetSharesTheRowReader(t *testing.T) {
	// One reader for nineteen columns, so the single-row read and the list
	// reads cannot disagree about which column is which.
	store := &fakeStore{row: &fakeRow{values: epistemicDirectiveRow()}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveGetRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, question, topic, entity, file, cause, priority, state,
		memoryA, memoryB, resolution, evidence, session, surfaced,
		lastSurfaced, resolvedAt, validUntil, createdAt, updatedAt, decodeErr :=
		db2contract.DecodeDirectiveGetReply(body)
	if decodeErr != nil || found != 1 || question != "which is true?" ||
		topic != "vault rotation" || entity != "vault" || file != "docs/a.md" ||
		cause != "contradiction" || priority != 80 || state != "open" ||
		memoryA != 10 || memoryB != 11 || resolution != 0 ||
		evidence != `["memory:10"]` || session != "session-1" || surfaced != 2 ||
		lastSurfaced == "" || resolvedAt != "" || validUntil == "" ||
		createdAt == "" || updatedAt == "" {
		t.Fatalf("directive = %d %q %q %q", found, question, topic, state)
	}
}

func TestUnknownDirectiveReportsItself(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveGetRequest(2147483000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, question, _, _, _, _, priority, _, _, _, _, _, _, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeDirectiveGetReply(body)
	if decodeErr != nil || found != 0 || question != "" || priority != 0 {
		t.Fatalf("found = %d, question = %q", found, question)
	}
}

func TestLexicalClauseBecomesOneOrOfLikes(t *testing.T) {
	// The clause arrives as the query builder wrote it -- quoted words joined
	// by OR -- so the words are parsed out, the connector dropped, and each
	// becomes an unanchored LIKE against the question and topic together.
	statement, args := lexicalDirectiveQuery(`"vault" OR "rotation"`, 16, 16)
	if !strings.Contains(statement, "LOWER(question || ' ' || topic) LIKE $2 OR "+
		"LOWER(question || ' ' || topic) LIKE $3") {
		t.Fatalf("statement = %q", statement)
	}
	if len(args) != 3 || args[0] != int64(16) || args[1] != "%vault%" ||
		args[2] != "%rotation%" {
		t.Fatalf("args = %v", args)
	}
	// The connector is dropped rather than searched for, and a word repeated in
	// the clause is one pattern.
	if strings.Contains(statement, "$4") {
		t.Errorf("the OR connector became a term: %q", statement)
	}
	_, repeated := lexicalDirectiveQuery(`"vault" OR "vault"`, 16, 16)
	if len(repeated) != 2 {
		t.Errorf("a repeated word became two patterns: %v", repeated)
	}
}

func TestLexicalClauseIsBounded(t *testing.T) {
	// A clause with a hundred words would otherwise build a hundred-way OR of
	// unanchored LIKEs.
	clause := strings.Repeat("word", 1)
	var builder strings.Builder
	for index := 0; index < 40; index++ {
		builder.WriteString(clause)
		builder.WriteString(string(rune('a' + index%26)))
		builder.WriteString(" OR ")
	}
	_, args := lexicalDirectiveQuery(builder.String(), 16, 16)
	if len(args) != directiveLexicalMaxTokens+1 {
		t.Fatalf("patterns = %d, want the bound", len(args)-1)
	}
}

func TestALexicalClauseWithNothingInItAsksNothing(t *testing.T) {
	// The statement it would build has an empty parenthesis in it and does not
	// parse.
	if statement, _ := lexicalDirectiveQuery(`"" OR ""`, 16, 16); statement != "" {
		t.Fatalf("statement = %q", statement)
	}
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveByLexicalRequest(`"" OR ""`, 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveByLexical), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeDirectiveByLexicalReply(body)
	if decodeErr != nil || len(found) != 0 {
		t.Fatalf("rows = %+v", found)
	}
	if store.lastSQL != "" {
		t.Errorf("a query ran for a clause with nothing in it: %q", store.lastSQL)
	}
}

func TestAnchorReadsDifferOnCaseForDirectivesToo(t *testing.T) {
	// The same split the prospective reads make: an entity anchor is folded
	// because the caller folds its own, a path is matched exactly.
	if !strings.Contains(directiveByEntityQuery, "LOWER(anchor_entity) = $2") {
		t.Errorf("the entity anchor is no longer folded: %q", directiveByEntityQuery)
	}
	if strings.Contains(directiveByFileQuery, "LOWER(anchor_file)") {
		t.Errorf("two distinct paths would collide: %q", directiveByFileQuery)
	}
}
