package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageRelTypesEnsureSeed,
		db2contract.OperationRelTypesEnsureSeed, relTypesEnsureSeed)
	Register(db2contract.StageDocDelete,
		db2contract.OperationDocDelete, docDelete)
	Register(db2contract.StageTaskDelete,
		db2contract.OperationTaskDelete, taskDelete)
	Register(db2contract.StageClearProject,
		db2contract.OperationClearProject, clearProject)
	Register(db2contract.StageClearCurrentProject,
		db2contract.OperationClearCurrentProject, clearCurrentProject)
	Register(db2contract.StageDocumentExists,
		db2contract.OperationDocumentExists, documentExists)
	Register(db2contract.StageBlobReferenced,
		db2contract.OperationBlobReferenced, blobReferenced)
	Register(db2contract.StageFenceActive,
		db2contract.OperationFenceActive, fenceActive)
	Register(db2contract.StageDocExistsByHash,
		db2contract.OperationDocExistsByHash, docExistsByHash)
	Register(db2contract.StagePdfQuarantineConfirm,
		db2contract.OperationPdfQuarantineConfirm, pdfQuarantineConfirm)
	Register(db2contract.StagePdfQuarantineReject,
		db2contract.OperationPdfQuarantineReject, pdfQuarantineReject)
	Register(db2contract.StageVectorRebuildLockTryAcquire,
		db2contract.OperationVectorRebuildLockTryAcquire,
		vectorRebuildLockTryAcquire)
	Register(db2contract.StageVectorRebuildLockRelease,
		db2contract.OperationVectorRebuildLockRelease, vectorRebuildLockRelease)
	Register(db2contract.StageReleaseGetActive,
		db2contract.OperationReleaseGetActive, releaseGetActive)
	Register(db2contract.StageEnrollmentActive,
		db2contract.OperationEnrollmentActive, enrollmentActive)
}

const (
	docDeleteQuery = `DELETE FROM docs WHERE id = $1`

	// Edges first, then the task. The edge table names tasks on both sides, so
	// deleting the task first would leave edges pointing at nothing -- and the
	// C deletes both for that reason. One statement keeps them together.
	taskDeleteQuery = `WITH edges AS (
   DELETE FROM task_edges WHERE source_id = $1 OR target_id = $1 RETURNING 1
 )
 DELETE FROM tasks WHERE id = $1`

	documentExistsQuery = `SELECT EXISTS (
 SELECT 1 FROM kb_documents WHERE id = $1)`

	blobReferencedQuery = `SELECT EXISTS (
 SELECT 1 FROM kb_doc_assets WHERE blob_ref = $1)`

	docExistsByHashQuery = `SELECT EXISTS (
 SELECT 1 FROM docs WHERE content_hash = $1 AND scope = $2)`

	// The index operations go with the documents they point at. Doing it in one
	// statement is what stops a half-cleared project: the C runs two, and a
	// failure between them leaves index operations referring to documents that
	// no longer exist.
	clearProjectQuery = `WITH ops AS (
   DELETE FROM vector_index_ops
    WHERE point_id IN (SELECT id FROM kb_documents WHERE project = $1)
   RETURNING 1
 ), cleared AS (
   DELETE FROM kb_documents WHERE project = $1 RETURNING 1
 )
 SELECT COUNT(*) FROM cleared`

	clearCurrentProjectQuery = `WITH current AS (
   SELECT d.id FROM kb_documents d
     JOIN projects p ON p.name = d.project
    WHERE d.project = $1 AND p.lifecycle_state = 'current'
      AND d.generation = p.current_generation
 ), ops AS (
   DELETE FROM vector_index_ops
    WHERE point_id IN (SELECT id FROM current) RETURNING 1
 ), cleared AS (
   DELETE FROM kb_documents WHERE id IN (SELECT id FROM current) RETURNING 1
 )
 SELECT COUNT(*) FROM cleared`

	// Confirming a quarantined PDF clears the flag and answers which chunks it
	// freed, so the caller can queue them for embedding.
	pdfQuarantineConfirmQuery = `UPDATE kb_documents SET quarantine_state = ''
 WHERE project = $1 AND file_path = $2 AND doc_kind = 'pdf'
   AND quarantine_state = 'pending'
   AND generation = (SELECT current_generation FROM projects
     WHERE name = $1 AND lifecycle_state = 'current')
 RETURNING id`

	pdfQuarantineRejectQuery = `DELETE FROM kb_documents
 WHERE project = $1 AND file_path = $2 AND doc_kind = 'pdf'
   AND quarantine_state = 'pending'
   AND generation = (SELECT current_generation FROM projects
     WHERE name = $1 AND lifecycle_state = 'current')
 RETURNING id`

	vectorRebuildLockKey = "vector_rebuild_lock"

	vectorRebuildLockAcquireQuery = `INSERT INTO kb_runtime_state
 (state_key, state_value) VALUES ($1, pg_now_text())
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`

	vectorRebuildLockReleaseQuery = `DELETE FROM kb_runtime_state
 WHERE state_key = $1`

	releaseGetActiveQuery = `SELECT state_value FROM kb_runtime_state
 WHERE state_key = 'active_release_id'`

	enrollmentActiveQuery = `SELECT state, revoked_at FROM kb_enrollments
 WHERE cert_issuer = $1 AND cert_serial_norm = $2`
)

