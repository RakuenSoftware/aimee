package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestScoreDefaultsCoverAbsenceNotAnUnsetScore(t *testing.T) {
	// Both columns are NOT NULL with defaults of their own, so a memory that
	// exists always has a number and the caller's default never stands in for
	// it. Passing a default answers "what should an unknown memory score",
	// which is a different question from what a known one scores.
	for _, testCase := range []struct {
		name   string
		stage  uint32
		build  func(id uint64, fallback float64) ([]byte, error)
		decode func([]byte) (float64, error)
	}{
		{
			"memory_salience",
			db2contract.StageMemorySalience,
			db2contract.EncodeMemorySalienceRequest,
			db2contract.DecodeMemorySalienceReply,
		},
		{
			"memory_surprise",
			db2contract.StageMemorySurprise,
			db2contract.EncodeMemorySurpriseRequest,
			db2contract.DecodeMemorySurpriseReply,
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			request, err := testCase.build(4, 0.25)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}

			// A memory that exists and scores zero keeps its zero.
			zero := 0.0
			present := &fakeStore{row: &fakeRow{values: []any{&zero}}}
			body, status := NewDispatchHandler(present)(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			score, decodeErr := testCase.decode(body)
			if decodeErr != nil || score != 0 {
				t.Fatalf("score = %v -- the default displaced a real zero", score)
			}

			// A memory nothing holds takes the caller's default.
			body, status = NewDispatchHandler(&fakeStore{})(
				invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			score, decodeErr = testCase.decode(body)
			if decodeErr != nil || score != 0.25 {
				t.Fatalf("score = %v, want the caller's default", score)
			}
		})
	}
}

func TestMarkPendingBindsItsWindowRatherThanPrintingIt(t *testing.T) {
	// The C prints the day count into the SQL text, noting it is a small
	// integer it controls. Binding it means no caller value reaches the
	// statement text at all, so that reasoning no longer has to hold.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLifecycleMarkPendingRequest(4, 14)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageLifecycleMarkPending), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "14") {
		t.Errorf("the day count reached the statement text: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "+14 days" {
		t.Fatalf("args = %v -- the window is not bound", store.lastArgs)
	}
}

func TestMarkPendingRefusesAZeroWindow(t *testing.T) {
	// A memory whose deadline is the moment it was set would be stale before
	// anyone could confirm it.
	store := &fakeStore{}
	if _, status := lifecycleMarkPending(t.Context(), store, mustEncode(t,
		func() ([]byte, error) {
			return db2contract.EncodeLifecycleMarkPendingRequest(
				4, db2contract.LifecycleMarkPendingTtlDaysMin)
		})); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	// The contract's own floor is what stops zero crossing the wire; the guard
	// in the operation is defence behind it.
	if db2contract.LifecycleMarkPendingTtlDaysMin < 1 {
		t.Fatal("a zero window can now be sent; the operation would set a " +
			"deadline in the past")
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d for a valid window", store.execCalls)
	}
}

func TestProspectiveSetStateAcceptsAnyTransition(t *testing.T) {
	// A caller cancelling a trigger that has already fired is acting on stale
	// information, not making a mistake. Failing them would turn a harmless
	// race into an error.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveSetStateRequest(4, "cancelled")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveSetState), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeProspectiveSetStateReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d -- a row count crept into the answer", acknowledged)
	}
	if strings.Contains(store.lastSQL, "AND state") {
		t.Errorf("a state predicate was added: %q", store.lastSQL)
	}
}

func TestDirectiveResolveOnlyClosesAnOpenQuestion(t *testing.T) {
	// Resolving twice would overwrite which memory answered, and the second
	// answer is not more right than the first.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveResolveRequest(4, 9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveResolve), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeDirectiveResolveReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d for a directive that was not open", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "AND state = 'open'") {
		t.Errorf("the resolve is no longer restricted to an open directive: %q",
			store.lastSQL)
	}
}

func TestOntologyEvalCountSeparatesFoundFromZero(t *testing.T) {
	// A relation seen zero times and a relation nobody has proposed are
	// different states, and the promotion threshold reads them differently.
	t.Run("proposed but never seen", func(t *testing.T) {
		store := &fakeStore{row: &fakeRow{values: []any{idPtr(0)}}}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeOntologyEvalCountRequest("worksFor")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageOntologyEvalCount), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, count, decodeErr := db2contract.DecodeOntologyEvalCountReply(body)
		if decodeErr != nil || found != 1 || count != 0 {
			t.Fatalf("found = %d, count = %d", found, count)
		}
		// Normalized before binding, since that is the form the table holds.
		if len(store.lastArgs) != 1 || store.lastArgs[0] != "works_for" {
			t.Fatalf("args = %v, want the normalized name", store.lastArgs)
		}
	})

	t.Run("never proposed", func(t *testing.T) {
		handler := NewDispatchHandler(&fakeStore{})
		request, err := db2contract.EncodeOntologyEvalCountRequest("never_proposed")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageOntologyEvalCount), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, count, decodeErr := db2contract.DecodeOntologyEvalCountReply(body)
		if decodeErr != nil || found != 0 || count != 0 {
			t.Fatalf("found = %d, count = %d", found, count)
		}
	})
}

func TestDecisionLogActiveIDMatchesTheIndex(t *testing.T) {
	// Reading through the same columns idx_dl_active_scope is built on is what
	// keeps this a lookup rather than a scan.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogActiveIDRequest("deploy-target", 3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogActiveID), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeDecisionLogActiveIDReply(body)
	if decodeErr != nil || id != 7 {
		t.Fatalf("id = %d", id)
	}
	if !strings.Contains(store.lastSQL,
		"WHERE subject = $1 AND linked_policy_id = $2 AND status = 'active'") {
		t.Errorf("the lookup no longer matches the index: %q", store.lastSQL)
	}
}

func TestDecisionLogActiveIDAnswersZeroWhenNoneIsInForce(t *testing.T) {
	// Zero is how a caller learns it is free to record one.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeDecisionLogActiveIDRequest("untouched", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDecisionLogActiveID), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeDecisionLogActiveIDReply(body)
	if decodeErr != nil || id != 0 {
		t.Fatalf("id = %d, want 0", id)
	}
}
