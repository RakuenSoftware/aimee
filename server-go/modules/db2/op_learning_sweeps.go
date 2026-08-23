package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageRulesDecay,
		db2contract.OperationRulesDecay, rulesDecay)
	Register(db2contract.StageCuriosityRescoreAll,
		db2contract.OperationCuriosityRescoreAll, curiosityRescoreAll)
	Register(db2contract.StageMiningSeedJobDefaults,
		db2contract.OperationMiningSeedJobDefaults, miningSeedJobDefaults)
	Register(db2contract.StageProposalsArchiveExpired,
		db2contract.OperationProposalsArchiveExpired, proposalsArchiveExpired)
	Register(db2contract.StageTraceMiningLastID,
		db2contract.OperationTraceMiningLastID, traceMiningLastID)
	Register(db2contract.StageTraceMiningRecord,
		db2contract.OperationTraceMiningRecord, traceMiningRecord)
	Register(db2contract.StageAntiPatternBump,
		db2contract.OperationAntiPatternBump, antiPatternBump)
	Register(db2contract.StageAntiPatternDelete,
		db2contract.OperationAntiPatternDelete, antiPatternDelete)
	Register(db2contract.StageAntiPatternExistsExact,
		db2contract.OperationAntiPatternExistsExact, antiPatternExistsExact)
	Register(db2contract.StageAntiPatternExistsBySourceRef,
		db2contract.OperationAntiPatternExistsBySourceRef,
		antiPatternExistsBySourceRef)
	Register(db2contract.StageArtifactCitationCount,
		db2contract.OperationArtifactCitationCount, artifactCitationCount)
	Register(db2contract.StageCommitsInLast7Days,
		db2contract.OperationCommitsInLast7Days, commitsInLast7Days)
	Register(db2contract.StageFidelityAttributionCount,
		db2contract.OperationFidelityAttributionCount, fidelityAttributionCount)
	Register(db2contract.StageArtifactStampReflected,
		db2contract.OperationArtifactStampReflected, artifactStampReflected)
	Register(db2contract.StageFailedQueryBump,
		db2contract.OperationFailedQueryBump, failedQueryBump)
	Register(db2contract.StageArtifactSetState,
		db2contract.OperationArtifactSetState, artifactSetState)
	Register(db2contract.StageArtifactRegisterExemplar,
		db2contract.OperationArtifactRegisterExemplar, artifactRegisterExemplar)
	Register(db2contract.StageEvidenceEnqueue,
		db2contract.OperationEvidenceEnqueue, evidenceEnqueue)
	Register(db2contract.StageEvidenceMarkFailed,
		db2contract.OperationEvidenceMarkFailed, evidenceMarkFailed)
}

// The decay policy, from the C's constants.
//
// Hard directives decay on a window three times longer than soft ones: a rule
// somebody marked hard is meant to survive not being reinforced for a while,
// which is most of what "hard" means here.
const (
	decayAmount       = 5
	decayIntervalSoft = 14
	decayIntervalHard = 42
	archiveThreshold  = 10
	archiveGraceDays  = 30
)

// The three decay steps in one statement.
//
// A rule reinforced by nobody loses weight; one that has fallen below the
// threshold and stayed there is deleted. The delete is last in both, and the
// order matters: a rule decayed to below the threshold in this same pass is not
// eligible for deletion until its grace period has also passed, and running the
// delete first would be a different policy.
//
// last_reinforced_at must be present. A rule nothing has ever reinforced has no
// idle window to measure, and decaying it would punish it for being new.
const rulesDecayQuery = `WITH softened AS (
   UPDATE rules SET weight = GREATEST(weight - $1, 0), updated_at = pg_now_text()
    WHERE (directive_type IS NULL OR directive_type <> 'hard')
      AND last_reinforced_at IS NOT NULL
      AND last_reinforced_at < pg_now_text($2)
   RETURNING 1
 ), hardened AS (
   UPDATE rules SET weight = GREATEST(weight - $1, 0), updated_at = pg_now_text()
    WHERE directive_type = 'hard'
      AND last_reinforced_at IS NOT NULL
      AND last_reinforced_at < pg_now_text($3)
   RETURNING 1
 ), archived AS (
   DELETE FROM rules
    WHERE weight < $4 AND updated_at < pg_now_text($5)
   RETURNING 1
 )
 SELECT (SELECT COUNT(*) FROM softened) + (SELECT COUNT(*) FROM hardened)
      + (SELECT COUNT(*) FROM archived)`

