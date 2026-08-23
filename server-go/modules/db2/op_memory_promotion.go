package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageExpire, db2contract.OperationExpire, memoryExpire)
	Register(db2contract.StageDemote, db2contract.OperationDemote, memoryDemote)
	Register(db2contract.StagePromoteStable,
		db2contract.OperationPromoteStable, promoteStable)
	Register(db2contract.StageReclassifyDirectives,
		db2contract.OperationReclassifyDirectives, reclassifyDirectives)
	Register(db2contract.StageRecordL4Approval,
		db2contract.OperationRecordL4Approval, recordL4Approval)
	Register(db2contract.StagePruneOrphanedL0,
		db2contract.OperationPruneOrphanedL0, pruneOrphanedL0)
	Register(db2contract.StageLifecycleSweepExpired,
		db2contract.OperationLifecycleSweepExpired, lifecycleSweepExpired)
	Register(db2contract.StageDemoteID,
		db2contract.OperationDemoteID, demoteID)
}

// The lifecycle defaults an unconfigured kind falls back to, from the C's
// default_lifecycle. A kind with no row in kind_lifecycle is not an error --
// most kinds have none -- so the join is a LEFT one and these fill the gap.
const (
	defaultExpireDays         = 30
	defaultDemoteDays         = 60
	defaultDemoteConfidence   = 0.7
	defaultDemotionResistance = 1.0
)

// Expiry, in one statement, where the C runs two deletes and then a pair more
// per kind it found at L1.
//
// The per-kind loop is a join to kind_lifecycle. That is what the loop was: a
// lookup of each kind's window followed by a delete using it, and a join does
// both at once for every kind at once. A corpus with forty kinds at L1 costs
// the C eighty-one statements and this one.
//
// Provenance goes first in both, and it has to: the rows name memories by
// identifier, and deleting the memories first would leave nothing to select
// them by. The CTEs run in one snapshot, so the stale set is computed once and
// both deletes use it -- which the C cannot promise, since its two statements
// re-evaluate the window separately and a memory used in between survives one
// and not the other.
//
// A kind configured with a non-positive window is refused rather than
// defaulted, which is the C's answer: it returns an internal error rather than
// guessing what a zero-day expiry meant.
const memoryExpireQuery = `WITH stale AS (
   SELECT m.id, COALESCE(k.expire_days, $1) AS expire_days
     FROM memories m
     LEFT JOIN kind_lifecycle k ON k.kind = m.kind
    WHERE m.tier = 'L1'
 ), misconfigured AS (
   SELECT COUNT(*) AS n FROM stale WHERE expire_days <= 0
 ), doomed AS (
   SELECT s.id FROM stale s
     JOIN memories m ON m.id = s.id
    WHERE (SELECT n FROM misconfigured) = 0
      AND m.last_used_at < pg_now_text('-' || s.expire_days || ' days')
 ), l0_provenance AS (
   DELETE FROM memory_provenance
    WHERE memory_id IN (SELECT id FROM memories WHERE tier = 'L0')
   RETURNING 1
 ), l0 AS (
   DELETE FROM memories WHERE tier = 'L0' RETURNING 1
 ), stale_provenance AS (
   DELETE FROM memory_provenance WHERE memory_id IN (SELECT id FROM doomed)
   RETURNING 1
 ), stale_rows AS (
   DELETE FROM memories WHERE id IN (SELECT id FROM doomed) RETURNING 1
 )
 SELECT (SELECT COUNT(*) FROM l0),
        (SELECT COUNT(*) FROM stale_rows),
        (SELECT n FROM misconfigured)`

