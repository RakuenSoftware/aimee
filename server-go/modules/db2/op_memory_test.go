package db2

import (
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func prospectiveRow() []any {
	return []any{
		int64(1), "replay trigger", "replay action", "replay-entity", "replay/file.c",
		"once", "armed", "", "replay-session", int32(0), "",
		"2026-08-22 10:56:17", "2026-08-22T10:56:17Z",
	}
}

func TestProspectiveGetReadsARow(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: prospectiveRow()}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveGetRequest(1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, trigger, action, entity, file, recurrence, state, validUntil,
		session, count, lastTriggered, created, updated, decodeErr :=
		db2contract.DecodeProspectiveGetReply(body)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	if found != 1 || trigger != "replay trigger" || action != "replay action" ||
		entity != "replay-entity" || file != "replay/file.c" || recurrence != "once" ||
		state != "armed" || validUntil != "" || session != "replay-session" || count != 0 ||
		lastTriggered != "" || created == "" || updated == "" {
		t.Error("a field did not survive the read")
	}
}

func TestProspectiveGetAbsenceIsAnAnswer(t *testing.T) {
	// No row is ModuleStatusOK with the found flag clear, not an error. A caller
	// asking whether a prospective exists is entitled to be told no.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeProspectiveGetRequest(404)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want ok", status)
	}
	found, _, _, _, _, _, _, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeProspectiveGetReply(body)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	if found != 0 {
		t.Fatalf("found = %d, want 0", found)
	}
}

func TestProspectiveGetReportsARealFailure(t *testing.T) {
	// A scan failure is not absence. The two answer differently, which is the
	// whole reason absence has a flag of its own.
	store := &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveGetRequest(1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageProspectiveGet), request); status !=
		bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

func TestProspectiveInsertReturnsTheNewIdentifier(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: []any{int64(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveInsertRequest(
		"trigger", "action", "entity", "file.c", "once", "", "session")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeProspectiveInsertReply(body)
	if decodeErr != nil || id != 7 {
		t.Fatalf("id = %d, err = %v", id, decodeErr)
	}
	// Every argument reaches the statement; the state is a literal in the SQL
	// because a prospective is always armed when it is created.
	if len(store.lastArgs) != 7 || store.lastArgs[0] != "trigger" ||
		store.lastArgs[6] != "session" {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestProspectiveRecordTriggerPicksTheStatementFromTerminal(t *testing.T) {
	// Terminal is the caller's judgement, not the row's: the recurrence column
	// is not consulted by either statement, so the same prospective is one-shot
	// or recurring depending on what the caller passes.
	for _, testCase := range []struct {
		name     string
		terminal uint32
		wantSQL  string
	}{
		{"counts", 0, prospectiveTriggerCountQuery},
		{"closes", 1, prospectiveTriggerTerminalQuery},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeProspectiveRecordTriggerRequest(1, testCase.terminal)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(db2contract.StageProspectiveRecordTrigger), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, decodeErr := db2contract.DecodeProspectiveRecordTriggerReply(body)
			if decodeErr != nil || acknowledged != 1 {
				t.Fatalf("acknowledged = %d, err = %v", acknowledged, decodeErr)
			}
			if store.lastSQL != testCase.wantSQL {
				t.Errorf("ran the wrong statement for terminal=%d", testCase.terminal)
			}
		})
	}
}

func TestProspectiveRecordTriggerReportsAFailedStatement(t *testing.T) {
	// The acknowledgement means the statement ran. An identifier naming nothing
	// still runs it and still acknowledges -- but a statement that could not run
	// answers zero, which is the only failure this reply can carry.
	store := &fakeStore{execErr: errors.New("no connection")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveRecordTriggerRequest(1, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveRecordTrigger), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeProspectiveRecordTriggerReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, err = %v", acknowledged, decodeErr)
	}
}
