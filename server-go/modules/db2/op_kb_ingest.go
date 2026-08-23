package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageKBPurgeFenceRead,
		db2contract.OperationKBPurgeFenceRead, kbPurgeFenceRead)
	Register(db2contract.StageKBFileIndexGet,
		db2contract.OperationKBFileIndexGet, kbFileIndexGet)
	Register(db2contract.StageKBIngestQueueComplete,
		db2contract.OperationKBIngestQueueComplete, kbIngestQueueComplete)
	Register(db2contract.StageKBDocSetState,
		db2contract.OperationKBDocSetState, kbDocSetState)
}

// The identity row and the liveness of its heartbeat, in one read.
//
// The C makes two calls -- fetch the identity row, then ask whether the
// heartbeat is recent -- and only asks the second when the first found
// something. The LEFT JOIN keeps that: no identity row means no row at all,
// which is the same "no fence" answer, and the liveness column is only
// evaluated for a project that has one.
//
// The heartbeat is stored as text and cast back to a timestamp for the
// comparison, which is what the C does. It is written by pg_now_text() in UTC,
// so comparing against CURRENT_TIMESTAMP AT TIME ZONE 'UTC' compares two UTC
// wall clocks; a heartbeat written in any other zone would read as stale or
// live by the size of the offset.
//
// make_interval takes the window as a parameter rather than the C's approach of
// formatting the seconds into the statement text, which keeps one prepared
// statement for every TTL.
const kbPurgeFenceReadQuery = `SELECT fence.state_value, heartbeat.state_value IS NOT NULL
 AND heartbeat.state_value::timestamp >
 (CURRENT_TIMESTAMP AT TIME ZONE 'UTC') - make_interval(secs => $3)
 FROM kb_runtime_state fence
 LEFT JOIN kb_runtime_state heartbeat ON heartbeat.state_key = $2
 WHERE fence.state_key = $1`

// kbPurgeFenceRead reports who holds a project's purge fence, and whether they
// still appear to be working.
//
// Liveness is TTL/3 rather than the full TTL, and the C says why: the purge
// that owns the fence heartbeats at least every TTL/6 seconds, so twice the
// heartbeat interval is the bound at which silence means something. It is a
// tighter window than expiry deliberately -- this answer feeds a takeover
// decision, and taking over a fence whose owner is merely slow is worse than
// waiting.
//
// The empty project the C refuses is refused a step earlier here: the envelope
// declares a minimum length of one for the name, so a request carrying none
// never decodes. There is nothing left for the operation to check.
//
// A missing or unparseable heartbeat reads as not live. The fail-closed reading
// of "identity row with no heartbeat" belongs to the writer's own check, not
// here: a caller asking who holds the fence wants stale rather than an error.
func kbPurgeFenceRead(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeKBPurgeFenceReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var identity *string
	// A plain bool, not a pointer: the liveness expression is a comparison
	// rather than a column, and it cannot answer NULL. With no heartbeat row the
	// left half is false and the cast on the right answers NULL, and false AND
	// NULL is false -- so a missing heartbeat reads as not live rather than as
	// an absent value, which is the answer the C gives too.
	var live bool
	scanErr := store.QueryRow(ctx, kbPurgeFenceReadQuery,
		purgeFenceKeyPrefix+project, purgeFenceTSKeyPrefix+project,
		fenceLivenessWindowSeconds()).Scan(&identity, &live)
	if scanErr != nil {
		// No fence row is the common answer here, not a failure, and it is not
		// worth distinguishing from a read that went wrong: either way this
		// caller has learned that nothing is holding the project.
		reply, encodeErr := db2contract.EncodeKBPurgeFenceReadReply(0, "", "", 0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}

	// The identity row holds the generation and the purge identifier separated
	// by a space, and splitting on the first one is what the C does -- so a
	// purge identifier containing a space survives and a generation containing
	// one does not.
	generation, purgeID := splitFenceIdentity(text(identity))
	present := uint32(1)
	liveness := uint32(0)
	if live {
		liveness = 1
	}
	reply, encodeErr := db2contract.EncodeKBPurgeFenceReadReply(
		present, generation, purgeID, liveness)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// fenceLivenessWindowSeconds is a third of the TTL, floored at one second.
//
// The floor matters only for a TTL configured below three seconds, where the
// division would otherwise answer zero and make every fence read as dead the
// instant it was written. The C floors it for the same reason.
func fenceLivenessWindowSeconds() int {
	window := kbPurgeFenceTTLSeconds() / 3
	if window < 1 {
		return 1
	}
	return window
}

// splitFenceIdentity splits "<generation> <purge_id>" at the first space.
//
// A value with no space at all is all generation and no purge identifier, which
// is what the C's strchr-then-terminate produces. It is not a shape any writer
// here creates, but it is the shape a hand-edited row can have.
func splitFenceIdentity(value string) (string, string) {
	for index := 0; index < len(value); index++ {
		if value[index] == ' ' {
			return value[:index], value[index+1:]
		}
	}
	return value, ""
}

// Both halves of the generation pinning matter. A file indexed under a
// superseded generation, or one belonging to a project that has been detached,
// is not a file this answer should report as present -- the caller uses the
// hash to decide whether to re-ingest, and a stale hit means the content never
// gets re-read.
const kbFileIndexGetQuery = `SELECT k.file_hash, k.ingested_at FROM kb_file_index k
 JOIN projects p ON p.name = k.project
 WHERE k.project = $1 AND k.file_path = $2
 AND p.lifecycle_state = 'current'
 AND k.generation = p.current_generation`

// kbFileIndexGet answers what a project's copy of a file was last ingested as.
//
// The project name is passed through as given. The C routes it through a
// resolver first, but that resolver only copies the name into a buffer -- there
// is no lookup and no default, so the only thing it does is truncate a name
// past 256 bytes, which is not behaviour worth reproducing.
//
// The C also answers "not found" for an empty project without reading. The
// envelope refuses one before this is reached, so there is no branch here for
// it -- and an empty name would match no project row in any case.
func kbFileIndexGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err := db2contract.DecodeKBFileIndexGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var fileHash, ingestedAt *string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, kbFileIndexGetQuery, project, filePath).
		Scan(&fileHash, &ingestedAt); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, fileHash, ingestedAt = 0, nil, nil
	}
	reply, encodeErr := db2contract.EncodeKBFileIndexGetReply(
		found, text(fileHash), text(ingestedAt))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The counts land with the state change rather than after it, so a job never
