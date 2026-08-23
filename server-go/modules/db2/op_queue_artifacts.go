package db2

import (
	"context"
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageKBAsyncQueueStatus,
		db2contract.OperationKBAsyncQueueStatus, kbAsyncQueueStatus)
	Register(db2contract.StageKBIngestQueueClaimNext,
		db2contract.OperationKBIngestQueueClaimNext, kbIngestQueueClaimNext)
	Register(db2contract.StageVectorIndexOpRecord,
		db2contract.OperationVectorIndexOpRecord, vectorIndexOpRecord)
	Register(db2contract.StageArtifactReject,
		db2contract.OperationArtifactReject, artifactReject)
	Register(db2contract.StageFidelityReportByTurn,
		db2contract.OperationFidelityReportByTurn, fidelityReportByTurn)
}

// Filtered aggregates, and the total is a plain count rather than the sum of
// the four. That difference is the C's and it is deliberate there: a job in a
// status nobody named still exists, and a total that only counted the four
// known ones would quietly under-report the queue.
const kbAsyncQueueStatusQuery = `SELECT
 COUNT(*) FILTER (WHERE status = 'pending'),
 COUNT(*) FILTER (WHERE status = 'running'),
 COUNT(*) FILTER (WHERE status = 'done'),
 COUNT(*) FILTER (WHERE status = 'failed'),
 COUNT(*)
 FROM kb_async_jobs`

// kbAsyncQueueStatus counts what the asynchronous job queue is holding.
//
// The reply carries a sixth field, queue_processed, and it is always zero. The
// C's backend never writes it -- the struct is zeroed and only the five counts
// above are filled -- so the adapter has been encoding a zero since the field
// was added. It is answered as zero here for the same reason, and named so the
// next person does not go looking for the query that fills it.
func kbAsyncQueueStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeKBAsyncQueueStatusRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var pending, running, done, failed, total int64
	if err := store.QueryRow(ctx, kbAsyncQueueStatusQuery).Scan(
		&pending, &running, &done, &failed, &total); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeKBAsyncQueueStatusReply(
		clampToU32(pending), clampToU32(running), clampToU32(done),
		clampToU32(failed), clampToU32(total), 0)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Claim and mark in one statement, which is what makes it a claim rather than a
// read: the row is selected FOR UPDATE, skipped if another worker holds it, and
// moved to running before anyone else can see it.
//
// SKIP LOCKED is the part that lets several workers share the queue. Without
// it a second worker blocks on the first's row and then claims it anyway when
// the lock clears -- the classic double-claim.
//
// Priority first, then arrival order within a priority, so a bulk reindex
// cannot starve work a caller is blocked on and equal-priority jobs keep their
// order.
const kbIngestQueueClaimNextQuery = `UPDATE kb_ingest_queue
 SET status = 'running', started_at = pg_now_text()
 WHERE id = (
   SELECT id FROM kb_ingest_queue WHERE status = 'pending'
   ORDER BY priority DESC, id LIMIT 1 FOR UPDATE SKIP LOCKED
 ) RETURNING id, project, root_path, workspace, force`

// kbIngestQueueClaimNext takes the next ingest job for this worker.
//
// An empty queue is not a failure: the reply says no job was claimed, and a
// worker polling an idle queue gets that answer constantly.
func kbIngestQueueClaimNext(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeKBIngestQueueClaimNextRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var jobID, force int64
	var project, rootPath, workspace string
	claimed := uint32(1)
	if scanErr := store.QueryRow(ctx, kbIngestQueueClaimNextQuery).Scan(
		&jobID, &project, &rootPath, &workspace, &force); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		claimed, jobID, project, rootPath, workspace, force = 0, 0, "", "", "", 0
	}
	forceFlag := uint32(0)
	if force != 0 {
		forceFlag = 1
	}
	reply, encodeErr := db2contract.EncodeKBIngestQueueClaimNextReply(
		claimed, uint64(jobID), project, rootPath, workspace, forceFlag)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The embedding version is read from the table rather than taken from the
// caller, because the vector was produced by whichever embedder was active when
// it was produced and no caller knows that better than the table does.
//
// It is stamped only on success. A failed attempt produced no vector, so
// claiming one exists at the current version would be exactly the lie the
// column was added to stop -- and on the conflict path a successful re-index
// restamps, because a point re-embedded at a new version is at the new version.
//
// memory_id and indexed_at are NULLIF-ed rather than branched on. A memory of
// zero is not a memory, and an unindexed point has no index time; both columns
// are nullable and that is what they are nullable for.
const vectorIndexOpRecordQuery = `INSERT INTO vector_index_ops
 (point_id, collection, memory_id, status, attempts, last_error, indexed_at,
  embedding_version, updated_at)
 VALUES ($1, $2, NULLIF($3, 0), $4, 1, $5,
  CASE WHEN $4 = 'ok' THEN pg_now_text() ELSE NULL END,
  CASE WHEN $4 = 'ok'
       THEN COALESCE((SELECT version FROM memory_active_embedder WHERE id = 1), '')
       ELSE '' END,
  pg_now_text())
 ON CONFLICT (point_id) DO UPDATE SET
  collection = excluded.collection,
  status     = excluded.status,
  attempts   = vector_index_ops.attempts + 1,
  last_error = excluded.last_error,
  indexed_at = excluded.indexed_at,
  embedding_version = CASE WHEN excluded.status = 'ok'
                          THEN excluded.embedding_version
                          ELSE vector_index_ops.embedding_version END,
  updated_at = pg_now_text()`

