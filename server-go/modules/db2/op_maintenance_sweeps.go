package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageProspectiveSweepExpired,
		db2contract.OperationProspectiveSweepExpired, prospectiveSweepExpired)
	Register(db2contract.StageDirectiveSweepExpired,
		db2contract.OperationDirectiveSweepExpired, directiveSweepExpired)
	Register(db2contract.StageMarkRevisitDue,
		db2contract.OperationMarkRevisitDue, markRevisitDue)
	Register(db2contract.StageIngestQueueResetRunning,
		db2contract.OperationIngestQueueResetRunning, ingestQueueResetRunning)
	Register(db2contract.StageEvidenceReembedAll,
		db2contract.OperationEvidenceReembedAll, evidenceReembedAll)
	Register(db2contract.StageCuratorReembedAll,
		db2contract.OperationCuratorReembedAll, curatorReembedAll)
	Register(db2contract.StageSynthReenqueueAll,
		db2contract.OperationSynthReenqueueAll, synthReenqueueAll)
	Register(db2contract.StageCuratorReenqueueExtractAll,
		db2contract.OperationCuratorReenqueueExtractAll,
		curatorReenqueueExtractAll)
	Register(db2contract.StageDirectiveSuppress,
		db2contract.OperationDirectiveSuppress, directiveSuppress)
	Register(db2contract.StageDirectiveRecordSurface,
		db2contract.OperationDirectiveRecordSurface, directiveRecordSurface)
	Register(db2contract.StageAsyncPendingCount,
		db2contract.OperationAsyncPendingCount, asyncPendingCount)
	Register(db2contract.StageRuntimeStateTouch,
		db2contract.OperationRuntimeStateTouch, runtimeStateTouch)
	Register(db2contract.StageSynthEnqueue,
		db2contract.OperationSynthEnqueue, synthEnqueue)
	Register(db2contract.StageSynthMarkDone,
		db2contract.OperationSynthMarkDone, synthMarkDone)
	Register(db2contract.StageReembedMarkFinished,
		db2contract.OperationReembedMarkFinished, reembedMarkFinished)
	Register(db2contract.StageMiningJobTryLock,
		db2contract.OperationMiningJobTryLock, miningJobTryLock)
	Register(db2contract.StageSynthMarkFailed,
		db2contract.OperationSynthMarkFailed, synthMarkFailed)
	Register(db2contract.StageRuntimeStateSet,
		db2contract.OperationRuntimeStateSet, runtimeStateSet)
	Register(db2contract.StageSetActiveEmbedderVersion,
		db2contract.OperationSetActiveEmbedderVersion, setActiveEmbedderVersion)
}

