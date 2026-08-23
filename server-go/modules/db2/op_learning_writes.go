package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCollabRuleApprove,
		db2contract.OperationCollabRuleApprove, collabRuleApprove)
	Register(db2contract.StageCollabRuleReject,
		db2contract.OperationCollabRuleReject, collabRuleReject)
	Register(db2contract.StageCollabRuleRetire,
		db2contract.OperationCollabRuleRetire, collabRuleRetire)
	Register(db2contract.StageProposalMarkCommitted,
		db2contract.OperationProposalMarkCommitted, proposalMarkCommitted)
	Register(db2contract.StageProposalBumpCorroboration,
		db2contract.OperationProposalBumpCorroboration, proposalBumpCorroboration)
	Register(db2contract.StageRulesDeleteByID,
		db2contract.OperationRulesDeleteByID, rulesDeleteByID)
	Register(db2contract.StageRulesDeleteByDirectiveType,
		db2contract.OperationRulesDeleteByDirectiveType, rulesDeleteByDirectiveType)
}

// maxActiveCollabRules mirrors COLLAB_MAX_ACTIVE_RULES. Every active rule is
// injected into an agent's context, so the ceiling is a budget rather than a
// storage limit: raising it here would silently lengthen every prompt.
const maxActiveCollabRules = 10

// The cap is inside the UPDATE rather than a count read before it. The C reads
// the count, compares, and then writes, which is a check that has stopped being
// true by the time it is acted on. Folding it into the statement's WHERE makes
// the two agree at one point in time instead of two.
//
// It does not make concurrent approvals safe. At READ COMMITTED two statements
// can each see nine active rules and each admit one, leaving eleven. Closing
// that needs a lock or SERIALIZABLE, neither of which the C takes either; this
// is strictly tighter than what it replaces and no more than that. The wire
// reply cannot tell a caller which of the two refusals it met, and neither
// could the C: both answer zero.
const collabRuleApproveQuery = `UPDATE collab_rules
 SET status = 'active', decided_at = pg_now_text()
 WHERE id = $1 AND status = 'proposed'
 AND (SELECT COUNT(*) FROM collab_rules WHERE status = 'active') < $2`

const collabRuleRejectQuery = `UPDATE collab_rules
 SET status = 'rejected', decided_at = pg_now_text()
 WHERE id = $1 AND status = 'proposed'`

const collabRuleRetireQuery = `UPDATE collab_rules
 SET status = 'retired', decided_at = pg_now_text()
 WHERE id = $1 AND status = 'active'`

// collabRulesEpochBumpQuery advances the number agents compare against to learn
// the active rule set has moved. The value is TEXT and is cast in and out, which
// is how the column was defined.
const collabRulesEpochBumpQuery = `INSERT INTO collab_rules_meta(key, value)
 VALUES('epoch', '1')
 ON CONFLICT(key) DO UPDATE
 SET value = CAST(CAST(collab_rules_meta.value AS INTEGER) + 1 AS TEXT)`

var errRuleNotInExpectedState = errors.New(
	"db2: the rule is absent, or not in the status this transition moves from")

// collabRuleApprove moves a proposed rule to active.
//
// The status change and the epoch bump are one transaction, which the C does
// not do. There the bump is a separate statement after the flip, so a failure
// between them leaves a rule active that no agent is told about -- and because
// agents only re-read when the epoch moves, that state does not resolve on its
// own. Every subsequent approval bumps the epoch and would eventually reveal
// the rule, but until one happens the rule is invisible and enforced.
func collabRuleApprove(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	ruleID, err := db2contract.DecodeCollabRuleApproveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		return transitionAndBumpEpoch(ctx, tx, collabRuleApproveQuery,
			int64(ruleID), maxActiveCollabRules)
	})
	return acknowledgement(txErr == nil, db2contract.EncodeCollabRuleApproveReply)
}

// collabRuleRetire moves an active rule to retired.
func collabRuleRetire(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	ruleID, err := db2contract.DecodeCollabRuleRetireRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		return transitionAndBumpEpoch(ctx, tx, collabRuleRetireQuery, int64(ruleID))
	})
	return acknowledgement(txErr == nil, db2contract.EncodeCollabRuleRetireReply)
}

