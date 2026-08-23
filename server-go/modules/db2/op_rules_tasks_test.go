package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestRulesCarryTheirExpiry(t *testing.T) {
	// The C's shared column list leaves expires_at out while the struct it
	// fills carries the field and the reply declares it, so every rules reply
	// has been answering an empty expiry for a rule that has one. A reader
	// treating empty as "never expires" keeps applying a rule past its expiry,
	// which is the failure the column exists to prevent.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(80), "positive", "Write it down", "always", "process",
			"2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z", "hard",
			"2026-06-01T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRulesList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rules, decodeErr := db2contract.DecodeRulesListReply(body)
	if decodeErr != nil || len(rules) != 1 {
		t.Fatalf("rules = %+v", rules)
	}
	if rules[0].ExpiresAt != "2026-06-01T00:00:00Z" {
		t.Fatalf("expires_at = %q, want the column", rules[0].ExpiresAt)
	}
	if rules[0].RuleID != 4 || rules[0].RuleWeight != 80 ||
		rules[0].RuleDirectiveType != "hard" {
		t.Fatalf("rule = %+v", rules[0])
	}
	// The column is nullable and most rules have no expiry, so absent has to
	// read as empty rather than failing the scan.
	if !strings.Contains(store.lastSQL, "COALESCE(expires_at, '')") {
		t.Errorf("a rule with no expiry would fail to scan: %q", store.lastSQL)
	}
}

func TestEveryRulesReadOrdersTheSameWay(t *testing.T) {
	// Weight decides whether a rule is a rule or an inclination, so a reader
	// taking the top of the list takes the binding ones -- and the title breaks
	// ties so the same set renders the same way twice running.
	for _, query := range []string{
		rulesListQuery, rulesListHardQuery, rulesListByTierQuery,
	} {
		if !strings.Contains(query, "ORDER BY weight DESC, title ASC") {
			t.Errorf("the ordering changed: %q", query)
		}
		if !strings.Contains(query, "COALESCE(expires_at, '')") {
			t.Errorf("the expiry is missing: %q", query)
		}
	}
}

func TestHardRulesAreADifferentQuestionFromHeavyOnes(t *testing.T) {
	// A soft rule of weight ninety still yields to judgement and a hard rule of
	// weight ten does not, so the two reads filter on different columns.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesListHardRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRulesListHard), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "directive_type = 'hard'") {
		t.Errorf("hard rules are no longer selected by directive: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "weight >=") {
		t.Errorf("hard now means heavy: %q", store.lastSQL)
	}

	store = &fakeStore{rows: &fakeRows{}}
	handler = NewDispatchHandler(store)
	tierRequest, err := db2contract.EncodeRulesListByTierRequest(75)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRulesListByTier), tierRequest); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "weight >= $2") {
		t.Errorf("the tier is no longer a weight: %q", store.lastSQL)
	}
	if store.lastArgs[1] != int64(75) {
		t.Errorf("threshold = %v", store.lastArgs[1])
	}
}

func TestRuleLookupIsCaseInsensitiveAndBounded(t *testing.T) {
	// Nothing constrains rules to unique titles, so this can match several. The
	// C takes the first the planner gives; LIMIT makes that explicit rather
	// than incidental, and the feedback write reinforces whichever this finds.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(4), "positive", "always", int64(80), "process", "hard", "",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesFindByTitleRequest("write it down")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRulesFindByTitle), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, id, polarity, description, weight, domain, directive, expires, decodeErr :=
		db2contract.DecodeRulesFindByTitleReply(body)
	if decodeErr != nil || found != 1 || id != 4 || polarity != "positive" ||
		description != "always" || weight != 80 || domain != "process" ||
		directive != "hard" || expires != "" {
		t.Fatalf("rule = %d %d %q %q %d %q %q %q",
			found, id, polarity, description, weight, domain, directive, expires)
	}
	if !strings.Contains(store.lastSQL, "LOWER(title) = LOWER($1)") {
		t.Errorf("the lookup is case sensitive again: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "LIMIT 1") {
		t.Errorf("a title matching twice would be ambiguous: %q", store.lastSQL)
	}
}

func TestTaskReadsHandleTheNullablePair(t *testing.T) {
	// A root task has no parent and a task can be created outside a session,
	// so both columns are nullable -- and pgx will not scan a NULL into a plain
	// type.
	for _, query := range []string{taskGetQuery, taskSubtasksQuery} {
		if !strings.Contains(query, "COALESCE(parent_id, 0)") ||
			!strings.Contains(query, "COALESCE(session_id, '')") {
			t.Errorf("a root or sessionless task would fail to scan: %q", query)
		}
	}
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(4), int64(0), "write it down", "todo", 1.0, "", "2026-01-01T00:00:00Z",
		"2026-01-01T00:00:00Z",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskGetRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, parent, title, state, confidence, session, created, updated, decodeErr :=
		db2contract.DecodeTaskGetReply(body)
	if decodeErr != nil || found != 1 || parent != 0 || title != "write it down" ||
		state != "todo" || confidence != 1 || session != "" ||
		created == "" || updated == "" {
		t.Fatalf("task = %d %d %q %q %v %q", found, parent, title, state,
			confidence, session)
	}
}

func TestSubtasksReadInPlanOrder(t *testing.T) {
	// A decomposition is read as a plan: the order the pieces were written down
	// is the order someone intended to do them in, and nothing else records
	// that.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(5), int64(4), "first", "todo", 1.0, "session-1",
			"2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskSubtasksRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskSubtasks), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	subtasks, decodeErr := db2contract.DecodeTaskSubtasksReply(body)
	if decodeErr != nil || len(subtasks) != 1 || subtasks[0].TaskRowID != 5 ||
		subtasks[0].ParentTaskID != 4 || subtasks[0].TaskSessionID != "session-1" {
		t.Fatalf("subtasks = %+v", subtasks)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY created_at ASC") {
		t.Errorf("the plan order is gone: %q", store.lastSQL)
	}
}

func TestUnknownTaskReportsItself(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskGetRequest(2147483000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, _, title, _, _, _, _, _, decodeErr := db2contract.DecodeTaskGetReply(body)
	if decodeErr != nil || found != 0 || title != "" {
		t.Fatalf("found = %d, title = %q", found, title)
	}
}
