package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCorpusPipelineStatus,
		db2contract.OperationCorpusPipelineStatus, corpusPipelineStatus)
	Register(db2contract.StageKBProjectStatus,
		db2contract.OperationKBProjectStatus, kbProjectStatus)
	Register(db2contract.StageKBReembedStatus,
		db2contract.OperationKBReembedStatus, kbReembedStatus)
	Register(db2contract.StageKBReleaseRead,
		db2contract.OperationKBReleaseRead, kbReleaseRead)
	Register(db2contract.StageMiningJobGet,
		db2contract.OperationMiningJobGet, miningJobGet)
	Register(db2contract.StageKBIngestQueueRecent,
		db2contract.OperationKBIngestQueueRecent, kbIngestQueueRecent)
}

// The four job states and, separately, the count of skipped transitions.
//
// The skipped count comes from the event log rather than the job rows, and the
// C explains why at length: a job row only knows where a document got to, not
// how much of the journey was a no-op. A drain reporting fourteen stages
// processed and none failed looked like a fully processed document and was in
// fact eight no-ops -- so a document could finish with no chunks and no claims
// while its pipeline said complete.
//
// A scalar subquery rather than a second round trip, which also makes the two
// counts consistent with each other: the C reads them separately and a job
// advancing between the two calls is counted in one and not the other.
const corpusPipelineStatusQuery = `SELECT
 COUNT(*),
 COUNT(*) FILTER (WHERE stage_status = 'pending'),
 COUNT(*) FILTER (WHERE stage_status = 'running'),
 COUNT(*) FILTER (WHERE stage_status = 'failed'),
 COUNT(*) FILTER (WHERE stage_status = 'complete'),
 (SELECT COUNT(*) FROM corpus_stage_events WHERE outcome = 'skipped')
 FROM corpus_processing_jobs`

// corpusPipelineStatus counts what the document pipeline is holding.
//
// The reply's corpus_processed field is always zero, like the async queue's:
// the C's status call never sets it -- only the drain does, on the same struct
// -- so the adapter has been encoding a zero. Said here so it is not mistaken
// for a count that failed to arrive.
func corpusPipelineStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeCorpusPipelineStatusRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var total, pending, running, failed, complete, skipped int64
	if err := store.QueryRow(ctx, corpusPipelineStatusQuery).Scan(&total, &pending,
		&running, &failed, &complete, &skipped); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCorpusPipelineStatusReply(
		clampToU32(total), clampToU32(pending), clampToU32(running),
		clampToU32(failed), clampToU32(complete), 0, clampToU32(skipped))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// One statement where the C makes two, which also makes the four counts
// consistent: the C reads the document counts and the embedding count
// separately, so an embedding landing between the two calls is counted in one
// and not the other -- and this read exists to be compared against itself.
//
// The collection filter names both spellings, and the C carries a long note
// about why. The writer records general-corpus vectors under 'kb_embeddings';
// 'kb_chunks' is the older name and still appears in existing stores. Matching
// only the legacy one made this count zero on every current deployment, so
// three separate surfaces told operators the embedder was broken while it was
// working.
const kbProjectStatusQuery = `SELECT COUNT(*), COALESCE(SUM(d.token_count), 0),
 COUNT(DISTINCT d.file_path),
 (SELECT COUNT(*) FROM vector_index_ops q
   JOIN kb_documents qd ON qd.id = q.point_id
   JOIN projects qp ON qp.name = qd.project
   WHERE ($1 = '' OR qd.project = $1)
     AND q.collection IN ('kb_chunks', 'kb_embeddings')
     AND qp.lifecycle_state = 'current'
     AND qd.generation = qp.current_generation
     AND q.status = 'ok')
 FROM kb_documents d JOIN projects p ON p.name = d.project
 WHERE ($1 = '' OR d.project = $1) AND p.lifecycle_state = 'current'
 AND d.generation = p.current_generation`

// kbProjectStatus counts what has been ingested for a project.
//
// An empty project name means every project, which is what makes this also the
// whole-corpus answer. The name comes back in the reply as given: the C routes
// it through a resolver that only copies it, so there is nothing to resolve.
//
// Files are counted distinct and chunks are not, because a file is chunked into
// many documents -- the two numbers together are what say whether ingestion is
// producing sensible pieces.
func kbProjectStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeKBProjectStatusRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var chunks, tokens, files, embeddings int64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, kbProjectStatusQuery, project).
		Scan(&chunks, &tokens, &files, &embeddings); scanErr != nil {
		found, chunks, tokens, files, embeddings = 0, 0, 0, 0, 0
	}
	reply, encodeErr := db2contract.EncodeKBProjectStatusReply(found, project,
		clampToU32(files), clampToU32(chunks), clampToU32(tokens),
		clampToU32(embeddings))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// A singleton keyed on id = 1: one re-embedding run at a time, because the
// whole point of the row is to say where the current one has got to.
const kbReembedStatusQuery = `SELECT target_version, last_id, total, done,
 started_at, COALESCE(finished_at, '')
 FROM memory_reembed_progress WHERE id = 1`