// rulesDecay ages the rule set and drops what has stayed weak.
//
// The C invalidates its in-process rule cache when anything changed. This
// module has no such cache and cannot reach the C's, which is a cutover item
// rather than a port one: whoever owns that cache has to invalidate it when the
// Go module starts serving this operation, or a decayed rule will keep being
// applied from memory.
func rulesDecay(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeRulesDecayRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var total int64
	// The windows are built here rather than concatenated in the statement:
	// '-' || $n || ' days' makes Postgres infer $n as text, and an integer
	// bound to a text parameter has no encoding.
	if err := store.QueryRow(ctx, rulesDecayQuery, decayAmount,
		retentionWindow(decayIntervalSoft), retentionWindow(decayIntervalHard),
		archiveThreshold, retentionWindow(archiveGraceDays)).
		Scan(&total); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(total, db2contract.EncodeRulesDecayReply)
}

// The whole rescoring, in one statement where the C runs a read, a coverage
// count per item and an update per item.
//
// A hundred open questions cost the C two hundred and one statements. The
// arithmetic is the same arithmetic:
//
//   - importance starts at the gap kind's base weight and is raised by an
//     eighth when the item carries evidence, because a question with something
//     to look at is more answerable than one without;
//   - novelty falls as the corpus already covers the subject, so a question
//     about something written about forty times is not novel;
//   - progress decays by half every thirty days, so an item nobody has touched
//     drifts back towards zero and the stale boost lifts it;
//   - maturity moves the balance from importance towards novelty as the corpus
//     grows: a young corpus should answer what matters, a large one should
//     answer what is missing.
//
// updated_at is deliberately assigned to itself. Rescoring is not activity, and
// letting it touch the stamp would make every item permanently fresh -- which
// would defeat the progress decay this same statement computes.
const curiosityRescoreAllQuery = `WITH maturity AS (
   SELECT LEAST(GREATEST(LOG(10, (SELECT COUNT(*) FROM memories) + 1.0) / 4.0,
     0.0), 1.0) AS value
 ), scored AS (
   SELECT c.id,
     LEAST(GREATEST(
       CASE c.gap_type
         WHEN 'contradiction' THEN 0.80
         WHEN 'unverified_assumption' THEN 0.60
         WHEN 'missing_fact' THEN 0.50
         WHEN 'weak_coverage' THEN 0.40
         WHEN 'stale_fact' THEN 0.30
         ELSE 0.10 END
       * CASE WHEN c.evidence <> '' THEN 1.15 ELSE 1.0 END, 0.0), 1.0) AS importance,
     1.0 / (1.0 + (
       SELECT COUNT(*) FROM memories m
        WHERE m.merged_into = 0
          AND COALESCE(NULLIF(c.target_topic, ''), NULLIF(c.target_entity, ''), '') <> ''
          AND (LOWER(m.key) LIKE '%' || LOWER(COALESCE(NULLIF(c.target_topic, ''),
                 c.target_entity)) || '%'
               OR LOWER(m.content) LIKE '%' || LOWER(COALESCE(NULLIF(c.target_topic, ''),
                 c.target_entity)) || '%'))) AS novelty,
     LEAST(GREATEST(
       CASE WHEN c.state = 'resolved' THEN 1.0
            WHEN c.updated_at = '' THEN 0.0
            ELSE EXP(-0.693147 * GREATEST(EXTRACT(EPOCH FROM
              (NOW() AT TIME ZONE 'UTC')
              - COALESCE(NULLIF(replace(replace(c.updated_at, 'T', ' '), 'Z', ''), '')::timestamp,
                NOW() AT TIME ZONE 'UTC')) / 86400.0, 0.0) / 30.0)
       END, 0.0), 1.0) AS progress,
     c.gap_type
   FROM curiosity_items c
   WHERE c.state IN ('open', 'in_progress')
 ), routed AS (
   SELECT s.id, s.importance, s.novelty, s.progress,
     LEAST(GREATEST((1.0 - 0.6 * m.value) * s.importance
       + (0.6 * m.value) * s.novelty
       + 0.2 * (1.0 - s.progress)
       + CASE WHEN s.gap_type = 'contradiction' THEN 0.10 ELSE 0.0 END,
       0.0), 1.0) AS routing_score
   FROM scored s CROSS JOIN maturity m
 ), rescored AS (
   UPDATE curiosity_items c
      SET importance = r.importance, novelty = r.novelty, progress = r.progress,
          routing_score = r.routing_score, updated_at = c.updated_at
     FROM routed r WHERE r.id = c.id
   RETURNING 1
 )
 SELECT COUNT(*) FROM rescored`

