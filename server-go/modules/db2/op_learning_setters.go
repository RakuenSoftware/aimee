package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageDecisionLogSetOutcome,
		db2contract.OperationDecisionLogSetOutcome, decisionLogSetOutcome)
	Register(db2contract.StageDecisionLogSetStatus,
		db2contract.OperationDecisionLogSetStatus, decisionLogSetStatus)
	Register(db2contract.StageDecisionLogSetRevisit,
		db2contract.OperationDecisionLogSetRevisit, decisionLogSetRevisit)
	Register(db2contract.StageProposalArchive,
		db2contract.OperationProposalArchive, proposalArchive)
	Register(db2contract.StageRulesUpdateDirectiveType,
		db2contract.OperationRulesUpdateDirectiveType, rulesUpdateDirectiveType)
	Register(db2contract.StageBanditDecisionClose,
		db2contract.OperationBanditDecisionClose, banditDecisionClose)
}

// The three decision-log setters each write one column of one row, and all
// three require the row to have existed: the C returns failure when nothing
// changed. That is the opposite of proposal_mark_committed's deliberate
// repeatability, and the difference is real -- setting a field on a decision
// nobody logged is the caller naming the wrong decision, where re-committing an
// already-committed proposal is a retry.
const (
	decisionLogSetOutcomeQuery = `UPDATE decision_log SET outcome = $2 WHERE id = $1`
	decisionLogSetStatusQuery  = `UPDATE decision_log SET status = $2 WHERE id = $1`
	decisionLogSetRevisitQuery = `UPDATE decision_log SET revisit_when = $2 WHERE id = $1`
)

// setDecisionLogField applies one column update that must find its row.
func setDecisionLogField(ctx context.Context, store Store, query string,
	decisionID uint64, value string, encode func(uint32) ([]byte, error),
) ([]byte, bus.ModuleStatus) {
	changed, err := store.Exec(ctx, query, int64(decisionID), value)
	return acknowledgement(err == nil && changed > 0, encode)
}

// decisionLogSetOutcome records how a logged decision turned out.
func decisionLogSetOutcome(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, outcome, err := db2contract.DecodeDecisionLogSetOutcomeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return setDecisionLogField(ctx, store, decisionLogSetOutcomeQuery, decisionID, outcome,
		db2contract.EncodeDecisionLogSetOutcomeReply)
}

// decisionLogSetStatus moves a logged decision between active and retired.
//
// No state machine here: any status replaces any other. What stops two active
// decisions sharing a scope is idx_dl_active_scope, unique on subject and
// linked_policy_id where the status is active and the subject is non-empty --
// that last clause being why a plain unscoped decision can be logged more than
// once. Restating any of it as a predicate here would put the rule in a second
// place where it could disagree with the index.
//
// Written out rather than quoted because gofmt rewrites a doubled apostrophe in
// a doc comment into a typographic quote, which would leave a non-ASCII
// character in the file every time it ran.
func decisionLogSetStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, status, err := db2contract.DecodeDecisionLogSetStatusRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return setDecisionLogField(ctx, store, decisionLogSetStatusQuery, decisionID, status,
		db2contract.EncodeDecisionLogSetStatusReply)
}

// decisionLogSetRevisit records when a decision should be looked at again.
func decisionLogSetRevisit(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, when, err := db2contract.DecodeDecisionLogSetRevisitRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return setDecisionLogField(ctx, store, decisionLogSetRevisitQuery, decisionID, when,
		db2contract.EncodeDecisionLogSetRevisitReply)
}

const proposalArchiveQuery = `UPDATE learning_proposals
 SET state = 'archived', archive_reason = $2,
     updated_at = pg_now_text()
 WHERE id = $1`

// proposalArchive retires a proposal without acting on it, recording why.
//
// No state predicate and no row check, matching its sibling
// proposal_mark_committed: archiving an already-archived proposal restamps the
// reason and still acknowledges, which is what makes it safe to retry. The
// reason is stored rather than validated -- it is for a person reading later,
// not for anything that branches on it.
func proposalArchive(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	proposalID, reason, err := db2contract.DecodeProposalArchiveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, proposalArchiveQuery, int64(proposalID), reason)
	return acknowledgement(execErr == nil, db2contract.EncodeProposalArchiveReply)
}

const rulesUpdateDirectiveTypeQuery = `UPDATE rules SET directive_type = $2 WHERE id = $1`

// rulesUpdateDirectiveType changes how strongly a rule is applied.
//
// Acknowledges the statement rather than a changed row, which is the C
// behaviour and differs from the deletes beside it: those report a count
// because the caller has to invalidate a cache on a real change, and this one
// invalidates unconditionally in the C. The Go module cannot reach that cache
// either way -- see the note on the rule deletes -- so the reply is the only
// signal a caller gets, and it says the statement ran.
func rulesUpdateDirectiveType(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	ruleID, directiveType, err := db2contract.DecodeRulesUpdateDirectiveTypeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, rulesUpdateDirectiveTypeQuery, int64(ruleID), directiveType)
	return acknowledgement(execErr == nil, db2contract.EncodeRulesUpdateDirectiveTypeReply)
}

const banditDecisionCloseQuery = `UPDATE bandit_decisions
 SET reward = $2,
     closed_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
 WHERE id = $1`

// banditDecisionClose records the reward a decision earned.
//
// reward is nullable in the schema and this always writes a number, so an open
// decision is one with a NULL reward and a closed one is never NULL. That is
// what closed_at being set alongside it means: the pair moves together, and a
// row with a reward but no closed_at would be a decision nobody can date.
//
// It acknowledges the statement rather than a changed row, matching the C,
// which ignores the step result entirely. A decision id nothing holds therefore
// acknowledges -- the reward is simply dropped, which is the behaviour a
// caller reporting rewards asynchronously already depends on.
//
// The timestamp is ISO-8601 with a Z, which is what now_utc produces here and
// is not the space-separated form the ontology tables use.
func banditDecisionClose(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, reward, err := db2contract.DecodeBanditDecisionCloseRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, banditDecisionCloseQuery, decisionID, reward)
	return acknowledgement(execErr == nil, db2contract.EncodeBanditDecisionCloseReply)
}