// memoryExpire drops what the tiers no longer keep: everything at L0, and
// everything at L1 idle past its kind's window.
func memoryExpire(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeExpireRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var level0, stale, misconfigured int64
	if err := store.QueryRow(ctx, memoryExpireQuery, defaultExpireDays).
		Scan(&level0, &stale, &misconfigured); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if misconfigured > 0 {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeExpireReply(uint32(max(level0, 0)),
		uint32(max(stale, 0)))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Demotion and its cascade, in one statement.
//
// The cascade follows the demoted rows by identifier rather than by matching
// the stamp they were written with, which is what the C does. Its stamp is a
// whole second wide, so anything else updated in the same second was cascaded
// over too -- a memory demoted by this sweep and a memory edited by a person
// are indistinguishable to it.
//
// The resistance multiplies the idle window, so a kind that resists demotion
// sits longer before it drops. It is applied where the C applies it, inside the
// window rather than to the confidence.
const memoryDemoteQuery = `WITH policy AS (
   SELECT m.id,
          COALESCE(k.demote_confidence, $1) AS demote_confidence,
          (COALESCE(k.demote_days, $2)
            * COALESCE(k.demotion_resistance, $3))::int AS demote_days
     FROM memories m
     LEFT JOIN kind_lifecycle k ON k.kind = m.kind
    WHERE m.tier = 'L2'
 ), doomed AS (
   SELECT p.id FROM policy p
     JOIN memories m ON m.id = p.id
    WHERE m.confidence < p.demote_confidence
      AND m.last_used_at < pg_now_text('-' || p.demote_days || ' days')
 ), demoted AS (
   UPDATE memories SET tier = 'L1', updated_at = pg_now_text()
    WHERE id IN (SELECT id FROM doomed)
   RETURNING id
 ), cascaded AS (
   UPDATE memories SET confidence = confidence * 0.9
    WHERE id IN (
      SELECT ml.source_id FROM memory_links ml
       WHERE ml.relation = 'depends_on'
         AND ml.target_id IN (SELECT id FROM demoted))
   RETURNING id
 )
 SELECT (SELECT COUNT(*) FROM demoted), (SELECT COUNT(*) FROM cascaded)`

// memoryDemote drops stale, low-confidence L2 memories a tier, and weakens
// whatever depended on them.
func memoryDemote(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeDemoteRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var demoted, cascaded int64
	if err := store.QueryRow(ctx, memoryDemoteQuery, defaultDemoteConfidence,
		defaultDemoteDays, defaultDemotionResistance).
		Scan(&demoted, &cascaded); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeDemoteReply(uint32(max(demoted, 0)),
		uint32(max(cascaded, 0)))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Only facts and preferences reach L3, and only after a month of not changing.
// The thresholds are the C's constants rather than a kind's configured policy,
// which is deliberate there: L3 is the tier that outlives a session, and what
// earns it is not something a kind gets to lower for itself.
const promoteStableQuery = `UPDATE memories
 SET tier = 'L3', updated_at = pg_now_text()
 WHERE tier = 'L2' AND kind IN ('fact', 'preference')
   AND confidence >= 0.95 AND use_count >= 5
   AND updated_at <= pg_now_text('-30 days')`

func promoteStable(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodePromoteStableRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	promoted, execErr := store.Exec(ctx, promoteStableQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(promoted, db2contract.EncodePromoteStableReply)
}

// The two spellings of "which directives are policy".
//
// With approval required a workflow still promotes itself, and a policy needs
// somebody to have said so -- which is the whole difference between the two
// statements, and why the flag is not a nicety: L4 is the tier that governs the
// system's own behaviour.
const (
	reclassifyDirectivesApprovedQuery = `UPDATE memories SET tier = 'L4'
 WHERE tier = 'L3'
   AND (kind = 'workflow'
        OR (kind = 'policy' AND id IN
            (SELECT memory_id FROM memory_promotion_approvals
              WHERE target_tier = 'L4')))`

	reclassifyDirectivesOpenQuery = `UPDATE memories SET tier = 'L4'
 WHERE tier = 'L3' AND kind IN ('policy', 'workflow')`
)

func reclassifyDirectives(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	requireApproval, err := db2contract.DecodeReclassifyDirectivesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	query := reclassifyDirectivesOpenQuery
	if requireApproval != 0 {
		query = reclassifyDirectivesApprovedQuery
	}
	reclassified, execErr := store.Exec(ctx, query)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(reclassified,
		db2contract.EncodeReclassifyDirectivesReply)
}

// One approval per memory per tier, and a second approval replaces the first.
//
// Replacing rather than refusing is the C's choice and the right one: an
// approval is a statement of who currently vouches for the promotion, not a
// log of everyone who ever did. The log is the audit trail's job.
const recordL4ApprovalQuery = `INSERT INTO memory_promotion_approvals
 (memory_id, target_tier, approver, note, approved_at)
 VALUES ($1, 'L4', $2, $3, pg_now_text())
 ON CONFLICT (memory_id, target_tier) DO UPDATE
 SET approver = EXCLUDED.approver, note = EXCLUDED.note,
     approved_at = EXCLUDED.approved_at`

func recordL4Approval(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, approver, note, err :=
		db2contract.DecodeRecordL4ApprovalRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if memoryID == 0 {
		// The C refuses a non-positive identifier and the adapter turns that
		// refusal into an internal error, because the reply has no field to
		// carry a "no" -- it is empty on success and there is no other answer.
		return nil, bus.ModuleStatusInternal
	}
	if approver == "" {
		// The C's default. An approval has to name somebody, and "operator" is
		// what a local promotion with nobody named amounts to.
		approver = "operator"
	}
	if _, execErr := store.Exec(ctx, recordL4ApprovalQuery, int64(memoryID),
		approver, note); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeRecordL4ApprovalReply()
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// An L0 older than a week was never going to be promoted.
const pruneOrphanedL0Query = `DELETE FROM memories
 WHERE tier = 'L0' AND created_at < pg_now_text('-7 days')`

func pruneOrphanedL0(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodePruneOrphanedL0Request(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	pruned, execErr := store.Exec(ctx, pruneOrphanedL0Query)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(pruned, db2contract.EncodePruneOrphanedL0Reply)
}

// A pending memory whose time-to-live has passed is archived, not deleted.
//
// The empty-string test is load-bearing: ttl_at is NOT NULL with an empty
// default, so a memory with no TTL holds an empty string rather than a null,
// and an empty string sorts before every real stamp. Without the test every
// pending memory that never had a TTL would archive itself immediately.
const lifecycleSweepExpiredQuery = `UPDATE memories
 SET lifecycle_state = 'archived', archive_reason = 'pending_ttl_expired',
     updated_at = pg_now_text()
 WHERE lifecycle_state = 'pending' AND ttl_at <> '' AND ttl_at < pg_now_text()`

func lifecycleSweepExpired(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeLifecycleSweepExpiredRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	swept, execErr := store.Exec(ctx, lifecycleSweepExpiredQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(swept, db2contract.EncodeLifecycleSweepExpiredReply)
}

// A floor of 0.3 rather than zero: repeated demotion should make a memory
// unlikely to surface, never make it certainly wrong.
const demoteIDQuery = `UPDATE memories
 SET confidence = confidence * 0.9, updated_at = pg_now_text()
 WHERE id = $1 AND confidence > 0.3`

func demoteID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeDemoteIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if memoryID == 0 {
		return removedCount(0, db2contract.EncodeDemoteIDReply)
	}
	demoted, execErr := store.Exec(ctx, demoteIDQuery, int64(memoryID))
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(demoted, db2contract.EncodeDemoteIDReply)
}
