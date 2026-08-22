package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageArtifactCite,
		db2contract.OperationArtifactCite, artifactCite)
	Register(db2contract.StageArtifactLink,
		db2contract.OperationArtifactLink, artifactLink)
	Register(db2contract.StageCollabRulePropose,
		db2contract.OperationCollabRulePropose, collabRulePropose)
	Register(db2contract.StageTaskUpdateState,
		db2contract.OperationTaskUpdateState, taskUpdateState)
	Register(db2contract.StageDemotionProfileRead,
		db2contract.OperationDemotionProfileRead, demotionProfileRead)
	Register(db2contract.StageLearningProposalFindPending,
		db2contract.OperationLearningProposalFindPending, learningProposalFindPending)
}

// Both citation and link writes are ON CONFLICT DO NOTHING and report the
// statement rather than a row. Recording the same citation twice is a caller
// re-running work, not a mistake to surface -- and the reply cannot tell the
// two apart, which is why neither reads the row count.
const (
	artifactCiteQuery = `INSERT INTO artifact_citations
 (artifact_id, source_kind, source_id, span_start, span_end)
 VALUES ($1, $2, $3, 0, 0)
 ON CONFLICT DO NOTHING`
	artifactLinkQuery = `INSERT INTO artifact_links (from_id, to_id, kind)
 VALUES ($1, $2, $3)
 ON CONFLICT DO NOTHING`
)

// artifactCite records that an artifact drew on a source.
//
// The span is written as (0, 0), which means "the whole source" rather than a
// range of nothing -- artifact_invalidate_citing reads that pair specially, so
// a citation written here is invalidated by any change to the source. A caller
// that knows the span it used has no way to say so through this operation.
func artifactCite(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	artifactID, sourceKind, sourceID, err := db2contract.DecodeArtifactCiteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, artifactCiteQuery, artifactID, sourceKind, sourceID)
	return acknowledgement(execErr == nil, db2contract.EncodeArtifactCiteReply)
}

// artifactLink records a relationship between two artifacts.
//
// Directed: from points at to, and nothing on this wire reads the reverse. The
// kind is stored without validation, so what link kinds exist is decided by
// whoever writes them rather than by the schema.
func artifactLink(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	fromID, toID, kind, err := db2contract.DecodeArtifactLinkRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, artifactLinkQuery, fromID, toID, kind)
	return acknowledgement(execErr == nil, db2contract.EncodeArtifactLinkReply)
}

// maxTotalCollabRules mirrors COLLAB_MAX_TOTAL_RULES. Unlike the active cap
// this bounds the whole table, proposals included, so a flood of proposals
// cannot crowd out the ability to propose at all.
const maxTotalCollabRules = 50

// The cap is inside the statement rather than a count read before it, as with
// approve: reading, comparing and then writing is a check that has stopped
// being true by the time it is acted on. INSERT ... SELECT is what lets a WHERE
// gate an insert, so a table already at its cap inserts nothing and RETURNING
// yields no row.
const collabRuleProposeQuery = `INSERT INTO collab_rules (text, reason, proposed_by, status)
 SELECT $1, $2, $3, 'proposed'
 WHERE (SELECT COUNT(*) FROM collab_rules) < $4
 RETURNING id`

// collabRulePropose raises a rule for a person to decide on.
//
// Zero means it was not raised, which happens when the table is full. The
// caller cannot tell that from any other refusal, and could not in the C
// either.
//
// The text and reason lengths are the contract's to enforce; the C checks them
// itself because its caller hands it a bare pointer, and the encoder here has
// already refused anything longer.
func collabRulePropose(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	text, reason, proposedBy, err := db2contract.DecodeCollabRuleProposeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	id, status := readOptionalInt(ctx, store, collabRuleProposeQuery,
		text, reason, proposedBy, maxTotalCollabRules)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeCollabRuleProposeReply(clampToU32(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const taskUpdateStateQuery = `UPDATE tasks
 SET state = $2, updated_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC',
                                      'YYYY-MM-DD"T"HH24:MI:SS"Z"')
 WHERE id = $1`

// taskUpdateState moves a task between states.
//
// Requires the task to exist: the reply is named "changed" rather than
// "acknowledged", and it means a row moved. A caller updating a task that has
// been deleted is working from a stale list and should be told.
func taskUpdateState(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	taskID, state, err := db2contract.DecodeTaskUpdateStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, taskUpdateStateQuery, int64(taskID), state)
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeTaskUpdateStateReply)
}

// Reading a profile also stamps it as accessed, which the C does with a
// separate db2_artifact_touch after the select. That stamp feeds the temporal
// decay sweep, so dropping it would let a profile that is being used decay as
// though nobody wanted it -- the read is the evidence that it is still wanted.
//
// One statement rather than two: the CTE finds the row, the UPDATE stamps it,
// and the outer SELECT returns the payload. That also removes a gap the C has,
// where a profile can be returned to a caller and then fail to be stamped.
const demotionProfileQuery = `WITH found AS (
 SELECT id, payload FROM artifacts
 WHERE kind = 'demotion_profile'
   AND target_surface = $1
   AND scope_kind = $2
   AND scope_id = $3
   AND state = 'committed'
 ORDER BY committed_at DESC
 LIMIT 1
), touched AS (
 UPDATE artifacts SET last_accessed_at = CURRENT_TIMESTAMP
 FROM found WHERE artifacts.id = found.id
)
SELECT payload FROM found`

// demotionProfileRead finds the most specific profile that applies.
//
// Three lookups, narrowest first: the exact scope, then the scope kind with no
// identifier, then global. The order is the whole behaviour -- a profile set
// for one project must beat the one set for every project, and falling back
// only when nothing more specific exists is what makes a global default a
// default rather than an override.
//
// The steps are skipped where they would repeat the one before: an empty scope
// id makes the second lookup identical to the first, and an empty scope kind
// makes the third identical to the second. The C guards both.
//
// Each lookup takes the most recently committed match, so a profile replaced
// rather than superseded still resolves to the newer one.
func demotionProfileRead(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryClass, scopeKind, scopeID, err :=
		db2contract.DecodeDemotionProfileReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	attempts := [][2]string{{scopeKind, scopeID}}
	if scopeID != "" {
		attempts = append(attempts, [2]string{scopeKind, ""})
	}
	if scopeKind != "" {
		attempts = append(attempts, [2]string{"global", ""})
	}

	profile := ""
	for _, attempt := range attempts {
		payload, status := readOptionalText(ctx, store, demotionProfileQuery,
			memoryClass, attempt[0], attempt[1])
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		if payload != "" {
			profile = payload
			break
		}
	}

	reply, err := db2contract.EncodeDemotionProfileReadReply(profile)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const learningProposalFindPendingQuery = `SELECT id FROM learning_proposals
 WHERE sink = $1 AND state = 'pending' AND target_key = $2 AND target_memory_id = $3
 ORDER BY id DESC LIMIT 1`

// learningProposalFindPending finds an open proposal for the same target.
//
// This is what stops the same observation raising a second proposal: a caller
// checks here first and corroborates the existing one instead. Newest first, so
// where duplicates already exist the most recent is the one corroborated and
// the older ones are left to expire.
//
// Only pending proposals count. One already committed or archived has been
// decided, and a new observation about the same target deserves a new proposal
// rather than reopening a closed one.
func learningProposalFindPending(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sink, targetKey, targetMemoryID, err :=
		db2contract.DecodeLearningProposalFindPendingRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	id, status := readOptionalInt(ctx, store, learningProposalFindPendingQuery,
		sink, targetKey, int64(targetMemoryID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeLearningProposalFindPendingReply(clampToU32(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
