package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// decisionRow is the fourteen columns the read-back selects.
// task_id and outcome are nullable, so the scan targets are pointers and the
// scripted values are too. nil for outcome is the case that matters: a row the
// database has not given an outcome, which is every row a write just made.
func decisionRow(id int64, subject string) []any {
	taskID := int64(0)
	return []any{
		id, &taskID, "options", "chosen", "rationale", "", (*string)(nil),
		"2026-08-22T09:00:00Z", "active", "", int64(0), subject, "author", int64(0),
	}
}

// A write and the read-back of what it wrote scan different widths, so the
// queue serves the insert's single column and then the row.
func decisionWriteThenRead(id int64, subject string) []*fakeRow {
	return []*fakeRow{
		{values: []any{id}},
		{values: decisionRow(id, subject)},
	}
}

func TestDecisionLogInsertPicksTheStatementFromTheTimestamp(t *testing.T) {
	// An empty timestamp is absence, not an empty value: it selects the
	// statement that lets the database stamp the row, so a caller wanting the
	// real time of the decision sends nothing rather than its own clock.
	for _, testCase := range []struct {
		name       string
		createdAt  string
		wantStamps bool
	}{
		{"database stamps", "", true},
		{"caller stamps", "2026-01-01T00:00:00Z", false},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{rowQueue: decisionWriteThenRead(1, "")}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeDecisionLogInsertRequest(
				4242, "options", "chosen", "rationale", "assumptions", testCase.createdAt)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(
				invocation(db2contract.StageDecisionLogInsert), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			// The first statement is the insert; lastSQL by now is the
			// read-back that followed it.
			stamps := strings.Contains(store.sqlLog[0], "pg_now_text()")
			if stamps != testCase.wantStamps {
				t.Fatalf("database stamps = %v, want %v", stamps, testCase.wantStamps)
			}
		})
	}
}

func TestDecisionLogInsertSendsNoTimestampWhenGivenNone(t *testing.T) {
	// Captured from the insert itself rather than the read-back after it: a
	// six-argument insert would mean the empty string was passed through as a
	// value instead of selecting the stamping statement.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(1)}}}}
	request, err := db2contract.EncodeDecisionLogInsertRequest(
		4242, "options", "chosen", "rationale", "assumptions", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	decisionLogInsert(t.Context(), store, request)
	if len(store.argsLog) == 0 {
		t.Fatal("no statement ran")
	}
	if len(store.argsLog[0]) != 5 {
		t.Fatalf("insert args = %d, want 5 with the database stamping",
			len(store.argsLog[0]))
	}
	if !strings.Contains(store.sqlLog[0], "pg_now_text()") {
		t.Error("an empty timestamp did not select the stamping statement")
	}
}

func TestDecisionLogInsertPassesACallerTimestampThrough(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(1)}}}}
	request, err := db2contract.EncodeDecisionLogInsertRequest(
		4242, "options", "chosen", "rationale", "assumptions", "2026-01-01T00:00:00Z")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	decisionLogInsert(t.Context(), store, request)
	if len(store.argsLog) == 0 || len(store.argsLog[0]) != 6 {
		t.Fatalf("insert args = %v, want 6 with the caller stamping", store.argsLog)
	}
	if strings.Contains(store.sqlLog[0], "pg_now_text()") {
		t.Error("a supplied timestamp still selected the stamping statement")
	}
}

func TestDecisionLogInsertReturnsTheRowItWrote(t *testing.T) {
	store := &fakeStore{rowQueue: decisionWriteThenRead(42, "")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogInsertRequest(
		4242, "options", "chosen", "rationale", "assumptions", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, id, _, options, _, _, _, _, _, statusText, _, _, _, _, _, decodeErr :=
		db2contract.DecodeDecisionLogInsertReply(body)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	// The status comes back from the row, not from the request: the caller never
	// sent it, and a write that returns what it was given proves nothing.
	if acknowledged != 1 || id != 42 || options != "options" || statusText != "active" {
		t.Fatalf("acknowledged=%d id=%d options=%q status=%q",
			acknowledged, id, options, statusText)
	}
}

func TestDecisionLogRecordRefusesAnEmptySubject(t *testing.T) {
	// An empty subject would be one global slot rather than a scope.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogRecordRequest(
		"", "options", "chosen", "rationale", "author", 0, "", 0)
	if err != nil {
		t.Skipf("the schema refuses an empty subject at encode, which is better: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, _, _, _, _, _, _, _, _, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeDecisionLogRecordReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, err = %v", acknowledged, decodeErr)
	}
	if store.txCalls != 0 {
		t.Fatal("an empty subject opened a transaction")
	}
}

func TestDecisionLogRecordSupersedesInsideOneTransaction(t *testing.T) {
	store := &fakeStore{rowQueue: decisionWriteThenRead(9, "subject")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogRecordRequest(
		"subject", "options", "chosen", "rationale", "author", 0, "", 5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDecisionLogRecord), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if store.execCalls != 1 {
		t.Fatalf("supersede statements = %d, want 1", store.execCalls)
	}
}

func TestDecisionLogRecordRollsBackWhenTheSupersedeMissed(t *testing.T) {
	// The UPDATE succeeds and changes nothing when the identifier names no
	// active decision in this subject. The insert must not proceed: a decision
	// that supersedes nothing is a second active decision for the scope, which
	// is what the count check exists to prevent.
	store := &fakeStore{
		rowQueue:   decisionWriteThenRead(9, "subject"),
		execRowsAt: true,
		execRows:   0,
	}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogRecordRequest(
		"subject", "options", "chosen", "rationale", "author", 0, "", 5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, _, _, _, _, _, _, _, _, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeDecisionLogRecordReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
}

func TestDecisionLogRecordSkipsTheSupersedeWhenNoneIsNamed(t *testing.T) {
	store := &fakeStore{rowQueue: decisionWriteThenRead(9, "subject")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogRecordRequest(
		"subject", "options", "chosen", "rationale", "author", 0, "", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	handler(invocation(db2contract.StageDecisionLogRecord), request)
	if store.execCalls != 0 {
		t.Fatal("a record superseding nothing still ran the supersede")
	}
	if !store.committed {
		t.Fatal("the transaction did not commit")
	}
}

func TestDecisionLogReadBackFailureIsUnacknowledged(t *testing.T) {
	store := &fakeStore{row: &fakeRow{err: errors.New("gone")}}
	body, status := decisionLogReadBack(t.Context(), store, 1,
		db2contract.EncodeDecisionLogInsertReply)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, _, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
		db2contract.DecodeDecisionLogInsertReply(body)
	if err != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, err = %v", acknowledged, err)
	}
}
