package db2

import (
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

// directiveRow is the nineteen columns ED_SELECT_COLS names.
func directiveRow() []any {
	return []any{
		int64(1), "replay question?", "replay-topic", "replay-entity", "replay/file.c",
		"retrieval_failure", int32(5), "open", int64(0), int64(0), int64(0),
		"replay-evidence", "replay-session", int32(0), "", "", "",
		"2026-08-22 08:59:14", "2026-08-22 08:59:14",
	}
}

func TestDirectiveFindReturnsTheDirective(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: directiveRow()}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveFindByCauseTopicRequest(
		"retrieval_failure", "replay-topic")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveFindByCauseTopic), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, id, question, entity, file, priority, state, _, _, _, evidence, session,
		surfaced, _, _, _, created, _, decodeErr :=
		db2contract.DecodeDirectiveFindByCauseTopicReply(body)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	if found != 1 || id != 1 || question != "replay question?" || entity != "replay-entity" ||
		file != "replay/file.c" || priority != 5 || state != "open" ||
		evidence != "replay-evidence" || session != "replay-session" || surfaced != 0 ||
		created == "" {
		t.Error("a field did not survive the read")
	}
}

func TestDirectiveFindAbsenceIsAnAnswer(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeDirectiveFindByCauseTopicRequest("nothing", "nothing")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveFindByCauseTopic), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want ok", status)
	}
	found, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
		db2contract.DecodeDirectiveFindByCauseTopicReply(body)
	if err != nil || found != 0 {
		t.Fatalf("found = %d, err = %v", found, err)
	}
}

func TestDirectiveInsertReportsAFreshInsert(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(7)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveInsertIgnoreRequest(
		"question?", "topic", "entity", "file.c", "retrieval_failure", 5, 0, 0,
		"evidence", "session", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveInsertIgnore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, id, existed, decodeErr :=
		db2contract.DecodeDirectiveInsertIgnoreReply(body)
	if decodeErr != nil || acknowledged != 1 || id != 7 || existed != 0 {
		t.Fatalf("acknowledged=%d id=%d existed=%d err=%v",
			acknowledged, id, existed, decodeErr)
	}
	// Only the insert ran: a fresh insert never needs the lookup.
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %d, want 1", len(store.sqlLog))
	}
}

func TestDirectiveInsertFindsTheExistingOneOnAConflict(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},      // ON CONFLICT DO NOTHING declined
		{values: []any{int64(3)}}, // the natural-key lookup found it
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveInsertIgnoreRequest(
		"question?", "topic", "", "", "retrieval_failure", 5, 0, 0, "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveInsertIgnore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, id, existed, decodeErr :=
		db2contract.DecodeDirectiveInsertIgnoreReply(body)
	if decodeErr != nil || acknowledged != 1 || id != 3 || existed != 1 {
		t.Fatalf("acknowledged=%d id=%d existed=%d", acknowledged, id, existed)
	}
}

func TestDirectiveInsertCanReportExistedWithNoIdentifier(t *testing.T) {
	// The lookup is by cause, topic and question -- NOT the index that refused
	// the insert. A contradiction deduped by its two memories can carry a
	// different question, so the lookup finds nothing and the caller learns the
	// directive exists without learning which one it is.
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows}, // the insert was declined
		{err: pgx.ErrNoRows}, // and the natural key does not name it
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveInsertIgnoreRequest(
		"a different question?", "", "", "", "contradiction", 5, 11, 22, "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveInsertIgnore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, id, existed, decodeErr :=
		db2contract.DecodeDirectiveInsertIgnoreReply(body)
	if decodeErr != nil || acknowledged != 1 || id != 0 || existed != 1 {
		t.Fatalf("acknowledged=%d id=%d existed=%d -- the gap this records is closed",
			acknowledged, id, existed)
	}
}

func TestDirectiveInsertRefusesWithoutAQuestionOrCause(t *testing.T) {
	for _, testCase := range []struct {
		name     string
		question string
		cause    string
	}{
		{"no question", "", "retrieval_failure"},
		{"no cause", "question?", ""},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeDirectiveInsertIgnoreRequest(
				testCase.question, "", "", "", testCase.cause, 0, 0, 0, "", "", "")
			if err != nil {
				t.Skipf("the schema refuses this at encode, which is better: %v", err)
			}
			body, status := handler(invocation(db2contract.StageDirectiveInsertIgnore), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, _, _, decodeErr :=
				db2contract.DecodeDirectiveInsertIgnoreReply(body)
			if decodeErr != nil || acknowledged != 0 {
				t.Fatalf("acknowledged = %d, want 0", acknowledged)
			}
			if len(store.sqlLog) != 0 {
				t.Fatal("an incoherent request reached the store")
			}
		})
	}
}

func TestDirectiveInsertReportsARealFailure(t *testing.T) {
	// A failure that is not a conflict must not read as one: ON CONFLICT DO
	// NOTHING answers no rows when it declines, and anything else is a fault.
	store := &fakeStore{rowQueue: []*fakeRow{{err: errors.New("connection lost")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveInsertIgnoreRequest(
		"question?", "topic", "", "", "retrieval_failure", 5, 0, 0, "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveInsertIgnore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, _, existed, decodeErr :=
		db2contract.DecodeDirectiveInsertIgnoreReply(body)
	if decodeErr != nil || acknowledged != 0 || existed != 0 {
		t.Fatalf("acknowledged=%d existed=%d -- a fault read as a conflict",
			acknowledged, existed)
	}
}
