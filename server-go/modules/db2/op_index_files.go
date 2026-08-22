package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCodeFileUpsert,
		db2contract.OperationCodeFileUpsert, codeFileUpsert)
	Register(db2contract.StageFileModifiedSince,
		db2contract.OperationFileModifiedSince, fileModifiedSince)
	Register(db2contract.StageProjectionGenerationsList,
		db2contract.OperationProjectionGenerationsList, projectionGenerationsList)
	Register(db2contract.StageKBIngestQueueStats,
		db2contract.OperationKBIngestQueueStats, kbIngestQueueStats)
	Register(db2contract.StageAsyncEnqueue,
		db2contract.OperationAsyncEnqueue, asyncEnqueue)
	Register(db2contract.StageKBDocumentsSetTsrState,
		db2contract.OperationKBDocumentsSetTsrState, kbDocumentsSetTsrState)
}

// INSERT ... SELECT again, so the generation comes from the project row rather
// than the caller and a project that is not current inserts nothing. The
// conflict target is the same triple the unique index carries, which is what
// makes a re-scan of an unchanged file an update rather than a duplicate.
const codeFileUpsertQuery = `INSERT INTO files
 (project_id, generation, path, scanned_at, language, vendored)
 SELECT $1, current_generation, $2, $3, $4, $5 FROM projects
 WHERE id = $1 AND lifecycle_state = 'current'
 ON CONFLICT (project_id, generation, path) DO UPDATE
 SET scanned_at = $3, language = $4, vendored = $5
 RETURNING id`

