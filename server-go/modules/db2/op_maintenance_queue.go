package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageIngestQueueFail,
		db2contract.OperationIngestQueueFail, ingestQueueFail)
	Register(db2contract.StageKBDocumentsDeleteForFile,
		db2contract.OperationKBDocumentsDeleteForFile, kbDocumentsDeleteForFile)
	Register(db2contract.StageKBDocumentsLinkNeighbours,
		db2contract.OperationKBDocumentsLinkNeighbours, kbDocumentsLinkNeighbours)
	Register(db2contract.StageRetryableIndexFailures,
		db2contract.OperationRetryableIndexFailures, retryableIndexFailures)
	Register(db2contract.StageWitnessCheckpointFreshness,
		db2contract.OperationWitnessCheckpointFreshness, witnessCheckpointFreshness)
}

const ingestQueueFailQuery = `UPDATE kb_ingest_queue
 SET status = 'failed',
     completed_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'),
     error_message = $2
 WHERE id = $1`

// ingestQueueFail records that an ingest job could not finish.
//
// completed_at is stamped even though the job did not succeed, because the
// column means "stopped running" rather than "succeeded" -- the dedup index on
// this table admits one pending-or-running row per project, and a failed job
// that never completed would hold that slot forever.
//
// No state predicate and no row check, matching the C: a job already recorded
// failed takes the newer message, which is the right answer when a retry fails
// differently.
func ingestQueueFail(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	jobID, message, err := db2contract.DecodeIngestQueueFailRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, ingestQueueFailQuery, int64(jobID), message)
	return acknowledgement(execErr == nil, db2contract.EncodeIngestQueueFailReply)
}

const kbDocumentsDeleteForFileQuery = `DELETE FROM kb_documents
 WHERE project = $1 AND file_path = $2
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current')`

// kbDocumentsDeleteForFile removes a file's chunks from the generation being
// built.
//
// Scoped to the current generation, so a re-ingest cannot empty a published
// one. The vectors behind the chunks are not touched here: a caller purges
// those by the identifiers document_chunk_ids hands it, which is why that read
// exists as a separate operation rather than being folded into this.
func kbDocumentsDeleteForFile(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err := db2contract.DecodeKBDocumentsDeleteForFileRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, kbDocumentsDeleteForFileQuery, project, filePath)
	return acknowledgement(execErr == nil, db2contract.EncodeKBDocumentsDeleteForFileReply)
}

// The two halves of one link. A chunk points back at its predecessor and the
// predecessor points forward at it, and a chain with only one of those is worse
// than no chain: context expansion walks it in one direction and stops in the
// other, so a caller reading around a chunk silently gets half the neighbourhood.
const (
	linkPreviousQuery = `UPDATE kb_documents SET prev_chunk_id = $1 WHERE id = $2`
	linkNextQuery     = `UPDATE kb_documents SET next_chunk_id = $1 WHERE id = $2`
)

// kbDocumentsLinkNeighbours joins two adjacent chunks in both directions.
//
// One transaction, which the C does not do: it issues the two updates
// separately and ignores both results, so a failure between them leaves the
// chain half-linked. Neither statement is required to match a row -- a chunk
// that has since been deleted links to nothing and that is not an error -- but
// they must either both land or neither.
func kbDocumentsLinkNeighbours(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	docID, previousID, err := db2contract.DecodeKBDocumentsLinkNeighboursRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The C refuses either identifier being absent before it prepares anything:
	// a link needs two ends.
	if docID == 0 || previousID == 0 {
		return acknowledgement(false, db2contract.EncodeKBDocumentsLinkNeighboursReply)
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, linkPreviousQuery,
			int64(previousID), int64(docID)); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, linkNextQuery, int64(docID), int64(previousID))
		return err
	})
	return acknowledgement(txErr == nil, db2contract.EncodeKBDocumentsLinkNeighboursReply)
}

// GROUP BY rather than SELECT DISTINCT. PostgreSQL refuses to order a DISTINCT
// by a column that is not selected -- it is an error, not a warning -- so the C
// form never ran and this retry queue always came back empty, leaving every
// failed embed failed. Grouping gives the same one-row-per-memory result and
// lets the ordering be an aggregate over the rows behind it.
//
// MIN(updated_at) because a memory can have several failed points: its place in
// the queue is how long it has had any failure outstanding, which is what
// draining oldest-first means here.
//
// NULLIF on the limit covers the C's two branches, which append a LIMIT clause
// only when the limit is positive.
const retryableIndexFailuresQuery = `SELECT memory_id FROM vector_index_ops
 WHERE status = 'failed' AND memory_id IS NOT NULL AND attempts < $1
 GROUP BY memory_id
 ORDER BY MIN(updated_at) ASC
 LIMIT NULLIF($2, 0)`

// retryableIndexFailures lists memories whose indexing failed and may be tried
// again.
//
// Bounded by attempts rather than by age: a memory that has failed its limit is
// not retryable however long ago it failed, because the limit is what stops a
// permanently broken embed from being retried forever.
func retryableIndexFailures(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	maxAttempts, limit, err := db2contract.DecodeRetryableIndexFailuresRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ids, status := readIntColumn(ctx, store, db2contract.RetryableIndexFailuresMaxRows,
		retryableIndexFailuresQuery, int64(maxAttempts), int64(limit))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.RetryableIndexFailuresRow, 0, len(ids))
	for _, id := range ids {
		found = append(found, db2contract.RetryableIndexFailuresRow{MemoryID: clampToU64(id)})
	}
	reply, err := db2contract.EncodeRetryableIndexFailuresReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const witnessCheckpointFreshnessQuery = `SELECT count(*),
 COALESCE(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - MAX(created_at)::timestamp))::bigint, 0)
 FROM kb_vault_witness_checkpoint`

// witnessCheckpointFreshness reports how many witness checkpoints exist and how
// old the newest is.
//
// The read flag is what makes the age readable: an age of zero means either a
// checkpoint written this second or no checkpoints at all, and those are
// opposite states. COALESCE turns the second into zero rather than NULL, so
// without the flag a monitor would read "perfectly fresh" from an empty table.
func witnessCheckpointFreshness(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeWitnessCheckpointFreshnessRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var count, age *int64
	if scanErr := store.QueryRow(ctx, witnessCheckpointFreshnessQuery).
		Scan(&count, &age); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeWitnessCheckpointFreshnessReply(
		1, clampToU64(number(count)), clampToU64(number(age)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
