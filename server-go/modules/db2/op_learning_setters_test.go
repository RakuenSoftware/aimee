package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestDecisionLogSettersRequireTheDecisionToExist(t *testing.T) {
	// Setting a field on a decision nobody logged is the caller naming the
	// wrong decision. That is the opposite of proposal_archive beside it, where
	// a repeat is a retry -- the two shapes sit next to each other and the
	// difference is easy to erase in either direction.
	for _, testCase := range []struct {
		name   string
		stage  uint32
		build  func() ([]byte, error)
		decode func([]byte) (uint32, error)
		column string
	}{
		{
			"decision_log_set_outcome",
			db2contract.StageDecisionLogSetOutcome,
			func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetOutcomeRequest(4, "worked")
			},
			db2contract.DecodeDecisionLogSetOutcomeReply,
			"outcome",
		},
		{
			"decision_log_set_status",
			db2contract.StageDecisionLogSetStatus,
			func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetStatusRequest(4, "retired")
			},
			db2contract.DecodeDecisionLogSetStatusReply,
			"status",
		},
		{
			"decision_log_set_revisit",
			db2contract.StageDecisionLogSetRevisit,
			func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetRevisitRequest(4, "2026-06-01")
			},
			db2contract.DecodeDecisionLogSetRevisitReply,
			"revisit_when",
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}

			matched := &fakeStore{}
			body, status := NewDispatchHandler(matched)(
				invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, decodeErr := testCase.decode(body)
			if decodeErr != nil || acknowledged != 1 {
				t.Fatalf("acknowledged = %d for a decision that exists", acknowledged)
			}
			if !strings.Contains(matched.lastSQL, testCase.column+" = $2") {
				t.Errorf("the wrong column is written: %q", matched.lastSQL)
			}

			missing := &fakeStore{execRowsAt: true, execRows: 0}
			body, status = NewDispatchHandler(missing)(
				invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, decodeErr = testCase.decode(body)
			if decodeErr != nil || acknowledged != 0 {
				t.Fatalf("acknowledged = %d for a decision nobody logged, want 0",
					acknowledged)
			}
		})
	}
}

func TestDecisionLogSetStatusImposesNoStateMachine(t *testing.T) {
	// The uniqueness of an active decision per scope belongs to
	// idx_dl_active_scope. Restating it here would put the rule in a second
	// place where it could disagree with the index.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDecisionLogSetStatusRequest(4, "active")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDecisionLogSetStatus), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "AND status") ||
		strings.Contains(store.lastSQL, "subject") {
		t.Errorf("a predicate duplicating the index was added: %q", store.lastSQL)
	}
}

func TestProposalArchiveIsRepeatable(t *testing.T) {
	// Matching proposal_mark_committed: no state predicate, no row check, so
	// archiving an already-archived proposal restamps the reason and still
	// acknowledges.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProposalArchiveRequest(4, "superseded upstream")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProposalArchive), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeProposalArchiveReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d -- a row count crept into the answer", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "WHERE id = $1") ||
		strings.Contains(store.lastSQL, "WHERE id = $1 AND") {
		t.Errorf("the WHERE was narrowed: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "superseded upstream" {
		t.Fatalf("args = %v -- the reason is not recorded", store.lastArgs)
	}
}

func TestBanditDecisionCloseWritesTheRewardAndTheStamp(t *testing.T) {
	// The pair moves together: a row with a reward and no closed_at would be a
	// decision nobody can date, and an open decision is one whose reward is
	// still NULL.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditDecisionCloseRequest("decision-1", 0.75)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditDecisionClose), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeBanditDecisionCloseReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "reward = $2") ||
		!strings.Contains(store.lastSQL, "closed_at =") {
		t.Errorf("the reward and the stamp do not move together: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != 0.75 {
		t.Fatalf("args = %v -- the reward is not bound as a number", store.lastArgs)
	}
}

func TestBanditDecisionCloseAcknowledgesAnUnknownDecision(t *testing.T) {
	// The C ignores the step result entirely. A caller reporting rewards
	// asynchronously depends on that: a decision that has since been pruned
	// drops its reward rather than failing the report.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditDecisionCloseRequest("pruned", 1.0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditDecisionClose), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeBanditDecisionCloseReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d, want 1", acknowledged)
	}
}

func TestRulesUpdateDirectiveTypeAcknowledgesTheStatement(t *testing.T) {
	// Unlike the rule deletes beside it, which report a count. The C
	// invalidates its cache unconditionally here, and the Go module cannot
	// reach that cache at all, so the reply says only that the statement ran.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesUpdateDirectiveTypeRequest(4, "hard")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRulesUpdateDirectiveType), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeRulesUpdateDirectiveTypeReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d, want 1", acknowledged)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "hard" {
		t.Fatalf("args = %v", store.lastArgs)
	}
}
