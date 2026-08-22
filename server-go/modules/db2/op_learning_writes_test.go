package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestApproveAndRetireBumpTheEpochInTheSameTransaction(t *testing.T) {
	// Agents re-read the active rule set only when the epoch moves, so a status
	// change that lands without its bump leaves a rule enforced and invisible --
	// and it does not resolve on its own. The C runs the two statements
	// unwrapped.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		state string
	}{
		{
			"collab_rule_approve",
			db2contract.StageCollabRuleApprove,
			func() ([]byte, error) { return db2contract.EncodeCollabRuleApproveRequest(4) },
			"'active'",
		},
		{
			"collab_rule_retire",
			db2contract.StageCollabRuleRetire,
			func() ([]byte, error) { return db2contract.EncodeCollabRuleRetireRequest(4) },
			"'retired'",
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if store.txCalls != 1 || !store.committed {
				t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
			}
			if len(store.sqlLog) != 2 {
				t.Fatalf("statements = %d, want the transition and the epoch bump",
					len(store.sqlLog))
			}
			if !strings.Contains(store.sqlLog[0], testCase.state) {
				t.Errorf("the transition does not set %s: %q", testCase.state, store.sqlLog[0])
			}
			if !strings.Contains(store.sqlLog[1], "collab_rules_meta") {
				t.Errorf("the epoch was not bumped: %q", store.sqlLog[1])
			}
		})
	}
}

func TestATransitionThatMatchesNoRowBumpsNothing(t *testing.T) {
	// The row count is the outcome, not statement success. Bumping the epoch for
	// a transition that missed would tell every agent to re-read a set that has
	// not moved.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCollabRuleApproveRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCollabRuleApprove), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeCollabRuleApproveReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %d, want the failed transition alone", len(store.sqlLog))
	}
}

func TestApproveEnforcesTheActiveCapInTheStatement(t *testing.T) {
	// The C counts, compares, then writes -- a check that has stopped being true
	// by the time it is acted on. Folding it into the WHERE makes the two agree
	// at one point in time. Every active rule is injected into an agent's
	// context, so the ceiling is a prompt budget, not a storage limit.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCollabRuleApproveRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCollabRuleApprove), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.sqlLog[0],
		"(SELECT COUNT(*) FROM collab_rules WHERE status = 'active') < $2") {
		t.Errorf("the cap is not part of the statement: %q", store.sqlLog[0])
	}
	if len(store.argsLog[0]) != 2 || store.argsLog[0][1] != maxActiveCollabRules {
		t.Fatalf("args = %v -- the cap is not bound", store.argsLog[0])
	}
	if maxActiveCollabRules != 10 {
		t.Fatalf("the cap is %d; COLLAB_MAX_ACTIVE_RULES is 10 and every active rule "+
			"is injected into an agent's context", maxActiveCollabRules)
	}
}

func TestRejectDoesNotBumpTheEpoch(t *testing.T) {
	// A proposal was never in the active set, so rejecting it changes nothing an
	// agent is holding. Bumping would make every agent re-read an unchanged set.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCollabRuleRejectRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCollabRuleReject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	changed, decodeErr := db2contract.DecodeCollabRuleRejectReply(body)
	if decodeErr != nil || changed != 1 {
		t.Fatalf("changed = %d", changed)
	}
	if store.txCalls != 0 {
		t.Error("reject opened a transaction it has no second statement for")
	}
	if len(store.sqlLog) != 1 || strings.Contains(store.sqlLog[0], "collab_rules_meta") {
		t.Errorf("statements = %v", store.sqlLog)
	}
	if !strings.Contains(store.lastSQL, "status = 'proposed'") {
		t.Error("reject is not restricted to a proposed rule")
	}
}

func TestRejectRequiresARowToHaveChanged(t *testing.T) {
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCollabRuleRejectRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCollabRuleReject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	changed, decodeErr := db2contract.DecodeCollabRuleRejectReply(body)
	if decodeErr != nil || changed != 0 {
		t.Fatalf("changed = %d, want 0", changed)
	}
}

func TestMarkCommittedIsRepeatableAndUnconditional(t *testing.T) {
	// It carries no state predicate and acknowledges the statement rather than a
	// row, so an already-committed proposal and an identifier nothing holds both
	// acknowledge. That is what makes it safe for a caller to retry; adding
	// either check would break the second attempt.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProposalMarkCommittedRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProposalMarkCommitted), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeProposalMarkCommittedReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d -- a row count crept into the answer", acknowledged)
	}
	if strings.Contains(store.lastSQL, "state =") &&
		!strings.Contains(store.lastSQL, "SET state = 'committed'") {
		t.Errorf("a state predicate was added: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "WHERE id = $1") ||
		strings.Contains(store.lastSQL, "WHERE id = $1 AND") {
		t.Errorf("the WHERE was narrowed: %q", store.lastSQL)
	}
}

func TestBumpCorroborationOnlyWhilePending(t *testing.T) {
	// Corroborating a decided proposal would make a committed one look like it
	// is still gathering evidence.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProposalBumpCorroborationRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageProposalBumpCorroboration), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "state = 'pending'") {
		t.Errorf("the bump is not restricted to a pending proposal: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "corroboration_count = corroboration_count + 1") {
		t.Errorf("the count is not incremented in place: %q", store.lastSQL)
	}
}

func TestRuleDeletesReportWhatTheyRemoved(t *testing.T) {
	// By id: a yes-or-no, because the identifier is a primary key. By directive
	// type: a count, because the caller has no other way to know how much of the
	// rule set just disappeared. Both matter because neither can invalidate the
	// in-process cache the C invalidates here.
	t.Run("by_id", func(t *testing.T) {
		store := &fakeStore{execRowsAt: true, execRows: 1}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeRulesDeleteByIDRequest(4)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageRulesDeleteByID), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		deleted, decodeErr := db2contract.DecodeRulesDeleteByIDReply(body)
		if decodeErr != nil || deleted != 1 {
			t.Fatalf("deleted = %d", deleted)
		}
	})

	t.Run("by_id_absent", func(t *testing.T) {
		store := &fakeStore{execRowsAt: true, execRows: 0}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeRulesDeleteByIDRequest(4)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageRulesDeleteByID), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		deleted, decodeErr := db2contract.DecodeRulesDeleteByIDReply(body)
		if decodeErr != nil || deleted != 0 {
			t.Fatalf("deleted = %d, want 0", deleted)
		}
	})

	t.Run("by_directive_type_counts", func(t *testing.T) {
		store := &fakeStore{execRowsAt: true, execRows: 7}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeRulesDeleteByDirectiveTypeRequest("hard")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(
			invocation(db2contract.StageRulesDeleteByDirectiveType), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		deleted, decodeErr := db2contract.DecodeRulesDeleteByDirectiveTypeReply(body)
		if decodeErr != nil || deleted != 7 {
			t.Fatalf("deleted = %d, want the count rather than a flag", deleted)
		}
	})
}