// kbReembedStatus answers how far a re-embedding run has got.
//
// have_job separates "no run has ever been started" from "a run that has done
// nothing yet", which encode identically in the counts. finished_at is the
// nullable one -- a run in progress has not finished -- and empty is how the
// reply says so.
func kbReembedStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeKBReembedStatusRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var targetVersion, startedAt, finishedAt string
	var lastID, total, done int64
	haveJob := uint32(1)
	if scanErr := store.QueryRow(ctx, kbReembedStatusQuery).Scan(&targetVersion,
		&lastID, &total, &done, &startedAt, &finishedAt); scanErr != nil {
		haveJob, lastID, total, done = 0, 0, 0, 0
		targetVersion, startedAt, finishedAt = "", "", ""
	}
	reply, encodeErr := db2contract.EncodeKBReembedStatusReply(haveJob,
		targetVersion, clampToU32(lastID), clampToU32(total), clampToU32(done),
		startedAt, finishedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The C selects the identifier the caller supplied and never reads it, so it is
// dropped. Both timestamps are nullable -- a draft release has been neither
// promoted nor retired -- and empty is what the reply says for absent.
const kbReleaseReadQuery = `SELECT name, state, COALESCE(promoted_at, ''),
 COALESCE(retired_at, ''), created_at
 FROM doc_releases WHERE id = $1`

// kbReleaseRead answers what a documentation release is and when it moved.
func kbReleaseRead(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	releaseID, err := db2contract.DecodeKBReleaseReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var name, state, promotedAt, retiredAt, createdAt string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, kbReleaseReadQuery, int64(releaseID)).
		Scan(&name, &state, &promotedAt, &retiredAt, &createdAt); scanErr != nil {
		found = 0
		name, state, promotedAt, retiredAt, createdAt = "", "", "", "", ""
	}
	reply, encodeErr := db2contract.EncodeKBReleaseReadReply(found, name, state,
		promotedAt, retiredAt, createdAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// COALESCE on both nullable columns, as the C has them: a job that has never
// run has no last run and no last error, and both read as empty.
const miningJobGetQuery = `SELECT COALESCE(last_run_at, ''), hwm, interval_s,
 enabled, COALESCE(last_error, '')
 FROM mining_jobs WHERE id = $1`

// miningJobGet answers when a mining job last ran and how far it got.
//
// The high water mark is what makes a mining job resumable: it is the last row
// the job consumed, so the next run starts there rather than re-reading
// everything. A job with no mark starts from the beginning, which is the same
// thing said differently.
func miningJobGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	jobID, err := db2contract.DecodeMiningJobGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var lastRunAt, lastError string
	var highWater, intervalSeconds int64
	// mining_jobs.enabled is BOOLEAN, not an integer flag. Scanning it into an
	// int64 fails the whole read, so this answered "no such job" for a job that
	// was there -- which the parity run caught by finding a row the C found and
	// this did not.
	var enabled bool
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, miningJobGetQuery, jobID).Scan(&lastRunAt,
		&highWater, &intervalSeconds, &enabled, &lastError); scanErr != nil {
		found, highWater, intervalSeconds, enabled = 0, 0, 0, false
		lastRunAt, lastError = "", ""
	}
	enabledFlag := uint32(0)
	if enabled {
		enabledFlag = 1
	}
	if highWater < 0 {
		highWater = 0
	}
	reply, encodeErr := db2contract.EncodeMiningJobGetReply(found, lastRunAt,
		uint64(highWater), clampToU32(intervalSeconds), enabledFlag, lastError)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Active work first, then everything else by recency. A queue view is read to
// answer "what is happening now", and a completed job from an hour ago is not
// that -- so running leads, then pending, then the rest.
//
// The timestamp falls back through completion, start and queueing, so the
// column is populated for every row whatever state it is in. NULLS LAST keeps
// a row with none of the three from leading the recency order, which in
// PostgreSQL is where a descending sort would otherwise put it.
const kbIngestQueueRecentQuery = `SELECT project, status,
 COALESCE(completed_at, started_at, queued_at),
 files_indexed, chunks_added, COALESCE(error_message, '')
 FROM kb_ingest_queue
 ORDER BY
   CASE status WHEN 'running' THEN 0 WHEN 'pending' THEN 1 ELSE 2 END,
   COALESCE(completed_at, started_at, queued_at) DESC NULLS LAST
 LIMIT $1`

// kbIngestQueueRecent lists what the ingest queue has been doing.
func kbIngestQueueRecent(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	rowLimit, err := db2contract.DecodeKBIngestQueueRecentRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.KBIngestQueueRecentMaxRows
	rows, queryErr := store.Query(ctx, kbIngestQueueRecentQuery,
		int64(pairLimit(rowLimit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	jobs := make([]db2contract.KBIngestQueueRecentRow, 0, 16)
	for rows.Next() {
		var project, status, at, errorMessage string
		var files, chunks int64
		if scanErr := rows.Scan(&project, &status, &at, &files, &chunks,
			&errorMessage); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		jobs = append(jobs, db2contract.KBIngestQueueRecentRow{
			ProjectName:  project,
			JobStatus:    status,
			CompletedAt:  at,
			FilesIndexed: clampToU32(files),
			ChunksAdded:  clampToU32(chunks),
			ErrorMessage: errorMessage,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeKBIngestQueueRecentReply(jobs)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