// vectorIndexOpRecord records the outcome of indexing one point.
//
// attempts counts from one on insert and grows on every conflict, so a point
// that keeps failing carries the count of how many times it has been tried --
// which is what a retry policy reads.
//
// The error message is kept only for a failure. Carrying one alongside a
// success would leave the last failure's text attached to a point that is now
// fine.
func vectorIndexOpRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pointID, collection, memoryID, indexOK, errorMessage, err :=
		db2contract.DecodeVectorIndexOpRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	status := "failed"
	if indexOK != 0 {
		status = "ok"
		errorMessage = ""
	}
	_, execErr := store.Exec(ctx, vectorIndexOpRecordQuery,
		int64(pointID), collection, int64(memoryID), status, errorMessage)
	return acknowledgement(execErr == nil,
		db2contract.EncodeVectorIndexOpRecordReply)
}

// The state change and the audit event in one transaction.
//
// The C does them as two calls and gives up after the first if the second
// fails, which leaves an artifact rejected with nothing recording why. The
// audit trail is the whole point of a rejection carrying a verdict, so the two
// belong together.
const (
	artifactRejectStateQuery = `UPDATE artifacts SET state = 'rejected' WHERE id = $1`
	// The snapshots are JSONB. NULLIF before the cast, because an absent
	// snapshot arrives as an empty string and an empty string is not valid
	// JSON -- casting it directly fails the whole statement, which in the C
	// means the rejection lands and the audit event does not.
	artifactRejectAuditQuery = `INSERT INTO audit_events
 (id, source_artifact_id, target_surface, target_id, operator_id,
  scope_kind, scope_id, applied_at, applied_confidence, flagged_for_review,
  before_snapshot, after_snapshot)
 VALUES ($1, $2, '', '', '', 'user', '', pg_now_text(), 0.0, false,
  NULLIF($3, '')::jsonb, $4::jsonb)
 ON CONFLICT (id) DO NOTHING`
)

// artifactReject marks an artifact rejected and records why.
//
// The verdict fields are written into the audit event's after-snapshot rather
// than onto the artifact, so the artifact says what it is and the audit trail
// says what was decided about it. Empty fields are left out of that snapshot
// entirely -- an absent verdict tag is different from one someone set to the
// empty string, and only one of those is worth recording.
func artifactReject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, verdictTag, verdictScope, counterExample, beforeJSON, err :=
		db2contract.DecodeArtifactRejectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	after := map[string]string{"state": "rejected"}
	if verdictTag != "" {
		after["verdict_tag"] = verdictTag
	}
	if verdictScope != "" {
		after["verdict_scope"] = verdictScope
	}
	if counterExample != "" {
		after["counter_example"] = counterExample
	}
	// Marshalled rather than assembled by hand: a counter-example is free text
	// and will contain quotes. The key order Go produces differs from the C's,
	// and it does not matter -- the column is JSONB, which does not preserve
	// order in either case.
	afterJSON, marshalErr := json.Marshal(after)
	if marshalErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	auditID, idErr := newArtifactID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}

	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, artifactRejectStateQuery, artifactID); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, artifactRejectAuditQuery,
			auditID, artifactID, beforeJSON, string(afterJSON))
		return err
	})
	return acknowledgement(txErr == nil, db2contract.EncodeArtifactRejectReply)
}

// Newest first, one row. A turn can be judged more than once -- a re-run of the
// judge writes another report -- and the latest verdict is the one that counts.
const fidelityReportByTurnQuery = `SELECT payload FROM artifacts
 WHERE kind = 'fidelity_report' AND turn_id = $1
 ORDER BY created_at DESC LIMIT 1`

// The three answers this operation can give.
const (
	fidelityAbsent    uint32 = 0
	fidelityFound     uint32 = 1
	fidelityMalformed uint32 = 2
)

// fidelityReportByTurn answers how well a turn's claims were supported.
//
// A row whose payload cannot be read is reported as malformed rather than as an
// all-zero report. The C is explicit about why: a report saying nothing was
// supported and nothing was unsupported is a legitimate answer for a turn that
// made no claims, and a caller cannot tell that from a report it failed to
// parse unless the two are kept apart.
func fidelityReportByTurn(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	turnID, err := db2contract.DecodeFidelityReportByTurnRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var payload string
	if scanErr := store.QueryRow(ctx, fidelityReportByTurnQuery, turnID).
		Scan(&payload); scanErr != nil {
		return fidelityReply(fidelityAbsent, "", 0, 0, 0)
	}
	var report struct {
		Status      *string  `json:"status"`
		Supported   *float64 `json:"supported"`
		Unsupported *float64 `json:"unsupported"`
		Abstained   *float64 `json:"abstained"`
	}
	if json.Unmarshal([]byte(payload), &report) != nil {
		return fidelityReply(fidelityMalformed, "", 0, 0, 0)
	}
	status := ""
	if report.Status != nil {
		status = *report.Status
	}
	return fidelityReply(fidelityFound, status,
		countFromReport(report.Supported), countFromReport(report.Unsupported),
		countFromReport(report.Abstained))
}

// countFromReport reads one of the report's counts.
//
// A missing count is zero, and so is a negative one: the adapter clamps
// negatives away before they reach the wire, and a report claiming minus three
// supported claims is not one to pass on.
func countFromReport(value *float64) uint32 {
	if value == nil || *value <= 0 {
		return 0
	}
	return uint32(*value)
}

func fidelityReply(result uint32, status string,
	supported, unsupported, abstained uint32,
) ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeFidelityReportByTurnReply(
		result, status, supported, unsupported, abstained)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
