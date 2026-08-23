package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func decisionLogFullRow() []any {
	return []any{
		int64(4), int64(9), "keep it | rewrite it", "keep it",
		"the rewrite costs more than it saves", "assumes traffic stays flat",
		"held", "2026-01-01T00:00:00Z", "active", "2026-06-01",
		int64(0), "storage", "jbailes", int64(0),
	}
}

func TestDecisionListsTruncateDeterministically(t *testing.T) {
	// The C's scoped list breaks ties on the identifier and its unscoped list
	// does not, so the unscoped one truncates unpredictably at the limit when
	// two decisions share a timestamp -- and these are written by hand, so
	// sharing one is not rare.
	for _, query := range []string{
		decisionLogListQuery, decisionLogListScopedQuery, taskListQuery,
	} {
		if !strings.Contains(query, ", id DESC LIMIT") {
			t.Errorf("the order is not total: %q", query)
		}
	}
}

func TestDecisionRowHandlesTheNullablePair(t *testing.T) {
	// A decision need not belong to a task, and one that has not played out yet
	// has no outcome. Both are the shape that fails only against a real row.
	if !strings.Contains(decisionColumns, "COALESCE(task_id, 0)") ||
		!strings.Contains(decisionColumns, "COALESCE(outcome, '')") {
		t.Errorf("a taskless or undecided row would fail to scan: %q",
			decisionColumns)
	}
}

func TestDecisionGetAnswersWhatWasRejected(t *testing.T) {
	// What was chosen is recoverable from the code; what was rejected and why
	// is not, which is what makes the log worth keeping.
	store := &fakeStore{row: &fakeRow{values: decisionLogFullRow()}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogGetRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, taskID, options, chosen, rationale, assumptions, outcome,
		createdAt, decisionStatus, revisit, supersedes, subject, author, policy,
		decodeErr := db2contract.DecodeDecisionLogGetReply(body)
	if decodeErr != nil || found != 1 || taskID != 9 ||
		options != "keep it | rewrite it" || chosen != "keep it" ||
		rationale != "the rewrite costs more than it saves" ||
		assumptions != "assumes traffic stays flat" || outcome != "held" ||
		createdAt == "" || decisionStatus != "active" || revisit != "2026-06-01" ||
		supersedes != 0 || subject != "storage" || author != "jbailes" ||
		policy != 0 {
		t.Fatalf("decision = %d %d %q %q", found, taskID, chosen, subject)
	}
}

func TestDecisionListsFilterWithoutBranching(t *testing.T) {
	// The C builds its statement text per filter combination; the predicates
	// say the same thing once.
	store := &fakeStore{rows: &fakeRows{values: [][]any{decisionLogFullRow()}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogListRequest("held", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeDecisionLogListReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].DecisionID != 4 ||
		found[0].Outcome != "held" || found[0].DecisionSubject != "storage" {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "($2 = '' OR outcome = $2)") {
		t.Errorf("the outcome filter changed shape: %q", store.lastSQL)
	}
	if store.lastArgs[1] != "held" {
		t.Errorf("the filter was not passed: %v", store.lastArgs)
	}

	store = &fakeStore{rows: &fakeRows{}}
	handler = NewDispatchHandler(store)
	scopedRequest, err := db2contract.EncodeDecisionLogListScopedRequest(
		"storage", "active", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDecisionLogListScoped), scopedRequest); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "($2 = '' OR subject = $2)") ||
		!strings.Contains(store.lastSQL, "($3 = '' OR status = $3)") {
		t.Errorf("the scoped filters changed shape: %q", store.lastSQL)
	}
}

func TestProposalGetSaysWhatBecameOfIt(t *testing.T) {
	// A proposal that was committed has a committed-at stamp, one that was
	// dropped has an archive reason, and one still pending has neither.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(7), "rules", "committed", "build-state", int64(4),
		`{"op":"reinforce"}`, `["signal:7"]`, int64(2), "", "2026-02-01T00:00:00Z",
		"", "2026-01-01T00:00:00Z", "2026-02-01T00:00:00Z",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalGetRequest(31)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLearningProposalGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, signalID, sink, state, targetKey, targetMemory, action, evidence,
		corroboration, expires, committed, archive, created, updated, decodeErr :=
		db2contract.DecodeLearningProposalGetReply(body)
	if decodeErr != nil || found != 1 || signalID != 7 || sink != "rules" ||
		state != "committed" || targetKey != "build-state" || targetMemory != 4 ||
		action != `{"op":"reinforce"}` || evidence != `["signal:7"]` ||
		corroboration != 2 || expires != "" || committed == "" ||
		archive != "" || created == "" || updated == "" {
		t.Fatalf("proposal = %d %d %q %q", found, signalID, sink, state)
	}
}

func TestUnknownProposalReportsItself(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalGetRequest(2147483000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLearningProposalGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, _, sink, _, _, _, _, _, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeLearningProposalGetReply(body)
	if decodeErr != nil || found != 0 || sink != "" {
		t.Fatalf("found = %d, sink = %q", found, sink)
	}
}

func TestTaskListLeadsWithWhatMoved(t *testing.T) {
	// A task list is read to see what is being worked on, and the last thing
	// that moved is the most interesting -- so the ordering is by update rather
	// than creation, unlike the subtask read.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(0), "write it down", "doing", 1.0, "session-1",
			"2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskListRequest("doing", "session-1", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	tasks, decodeErr := db2contract.DecodeTaskListReply(body)
	if decodeErr != nil || len(tasks) != 1 || tasks[0].TaskRowID != 4 ||
		tasks[0].TaskState != "doing" || tasks[0].TaskSessionID != "session-1" {
		t.Fatalf("tasks = %+v", tasks)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY updated_at DESC") {
		t.Errorf("the list no longer leads with what moved: %q", store.lastSQL)
	}
	if strings.Contains(taskSubtasksQuery, "ORDER BY updated_at") {
		t.Errorf("the subtask read now orders by update: %q", taskSubtasksQuery)
	}
	if store.lastArgs[1] != "doing" || store.lastArgs[2] != "session-1" {
		t.Errorf("the filters were not passed: %v", store.lastArgs)
	}
}