// curiosityRescoreAll re-ranks every open question against the corpus as it
// now stands.
func curiosityRescoreAll(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCuriosityRescoreAllRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var rescored int64
	if err := store.QueryRow(ctx, curiosityRescoreAllQuery).
		Scan(&rescored); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(rescored, db2contract.EncodeCuriosityRescoreAllReply)
}

// The mining jobs the system runs on its own schedule, and how often.
//
// Seeded rather than assumed: a job with no row does not run at all, and these
// two are the ones the miner cannot work without. DO NOTHING on conflict, so a
// deployment that has retuned an interval keeps its own.
const miningSeedJobDefaultsQuery = `INSERT INTO mining_jobs
 (id, hwm, interval_s, enabled)
 SELECT job.id, 0, job.interval_s, TRUE
   FROM (VALUES ('pattern_cluster', 900), ('recurrence', 1800))
     AS job(id, interval_s)
 ON CONFLICT (id) DO NOTHING`

func miningSeedJobDefaults(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeMiningSeedJobDefaultsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, miningSeedJobDefaultsQuery); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(infallible(db2contract.EncodeMiningSeedJobDefaultsReply))
}

// A proposal past its expiry is archived with the reason recorded, never
// deleted: what the system proposed and did not do is part of what it learned.
//
// The empty-string test is the same trap the lifecycle sweep has: expires_at is
// NOT NULL with an empty default, and an empty string sorts before every stamp.
const proposalsArchiveExpiredQuery = `UPDATE learning_proposals
 SET state = 'archived', archive_reason = 'expired', updated_at = pg_now_text()
 WHERE state = 'pending' AND expires_at <> '' AND expires_at < pg_now_text()`

func proposalsArchiveExpired(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeProposalsArchiveExpiredRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, proposalsArchiveExpiredQuery); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(infallible(db2contract.EncodeProposalsArchiveExpiredReply))
}

const (
	// COALESCE to zero because MAX over an empty log is NULL, and a miner
	// starting from nothing has mined up to trace zero.
	traceMiningLastIDQuery = `SELECT COALESCE(MAX(last_trace_id), 0)
 FROM trace_mining_log`

	traceMiningRecordQuery = `INSERT INTO trace_mining_log
 (last_trace_id, mined_at) VALUES ($1, pg_now_text())`

	antiPatternBumpQuery = `UPDATE anti_patterns
 SET hit_count = hit_count + 1 WHERE id = $1`

	antiPatternDeleteQuery = `DELETE FROM anti_patterns WHERE id = $1`

	antiPatternExistsExactQuery = `SELECT EXISTS (
 SELECT 1 FROM anti_patterns WHERE pattern = $1)`

	antiPatternExistsBySourceRefQuery = `SELECT EXISTS (
 SELECT 1 FROM anti_patterns WHERE source_ref = $1)`

	// Distinct sources, not rows: an artifact cited five times from one
	// document has one source behind it, and the count is a measure of
	// corroboration.
	artifactCitationCountQuery = `SELECT COUNT(DISTINCT source_id)
 FROM artifact_citations WHERE artifact_id = $1`

	commitsInLast7DaysQuery = `SELECT COUNT(*) FROM learning_proposals
 WHERE sink = $1 AND state = 'committed'
   AND committed_at >= pg_now_text('-7 days')`

	fidelityAttributionCountQuery = `SELECT COUNT(*) FROM artifacts
 WHERE kind = 'fidelity_attribution' AND turn_id = $1`

	artifactStampReflectedQuery = `UPDATE artifacts
 SET reflected_at = CURRENT_TIMESTAMP WHERE id = $1`

	artifactSetStateQuery = `UPDATE artifacts SET state = $2 WHERE id = $1`

	// No conflict clause, as in the C: registering the same artifact twice in
	// one collection is a duplicate row, and the table has no uniqueness rule
	// that would stop it.
	artifactRegisterExemplarQuery = `INSERT INTO exemplar_vectors
 (artifact_id, collection) VALUES ($1, $2)`

	evidenceEnqueueQuery = `INSERT INTO evidence_index_ops (artifact_id, collection)
 VALUES ($1, $2) ON CONFLICT DO NOTHING`

	evidenceMarkFailedQuery = `UPDATE evidence_index_ops
 SET status = 'failed', attempts = attempts + 1, last_error = $2,
     updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE artifact_id = $1`

	// Upsert then read back, in one statement: the C runs two and can report a
	// count another bumper has already moved past.
	failedQueryBumpQuery = `INSERT INTO failed_queries
 (query_norm, failure_count, last_failed_at)
 VALUES ($1, 1, pg_now_text())
 ON CONFLICT (query_norm) DO UPDATE
 SET failure_count = failed_queries.failure_count + 1,
     last_failed_at = pg_now_text()
 RETURNING failure_count`
)