// The relation ontology the graph refuses to store an edge outside of.
//
// Seeded rather than assumed: a relation with no row here cannot be written, so
// an empty table is a graph that accepts nothing. DO NOTHING on conflict, so a
// deployment that has edited a row keeps its edit.
//
// The kinds are stored as text lists exactly as the C writes them, because the
// reader splits on the same separator.
const relTypesEnsureSeedQuery = `INSERT INTO rel_types
 (rel_type, head_kinds, tail_kinds, is_symmetric, inverse_rel_type,
  correction_behavior, category, sensitivity, is_hierarchy_rel, status)
 SELECT seed.rel_type, seed.head_kinds, seed.tail_kinds, seed.is_symmetric,
        seed.inverse_rel_type, seed.correction_behavior, seed.category,
        seed.sensitivity, seed.is_hierarchy_rel, 'active'
   FROM unnest($1::text[], $2::text[], $3::text[], $4::int[], $5::text[],
               $6::text[], $7::text[], $8::text[], $9::int[])
     AS seed(rel_type, head_kinds, tail_kinds, is_symmetric, inverse_rel_type,
             correction_behavior, category, sensitivity, is_hierarchy_rel)
 ON CONFLICT (rel_type) DO NOTHING`

// relTypesEnsureSeed writes the seeded relation ontology, once.
//
// One statement for the whole table where the C prepares and runs one insert
// per relation. The seed itself is generated from the C's own table rather than
// transcribed -- scripts/gen_db2_rel_type_seed.py reads src/rel_types.c and the
// test beside it fails if the two drift, because a Go module seeding a
// different ontology would accept a different graph.
func relTypesEnsureSeed(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeRelTypesEnsureSeedRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := len(relTypeSeedOntology)
	relTypes := make([]string, count)
	headKinds := make([]string, count)
	tailKinds := make([]string, count)
	symmetric := make([]int32, count)
	inverses := make([]string, count)
	corrections := make([]string, count)
	categories := make([]string, count)
	sensitivities := make([]string, count)
	hierarchies := make([]int32, count)
	for index, seed := range relTypeSeedOntology {
		relTypes[index] = seed.RelType
		headKinds[index] = seed.HeadKinds
		tailKinds[index] = seed.TailKinds
		symmetric[index] = int32(seed.IsSymmetric)
		inverses[index] = seed.InverseRelType
		corrections[index] = seed.CorrectionBehavior
		categories[index] = seed.Category
		sensitivities[index] = seed.Sensitivity
		hierarchies[index] = int32(seed.IsHierarchyRel)
	}
	if _, execErr := store.Exec(ctx, relTypesEnsureSeedQuery, relTypes,
		headKinds, tailKinds, symmetric, inverses, corrections, categories,
		sensitivities, hierarchies); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(infallible(db2contract.EncodeRelTypesEnsureSeedReply))
}

func docDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	docID, err := db2contract.DecodeDocDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, docDeleteQuery,
		infallible(db2contract.EncodeDocDeleteReply), int64(docID))
}

func taskDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	taskID, err := db2contract.DecodeTaskDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, taskDeleteQuery,
		infallible(db2contract.EncodeTaskDeleteReply), int64(taskID))
}

func clearProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeClearProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, clearProjectQuery,
		db2contract.EncodeClearProjectReply, project)
}

func clearCurrentProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeClearCurrentProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, clearCurrentProjectQuery,
		db2contract.EncodeClearCurrentProjectReply, project)
}

func documentExists(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	documentID, err := db2contract.DecodeDocumentExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, documentExistsQuery,
		db2contract.EncodeDocumentExistsReply, int64(documentID))
}

// blobReferenced answers whether any asset still points at a blob, which is
// what decides whether its bytes can be reclaimed.
func blobReferenced(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	blobRef, err := db2contract.DecodeBlobReferencedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, blobReferencedQuery,
		db2contract.EncodeBlobReferencedReply, blobRef)
}

func docExistsByHash(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	contentHash, scope, err := db2contract.DecodeDocExistsByHashRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, docExistsByHashQuery,
		db2contract.EncodeDocExistsByHashReply, contentHash, scope)
}

// The fence read, in one statement where the C makes two or three.
//
// Three states and only two of them mean "not fenced": no identity row at all
// is inactive; an identity row with no heartbeat is ACTIVE, deliberately, so a
// fence that was written by hand and half-removed still blocks; and an identity
// row with a heartbeat is active only while the heartbeat is inside the TTL.
//
// The fail-closed middle case is why this cannot be a single EXISTS: it has to
// tell "no heartbeat row" from "old heartbeat row", and those differ only in
// whether the second read found anything.
const fenceActiveQuery = `SELECT
 EXISTS (SELECT 1 FROM kb_runtime_state WHERE state_key = $1),
 EXISTS (SELECT 1 FROM kb_runtime_state WHERE state_key = $2),
 EXISTS (SELECT 1 FROM kb_runtime_state
   WHERE state_key = $2
     AND state_value > pg_now_text($3))`

func fenceActive(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeFenceActiveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var hasIdentity, hasHeartbeat, heartbeatFresh bool
	if scanErr := store.QueryRow(ctx, fenceActiveQuery,
		purgeFenceKeyPrefix+project, purgeFenceTSKeyPrefix+project,
		fenceWindow(kbPurgeFenceTTLSeconds())).
		Scan(&hasIdentity, &hasHeartbeat, &heartbeatFresh); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	active := uint32(0)
	switch {
	case !hasIdentity:
	case !hasHeartbeat:
		// Fail closed: a partially written fence is never inactive.
		active = 1
	case heartbeatFresh:
		active = 1
	}
	reply, encodeErr := db2contract.EncodeFenceActiveReply(active)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// pdfQuarantineConfirm clears the quarantine flag and answers how many chunks
// it released.
//
// The C enqueues an embed job per released chunk. That is not done here: the
// enqueue is a second operation the catalogue describes separately, and doing
// it inside this one would make a caller that wanted only the release get a
// queue full of work it did not ask for. Whoever drives confirmation has to
// call the enqueue, and this reply's count is what tells it how many to expect.
func pdfQuarantineConfirm(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, documentKey, err :=
		db2contract.DecodePdfQuarantineConfirmRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	confirmed, execErr := store.Exec(ctx, pdfQuarantineConfirmQuery, project,
		documentKey)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(confirmed, db2contract.EncodePdfQuarantineConfirmReply)
}

func pdfQuarantineReject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, documentKey, err :=
		db2contract.DecodePdfQuarantineRejectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	rejected, execErr := store.Exec(ctx, pdfQuarantineRejectQuery, project,
		documentKey)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(rejected, db2contract.EncodePdfQuarantineRejectReply)
}