const (
	// The three expiry sweeps share a shape: a state, a bound that may be
	// absent, and the bound having passed. The empty-string test is the one
	// that matters -- these columns are NOT NULL with an empty default, and an
	// empty string sorts before every stamp, so without it everything with no
	// deadline expires at once.
	prospectiveSweepExpiredQuery = `UPDATE prospective_memories
 SET state = 'expired', updated_at = pg_now_text()
 WHERE state = 'armed' AND valid_until <> '' AND valid_until < pg_now_text()`

	directiveSweepExpiredQuery = `UPDATE epistemic_directives
 SET state = 'expired', updated_at = pg_now_text()
 WHERE state = 'open' AND valid_until <> '' AND valid_until < pg_now_text()`

	markRevisitDueQuery = `UPDATE decision_log SET status = 'revisit_due'
 WHERE status = 'active' AND revisit_when <> ''
   AND revisit_when <= pg_now_text()`

	// A running ingest with no worker behind it is a crashed one. Resetting it
	// to pending is what lets the next drain pick it up; started_at goes back
	// to NULL so the row does not look like it is making progress.
	ingestQueueResetRunningQuery = `UPDATE kb_ingest_queue
 SET status = 'pending', started_at = NULL WHERE status = 'running'`

	evidenceReembedAllQuery = `UPDATE evidence_index_ops
 SET status = 'pending', attempts = 0, last_error = ''`

	// Committed curator artifacts go back to proposed, which is what makes the
	// curator re-derive their vectors: the vector is rebuilt from the
	// authoritative artifact rather than re-embedded in place.
	curatorReembedAllQuery = `UPDATE artifacts SET state = 'proposed'
 WHERE state = 'committed'
   AND kind IN ('doc_summary','synthesis','open_question','claim','entity',
                'code_unit')`

	synthReenqueueAllQuery = `UPDATE learning_synth_ops
 SET status = 'pending', attempts = 0, last_error = ''`

	directiveSuppressQuery = `UPDATE epistemic_directives
 SET state = 'suppressed', updated_at = pg_now_text()
 WHERE id = $1 AND state = 'open'`

	directiveRecordSurfaceQuery = `UPDATE epistemic_directives
 SET surfaced_count = surfaced_count + 1, last_surfaced_at = pg_now_text(),
     updated_at = pg_now_text()
 WHERE id = $1 AND state = 'open'`

	asyncPendingCountQuery = `SELECT COUNT(*) FROM kb_async_jobs
 WHERE kind = $1 AND status = 'pending'`

	runtimeStateTouchQuery = `INSERT INTO kb_runtime_state
 (state_key, state_value) VALUES ($1, pg_now_text())
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`

	runtimeStateSetQuery = `INSERT INTO kb_runtime_state
 (state_key, state_value) VALUES ($1, $2)
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`

	synthEnqueueQuery = `INSERT INTO learning_synth_ops (artifact_id)
 VALUES ($1) ON CONFLICT DO NOTHING`

	synthMarkDoneQuery = `UPDATE learning_synth_ops SET status = 'ok',
     updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE artifact_id = $1`

	synthMarkFailedQuery = `UPDATE learning_synth_ops SET status = 'failed',
     attempts = attempts + 1, last_error = $2,
     updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE artifact_id = $1`

	// Only the row that has not finished. A re-embed already marked finished
	// keeps its original stamp, so a second call cannot move the record of when
	// the work actually ended.
	reembedMarkFinishedQuery = `UPDATE memory_reembed_progress
 SET finished_at = $1 WHERE id = 1 AND finished_at IS NULL`

	setActiveEmbedderVersionQuery = `INSERT INTO memory_active_embedder
 (id, version, updated_at) VALUES (1, $1, $2)
 ON CONFLICT (id) DO UPDATE
 SET version = EXCLUDED.version, updated_at = EXCLUDED.updated_at`

	// A session-level advisory lock, not a transaction one: the miner holds it
	// across statements and releases it explicitly when the job ends.
	miningJobTryLockQuery = `SELECT pg_try_advisory_lock(hashtext($1))`
)

// Extraction jobs for every current document, and every stale job reset.
//
// The insert covers documents with no job at all; the update covers documents
// whose job exists and has moved on. The C runs them as two statements because
// its SQLite test shim will not take an upsert, and the count it answers is of
// jobs afterwards rather than of rows it moved -- which is the number the
// caller wants: how much extraction work is now queued.
const curatorReenqueueExtractAllQuery = `WITH inserted AS (
   INSERT INTO kb_async_jobs (kind, document_id, project, status)
   SELECT 'extract_doc', d.id, d.project, 'pending'
     FROM kb_documents d
     JOIN projects p ON p.name = d.project
    WHERE NOT EXISTS (SELECT 1 FROM kb_async_jobs j
       WHERE j.kind = 'extract_doc' AND j.document_id = d.id)
      AND p.lifecycle_state = 'current'
      AND d.generation = p.current_generation
   RETURNING 1
 ), reset AS (
   UPDATE kb_async_jobs SET status = 'pending'
    WHERE kind = 'extract_doc' AND status <> 'pending'
      AND EXISTS (SELECT 1 FROM kb_documents d
        JOIN projects p ON p.name = d.project
        WHERE d.id = kb_async_jobs.document_id
          AND p.lifecycle_state = 'current'
          AND d.generation = p.current_generation)
   RETURNING 1
 )
 SELECT COUNT(*) FROM kb_async_jobs j
  WHERE j.kind = 'extract_doc'
    AND EXISTS (SELECT 1 FROM kb_documents d
      JOIN projects p ON p.name = d.project
      WHERE d.id = j.document_id AND p.lifecycle_state = 'current'
        AND d.generation = p.current_generation)`

// countingSweep runs a statement whose reply is how many rows it moved.
func countingSweep(ctx context.Context, store Store, query string,
	encode func(uint32) ([]byte, error)) ([]byte, bus.ModuleStatus) {
	moved, execErr := store.Exec(ctx, query)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(moved, encode)
}