func traceMiningLastID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeTraceMiningLastIDRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var last int64
	if err := store.QueryRow(ctx, traceMiningLastIDQuery).Scan(&last); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTraceMiningLastIDReply(
		uint64(max(last, 0)))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func traceMiningRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	lastTraceID, err := db2contract.DecodeTraceMiningRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, traceMiningRecordQuery,
		int64(lastTraceID)); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeTraceMiningRecordReply)
}

func antiPatternBump(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	patternID, err := db2contract.DecodeAntiPatternBumpRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// No changed-row requirement, unlike its delete neighbour: the C returns
	// success whenever the statement ran, so bumping a pattern that is not
	// there is a no-op rather than a failure.
	if _, execErr := store.Exec(ctx, antiPatternBumpQuery,
		int64(patternID)); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(infallible(db2contract.EncodeAntiPatternBumpReply))
}

func antiPatternDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	patternID, err := db2contract.DecodeAntiPatternDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, antiPatternDeleteQuery,
		infallible(db2contract.EncodeAntiPatternDeleteReply), int64(patternID))
}

func antiPatternExistsExact(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pattern, err := db2contract.DecodeAntiPatternExistsExactRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, antiPatternExistsExactQuery,
		db2contract.EncodeAntiPatternExistsExactReply, pattern)
}

func antiPatternExistsBySourceRef(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sourceRef, err :=
		db2contract.DecodeAntiPatternExistsBySourceRefRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, antiPatternExistsBySourceRefQuery,
		db2contract.EncodeAntiPatternExistsBySourceRefReply, sourceRef)
}

func artifactCitationCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeArtifactCitationCountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, artifactCitationCountQuery,
		db2contract.EncodeArtifactCitationCountReply, artifactID)
}

func commitsInLast7Days(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sink, err := db2contract.DecodeCommitsInLast7DaysRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, commitsInLast7DaysQuery,
		db2contract.EncodeCommitsInLast7DaysReply, sink)
}

func fidelityAttributionCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	turnID, err := db2contract.DecodeFidelityAttributionCountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, fidelityAttributionCountQuery,
		db2contract.EncodeFidelityAttributionCountReply, turnID)
}

func artifactStampReflected(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeArtifactStampReflectedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, artifactStampReflectedQuery,
		artifactID); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeArtifactStampReflectedReply)
}

// failedQueryBump records that a query found nothing, and answers how many
// times that has now happened.
func failedQueryBump(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	queryNorm, err := db2contract.DecodeFailedQueryBumpRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var failures int64
	if scanErr := store.QueryRow(ctx, failedQueryBumpQuery, queryNorm).
		Scan(&failures); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(failures, db2contract.EncodeFailedQueryBumpReply)
}

func artifactSetState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, newState, err :=
		db2contract.DecodeArtifactSetStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, artifactSetStateQuery, artifactID,
		newState); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeArtifactSetStateReply)
}

func artifactRegisterExemplar(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, collection, err :=
		db2contract.DecodeArtifactRegisterExemplarRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if collection == "" {
		// The C's default collection. An exemplar with no collection named is
		// a case exemplar, which is what the caller almost always means.
		collection = "case_exemplars"
	}
	if _, execErr := store.Exec(ctx, artifactRegisterExemplarQuery, artifactID,
		collection); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeArtifactRegisterExemplarReply)
}

func evidenceEnqueue(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, collection, err :=
		db2contract.DecodeEvidenceEnqueueRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if collection == "" {
		collection = "evidence"
	}
	if _, execErr := store.Exec(ctx, evidenceEnqueueQuery, artifactID,
		collection); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeEvidenceEnqueueReply)
}

func evidenceMarkFailed(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, message, err :=
		db2contract.DecodeEvidenceMarkFailedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, evidenceMarkFailedQuery, artifactID,
		message); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeEvidenceMarkFailedReply)
}