// vectorRebuildLockTryAcquire takes the rebuild lock, and always gets it.
//
// The C's "try" is not a try: it writes the key and upserts over whatever was
// there, so a second rebuild starting while the first runs takes the lock from
// it and both proceed. Reproduced as it is, because making it a real lock would
// change which of two concurrent rebuilds runs -- a behaviour change that
// belongs with whoever owns the rebuild, not in a port of the write.
func vectorRebuildLockTryAcquire(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeVectorRebuildLockTryAcquireRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	acquired := uint32(1)
	if _, execErr := store.Exec(ctx, vectorRebuildLockAcquireQuery,
		vectorRebuildLockKey); execErr != nil {
		acquired = 0
	}
	reply, encodeErr :=
		db2contract.EncodeVectorRebuildLockTryAcquireReply(acquired)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func vectorRebuildLockRelease(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeVectorRebuildLockReleaseRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return voidWrite(ctx, store, vectorRebuildLockReleaseQuery,
		"vector_rebuild_lock_release",
		infallible(db2contract.EncodeVectorRebuildLockReleaseReply),
		vectorRebuildLockKey)
}

// releaseGetActive answers which release is live, as an identifier.
//
// The value is stored as text in the runtime-state table, so a row holding
// something that is not a number answers zero -- which is the same answer as no
// row at all, and is what the C's parse does with it.
func releaseGetActive(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeReleaseGetActiveRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var value string
	switch err := store.QueryRow(ctx, releaseGetActiveQuery).Scan(&value); {
	case errors.Is(err, pgx.ErrNoRows):
		value = ""
	case err != nil:
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeReleaseGetActiveReply(
		parseReleaseID(value))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// parseReleaseID reads the leading digits, stopping at the first byte that is
// not one -- which is what the C's strtoll does with the stored text.
func parseReleaseID(value string) uint64 {
	var parsed uint64
	for index := 0; index < len(value); index++ {
		if value[index] < '0' || value[index] > '9' {
			break
		}
		parsed = parsed*10 + uint64(value[index]-'0')
	}
	return parsed
}

// enrollmentActive answers whether a certificate is currently enrolled.
//
// Three outcomes, not two: active with no revocation is active; revoked or
// revocation-stamped is not; and a state the schema does not know is refused
// rather than guessed, because an enrolment in an unrecognised state is not
// something to admit on.
func enrollmentActive(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	certIssuer, certSerial, err :=
		db2contract.DecodeEnrollmentActiveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var state, revokedAt string
	switch scanErr := store.QueryRow(ctx, enrollmentActiveQuery, certIssuer,
		certSerial).Scan(&state, &revokedAt); {
	case errors.Is(scanErr, pgx.ErrNoRows):
		reply, encodeErr := db2contract.EncodeEnrollmentActiveReply(0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	case scanErr != nil:
		return nil, bus.ModuleStatusInternal
	}
	if state != "active" && state != "revoked" {
		// The C treats an unknown state as a fault rather than as a "no": the
		// row exists and says something nobody can act on.
		return nil, bus.ModuleStatusInternal
	}
	active := uint32(0)
	if state == "active" && revokedAt == "" {
		active = 1
	}
	reply, encodeErr := db2contract.EncodeEnrollmentActiveReply(active)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// fenceWindow spells a second count the way pg_now_text takes it.
//
// Built here rather than concatenated in the statement, for the reason the
// decay windows are: concatenation makes Postgres infer the parameter as text,
// and an integer bound to a text parameter has no encoding.
func fenceWindow(seconds int) string {
	return "-" + itoa(uint32(seconds)) + " seconds"
}