// reads as done with nothing to show for it.
const kbIngestQueueCompleteQuery = `UPDATE kb_ingest_queue
 SET status = 'done', completed_at = pg_now_text(),
 files_indexed = $2, chunks_added = $3, embeddings_added = $4
 WHERE id = $1`

// kbIngestQueueComplete marks an ingest job finished and records what it did.
//
// Acknowledgement here means the statement ran, not that it matched a job. The
// C is the same, and the difference shows up when a job identifier is wrong or
// the job was already reaped: the caller is told the completion was accepted
// when nothing was updated. Reporting the changed-row count instead would be a
// different answer than the C gives, so it is left alone -- but a caller that
// needs to know the job existed cannot learn it from this reply.
func kbIngestQueueComplete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	jobID, filesIndexed, chunksAdded, embeddingsAdded, err :=
		db2contract.DecodeKBIngestQueueCompleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, kbIngestQueueCompleteQuery,
		int64(jobID), int64(filesIndexed), int64(chunksAdded), int64(embeddingsAdded))
	return acknowledgement(execErr == nil, db2contract.EncodeKBIngestQueueCompleteReply)
}

// Two statements, not three. The C has a third branch -- clear the flag but
// leave the reason -- reached only when its review_reason pointer is NULL, and
// nothing reaches this operation that way: the adapter decodes the reason into
// a stack buffer, so the pointer is never NULL and an absent reason arrives as
// the empty string. Through the module the branch is dead, and writing an empty
// reason alongside the cleared flag is what actually happens today.
const (
	kbDocSetStateQuery = `UPDATE docs SET state = $1, updated_at = pg_now_text()
 WHERE id = $2`
	kbDocSetStateClearingQuery = `UPDATE docs
 SET state = $1, review_needed = false, review_reason = $2,
 updated_at = pg_now_text()
 WHERE id = $3`
)

// kbDocSetState moves a document to a new state, optionally clearing the review
// flag that put it in front of a person.
//
// Clearing is opt-in rather than implied by the state change, because a
// document can move between states while still needing review -- and a state
// change that silently dismissed the review would lose the reason someone
// raised it.
//
// The stamp comes from pg_now_text() rather than being computed and bound. The
// C formats its own with now_utc(), which emits the identical spelling, so the
// only difference is whose clock it is: the database's rather than the calling
// host's. That is the better of the two, because every threshold these columns
// are compared against is evaluated by the database.
//
// As with the ingest completion, acknowledgement means the statement ran: a
// document identifier that matches nothing is acknowledged.
func kbDocSetState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	docID, docState, clearReviewNeeded, reviewReason, err :=
		db2contract.DecodeKBDocSetStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var execErr error
	if clearReviewNeeded != 0 {
		_, execErr = store.Exec(ctx, kbDocSetStateClearingQuery,
			docState, reviewReason, int64(docID))
	} else {
		_, execErr = store.Exec(ctx, kbDocSetStateQuery, docState, int64(docID))
	}
	return acknowledgement(execErr == nil, db2contract.EncodeKBDocSetStateReply)
}