func prospectiveSweepExpired(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeProspectiveSweepExpiredRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, prospectiveSweepExpiredQuery,
		db2contract.EncodeProspectiveSweepExpiredReply)
}

func directiveSweepExpired(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeDirectiveSweepExpiredRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, directiveSweepExpiredQuery,
		db2contract.EncodeDirectiveSweepExpiredReply)
}

func markRevisitDue(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeMarkRevisitDueRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, markRevisitDueQuery,
		db2contract.EncodeMarkRevisitDueReply)
}

func ingestQueueResetRunning(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeIngestQueueResetRunningRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, ingestQueueResetRunningQuery,
		db2contract.EncodeIngestQueueResetRunningReply)
}

func evidenceReembedAll(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEvidenceReembedAllRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, evidenceReembedAllQuery,
		db2contract.EncodeEvidenceReembedAllReply)
}

func curatorReembedAll(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCuratorReembedAllRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, curatorReembedAllQuery,
		db2contract.EncodeCuratorReembedAllReply)
}

func synthReenqueueAll(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeSynthReenqueueAllRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countingSweep(ctx, store, synthReenqueueAllQuery,
		db2contract.EncodeSynthReenqueueAllReply)
}

func curatorReenqueueExtractAll(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCuratorReenqueueExtractAllRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, curatorReenqueueExtractAllQuery,
		db2contract.EncodeCuratorReenqueueExtractAllReply)
}

func directiveSuppress(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	directiveID, err := db2contract.DecodeDirectiveSuppressRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, directiveSuppressQuery,
		infallible(db2contract.EncodeDirectiveSuppressReply), int64(directiveID))
}

func directiveRecordSurface(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	directiveID, err :=
		db2contract.DecodeDirectiveRecordSurfaceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, directiveRecordSurfaceQuery,
		infallible(db2contract.EncodeDirectiveRecordSurfaceReply),
		int64(directiveID))
}

func asyncPendingCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	kind, err := db2contract.DecodeAsyncPendingCountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, asyncPendingCountQuery,
		db2contract.EncodeAsyncPendingCountReply, kind)
}

func runtimeStateTouch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, err := db2contract.DecodeRuntimeStateTouchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, runtimeStateTouchQuery, key); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeRuntimeStateTouchReply)
}

func runtimeStateSet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, value, err := db2contract.DecodeRuntimeStateSetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, runtimeStateSetQuery, key,
		value); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeRuntimeStateSetReply)
}

func synthEnqueue(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeSynthEnqueueRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, synthEnqueueQuery,
		artifactID); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeSynthEnqueueReply)
}

func synthMarkDone(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeSynthMarkDoneRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, synthMarkDoneQuery,
		artifactID); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeSynthMarkDoneReply)
}

func synthMarkFailed(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, message, err :=
		db2contract.DecodeSynthMarkFailedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, synthMarkFailedQuery, artifactID,
		message); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeSynthMarkFailedReply)
}

func reembedMarkFinished(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	finishedAt, err := db2contract.DecodeReembedMarkFinishedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, reembedMarkFinishedQuery,
		finishedAt); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeReembedMarkFinishedReply)
}

func setActiveEmbedderVersion(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	version, updatedAt, err :=
		db2contract.DecodeSetActiveEmbedderVersionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, setActiveEmbedderVersionQuery, version,
		updatedAt); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeSetActiveEmbedderVersionReply)
}

// miningJobTryLock takes a session advisory lock named after the job.
//
// Session-scoped, not transaction-scoped, which is what the C's
// pg_try_advisory_lock does and what a miner needs: the lock has to outlive the
// statement that took it and be released explicitly when the job finishes.
//
// That makes it lock the pooled connection rather than the caller, and the
// connection goes back to the pool still holding it. Naming the hazard here
// because it is the C's and this port cannot fix it alone: a lock the next
// borrower of that connection inherits is only safe while one miner runs at a
// time, which is the arrangement today.
func miningJobTryLock(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	jobID, err := db2contract.DecodeMiningJobTryLockRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var locked bool
	if scanErr := store.QueryRow(ctx, miningJobTryLockQuery, jobID).
		Scan(&locked); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	acquired := uint32(0)
	if locked {
		acquired = 1
	}
	reply, encodeErr := db2contract.EncodeMiningJobTryLockReply(acquired)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