// codeFileUpsert records that a file exists in the generation being built.
//
// The language and vendored flag are derived here from the path, because the
// request carries neither -- the C derives them the same way at the same point.
// They are re-derived on every upsert rather than only on insert, so a
// classifier change takes effect on the next scan instead of only for files
// that have never been seen.
func codeFileUpsert(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	projectID, filePath, scannedAt, err := db2contract.DecodeCodeFileUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	vendored := 0
	if pathIsVendored(filePath) {
		vendored = 1
	}
	id, status := readOptionalInt(ctx, store, codeFileUpsertQuery,
		int64(projectID), filePath, scannedAt, languageFromPath(filePath), vendored)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeCodeFileUpsertReply(clampToU64(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The comparison is done in the statement rather than by parsing the stamp in
// Go. scanned_at holds two spellings -- the ISO form and the space-separated
// one pg_now_text writes -- and a cast handles both, where the C once had a
// parser that read only the first and re-indexed every file on every scan
// because of it.
//
// The regular expression is the guard a cast cannot have: PostgreSQL has no
// try-cast, and a stamp in neither format would raise rather than answering.
// An unrecognised stamp reads as modified, which is the C's behaviour and the
// safe direction -- re-indexing something unchanged costs work, skipping
// something changed costs correctness.
const fileModifiedSinceQuery = `SELECT CASE
 WHEN f.scanned_at ~ '^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9]{2}:[0-9]{2}:[0-9]{2}'
  AND EXTRACT(EPOCH FROM (f.scanned_at::timestamp AT TIME ZONE 'UTC'))::bigint >= $3
 THEN 0 ELSE 1 END
 FROM files f
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND f.path = $2
 AND p.lifecycle_state = 'current'
 AND f.generation = p.current_generation`

// fileModifiedSince reports whether a file needs re-indexing.
//
// A file with no row answers modified, which is how a file new to this
// generation gets indexed at all.
func fileModifiedSince(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	projectID, filePath, since, err := db2contract.DecodeFileModifiedSinceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var verdict *int64
	modified := uint32(1)
	if scanErr := store.QueryRow(ctx, fileModifiedSinceQuery,
		int64(projectID), filePath, int64(since)).Scan(&verdict); scanErr == nil {
		modified = clampToU32(number(verdict))
	}
	reply, err := db2contract.EncodeFileModifiedSinceReply(modified)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const projectionGenerationsListQuery = `SELECT id, state, started_at
 FROM code_projection_generations
 WHERE project = $1 ORDER BY id DESC LIMIT $2`

// projectionGenerationsList lists a project's projection generations, newest
// first.
//
// Every state, including aborted and superseded ones: this is the history a
// person reads to see what happened, and filtering it to the visible one would
// answer a question they can already ask another way.
func projectionGenerationsList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectionGenerationsListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProjectionGenerationsListMaxRows
	rows, queryErr := store.Query(ctx, projectionGenerationsListQuery, project, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.ProjectionGenerationsListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var state, startedAt *string
		if err := rows.Scan(&id, &state, &startedAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.ProjectionGenerationsListRow{
			Generation: clampToU64(number(id)),
			State:      text(state),
			StartedAt:  text(startedAt),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProjectionGenerationsListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// One statement where the C issues three: a grouped count for the live states
// and two more for the finished ones. Filtered aggregates give the same four
// numbers from a single pass, and from one snapshot -- the C's three statements
// can disagree with each other when a job finishes between them, so its pending
// count and its done count need not describe the same moment.
const kbIngestQueueStatsQuery = `SELECT
 COUNT(*) FILTER (WHERE status = 'pending'),
 COUNT(*) FILTER (WHERE status = 'running'),
 COUNT(*) FILTER (WHERE status = 'done' AND completed_at >= pg_now_text('-1 day')),
 COUNT(*) FILTER (WHERE status = 'failed' AND completed_at >= pg_now_text('-1 day'))
 FROM kb_ingest_queue`

// kbIngestQueueStats reports how much ingest work is outstanding and how much
// finished recently.
//
// The two live counts have no time bound and the two finished ones do. That is
// the right asymmetry: a job pending for a week is still pending, while a job
// that finished last month says nothing about how the queue is doing now.
func kbIngestQueueStats(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeKBIngestQueueStatsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var pending, running, done, failed *int64
	if scanErr := store.QueryRow(ctx, kbIngestQueueStatsQuery).
		Scan(&pending, &running, &done, &failed); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeKBIngestQueueStatsReply(
		clampToU32(number(pending)), clampToU32(number(running)),
		clampToU32(number(done)), clampToU32(number(failed)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const asyncEnqueueQuery = `INSERT INTO kb_async_jobs
 (kind, document_id, project, status, updated_at)
 VALUES ($1, $2, $3, 'pending',
         to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'))
 ON CONFLICT (kind, document_id) DO NOTHING`

// asyncEnqueue queues background work for a document.
//
// One job per kind and document, enforced by the conflict target: enqueuing the
// same work twice while the first is still pending leaves the first alone
// rather than queuing a duplicate. Note what that means for a job already
// marked done -- the row is still there, so re-enqueuing does not re-arm it.
// Re-running finished work needs the row moved back to pending, which this
// operation does not do.
func asyncEnqueue(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	kind, documentID, project, err := db2contract.DecodeAsyncEnqueueRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, asyncEnqueueQuery, kind, int64(documentID), project)
	return acknowledgement(execErr == nil, db2contract.EncodeAsyncEnqueueReply)
}

const kbDocumentsSetTsrStateQuery = `UPDATE kb_documents SET tsr_state = $3
 WHERE project = $1 AND file_path = $2 AND doc_kind = 'pdf'
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current')`

// kbDocumentsSetTsrState records how far table-structure recognition has got.
//
// Writes every chunk of the file, not one row: the state describes the document
// rather than a chunk, and the read beside it takes whichever chunk answers
// first. Keeping them equal is what makes that read well-defined.
//
// No quarantine check here, unlike the read. A document awaiting review still
// has its recognition state recorded -- what is withheld is telling a caller
// about it, not doing the work.
func kbDocumentsSetTsrState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, state, err := db2contract.DecodeKBDocumentsSetTsrStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, kbDocumentsSetTsrStateQuery, project, filePath, state)
	return acknowledgement(execErr == nil, db2contract.EncodeKBDocumentsSetTsrStateReply)
}