// collabRuleReject moves a proposed rule to rejected.
//
// No epoch bump, and that is not an omission: a proposal was never in the
// active set, so rejecting it changes nothing an agent is holding. Bumping
// would make every agent re-read a rule set that has not moved.
func collabRuleReject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	ruleID, err := db2contract.DecodeCollabRuleRejectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, collabRuleRejectQuery, int64(ruleID))
	return acknowledgement(execErr == nil && changed == 1,
		db2contract.EncodeCollabRuleRejectReply)
}

// transitionAndBumpEpoch applies a status change that must match exactly one
// row, then advances the epoch.
//
// The row count is the outcome here, not statement success: a transition whose
// WHERE misses means the rule is absent or was in another status, and bumping
// the epoch for that would tell every agent to re-read a set that has not
// changed.
func transitionAndBumpEpoch(ctx context.Context, tx Store, query string, args ...any) error {
	changed, err := tx.Exec(ctx, query, args...)
	if err != nil {
		return err
	}
	if changed != 1 {
		return errRuleNotInExpectedState
	}
	_, err = tx.Exec(ctx, collabRulesEpochBumpQuery)
	return err
}

const proposalMarkCommittedQuery = `UPDATE learning_proposals
 SET state = 'committed',
     committed_at = pg_now_text(),
     updated_at = pg_now_text()
 WHERE id = $1`

// proposalMarkCommitted records that a proposal was acted on.
//
// No state predicate, unlike every other transition here, so it commits a
// proposal from any state -- including one already committed, which re-stamps
// committed_at, and one rejected. And it acknowledges on the statement running
// rather than on a row changing, so an identifier nothing holds acknowledges
// too. Both are the C behaviour and both are load-bearing to a caller that
// retries: adding a predicate or a row check would turn a safely repeatable
// call into one that fails its second attempt.
func proposalMarkCommitted(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	proposalID, err := db2contract.DecodeProposalMarkCommittedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, proposalMarkCommittedQuery, int64(proposalID))
	return acknowledgement(execErr == nil, db2contract.EncodeProposalMarkCommittedReply)
}

const proposalBumpCorroborationQuery = `UPDATE learning_proposals
 SET corroboration_count = corroboration_count + 1,
     updated_at = pg_now_text()
 WHERE id = $1 AND state = 'pending'`

// proposalBumpCorroboration records another observation supporting a proposal.
//
// Only while pending: corroborating a proposal that has already been decided
// would move a count nothing reads any more, and worse, would make a committed
// proposal look like it is still gathering evidence. Like its neighbour it
// acknowledges the statement rather than a row.
func proposalBumpCorroboration(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	proposalID, err := db2contract.DecodeProposalBumpCorroborationRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, proposalBumpCorroborationQuery, int64(proposalID))
	return acknowledgement(execErr == nil, db2contract.EncodeProposalBumpCorroborationReply)
}

// Both deletes invalidate an in-process rules cache in the C, which the Go
// module has no way to reach and which will not exist once the C retires. The
// reply carries what a caller needs to do it themselves: a non-zero count means
// the rule set moved. That is the one piece of behaviour this port cannot carry
// across, so it is stated rather than dropped quietly.
const (
	rulesDeleteByIDQuery            = `DELETE FROM rules WHERE id = $1`
	rulesDeleteByDirectiveTypeQuery = `DELETE FROM rules WHERE directive_type = $1`
)

// rulesDeleteByID removes one rule, answering whether it was there.
func rulesDeleteByID(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	ruleRowID, err := db2contract.DecodeRulesDeleteByIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, rulesDeleteByIDQuery, int64(ruleRowID))
	// One or nothing: the identifier is a primary key, so the count is a
	// yes-or-no and the reply says so.
	return acknowledgement(execErr == nil && deleted > 0,
		db2contract.EncodeRulesDeleteByIDReply)
}

// rulesDeleteByDirectiveType removes every rule of a kind, answering how many.
//
// A count rather than a flag, because the caller has no other way to know how
// much of the rule set just disappeared.
func rulesDeleteByDirectiveType(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	directiveType, err := db2contract.DecodeRulesDeleteByDirectiveTypeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, rulesDeleteByDirectiveTypeQuery, directiveType)
	if execErr != nil {
		deleted = 0
	}
	reply, err := db2contract.EncodeRulesDeleteByDirectiveTypeReply(clampToU32(deleted))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
