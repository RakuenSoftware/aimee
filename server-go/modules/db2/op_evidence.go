package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageEvidenceStoreVector,
		db2contract.OperationEvidenceStoreVector, evidenceStoreVector)
	Register(db2contract.StageFeatureRowRead,
		db2contract.OperationFeatureRowRead, featureRowRead)
	Register(db2contract.StageMiningJobComplete,
		db2contract.OperationMiningJobComplete, miningJobComplete)
	Register(db2contract.StageKBDirectiveResolve,
		db2contract.OperationKBDirectiveResolve, kbDirectiveResolve)
	Register(db2contract.StageResolveContradiction,
		db2contract.OperationResolveContradiction, resolveContradiction)
	Register(db2contract.StageMemoryConflictingL2,
		db2contract.OperationMemoryConflictingL2, memoryConflictingL2)
}

// Delete then insert, because artifact_id is indexed but not unique: there is
// no conflict target to upsert against, so re-embedding on a model bump has to
// replace the old row explicitly. The queue row is then marked done.
const (
	evidenceDeleteVectorQuery = `DELETE FROM evidence_vectors WHERE artifact_id = $1`
	evidenceInsertVectorQuery = `INSERT INTO evidence_vectors
 (artifact_id, collection, embedding) VALUES ($1, $2, $3)`
	evidenceMarkIndexedQuery = `UPDATE evidence_index_ops SET status = 'ok',
 updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE artifact_id = $1`
)

// evidenceStoreVector replaces an artifact's embedding and marks it indexed.
//
// Three statements in one transaction, which the C does not do. The window that
// matters is the delete succeeding and the insert failing: the artifact then
// has no vector at all, and if its queue row already said 'ok' from an earlier
// run nothing retries it -- a silent hole in the evidence index rather than a
// visible failure.
//
// The queue row is marked in the same transaction for the same reason: an
// artifact reported indexed must have a vector behind it.
func evidenceStoreVector(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, collection, embedding, err :=
		db2contract.DecodeEvidenceStoreVectorRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// Both substitutions are the C's, and only the first one rescues anything.
	//
	// A vector row with no collection would not be found by the reads that
	// filter on one, so defaulting it matters.
	//
	// The empty-embedding default does not help: embedding is halfvec(384), and
	// "[]" has no dimensions, so the insert fails either way. It is kept because
	// failing is the right answer -- storing a zero vector would put a row in
	// the index that matches nothing and looks embedded -- and because the
	// failure is now reported rather than swallowed. A caller with no embedding
	// has nothing to store, and should be told so rather than quietly indexed.
	if collection == "" {
		collection = "evidence"
	}
	if embedding == "" {
		embedding = "[]"
	}

	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, evidenceDeleteVectorQuery, artifactID); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, evidenceInsertVectorQuery,
			artifactID, collection, embedding); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, evidenceMarkIndexedQuery, artifactID)
		return err
	})
	return acknowledgement(txErr == nil, db2contract.EncodeEvidenceStoreVectorReply)
}

const featureRowReadQuery = `SELECT features FROM feature_rows
 WHERE subject_id = $1 AND subject_kind = $2 AND feature_set_version = $3`

// featureRowRead reads a subject's computed features at one version.
//
// The version is part of the key rather than a filter over versions: features
// computed by an older extractor are a different row, not a stale one, so a
// caller asking for a version that has not been computed gets nothing rather
// than something computed differently.
func featureRowRead(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	subjectID, subjectKind, version, err := db2contract.DecodeFeatureRowReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	features, status := readOptionalText(ctx, store, featureRowReadQuery,
		subjectID, subjectKind, version)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeFeatureRowReadReply(features)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const miningJobCompleteQuery = `UPDATE mining_jobs
 SET hwm = CASE WHEN hwm > $2 THEN hwm ELSE $2 END,
     last_run_at = pg_now_text(),
     last_error = $3
 WHERE id = $1`

// miningJobComplete records where a mining pass got to.
//
// The high-water mark only moves forward. A pass that failed partway reports
// how far it actually reached, and that can be behind where a previous pass
// finished -- taking the lower number would make the next pass re-mine work
// already done, and doing that repeatedly is how a job stops making progress.
//
// The error is recorded whether or not the pass failed, so a successful pass
// clears the previous failure by writing an empty string. That is why the field
// is not optional: omitting it would leave a stale error attached to a job that
// has since succeeded.
func miningJobComplete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	jobID, highWaterMark, lastError, err :=
		db2contract.DecodeMiningJobCompleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, miningJobCompleteQuery,
		jobID, int64(highWaterMark), lastError)
	return acknowledgement(execErr == nil, db2contract.EncodeMiningJobCompleteReply)
}

// The open check is in the WHERE, where the C reads the state first and then
// updates without it. Same rule, one fewer race: between its check and its
// write another caller can resolve the directive, and the second write then
// overwrites which memory answered it. maintenance.directive_resolve already
// carries the predicate inline; this is the same operation reached from the
// kb service, and it now guards itself the same way.
const kbDirectiveResolveQuery = `UPDATE epistemic_directives
 SET state = 'resolved',
     resolution_memory_id = $2,
     resolved_at = pg_now_text(),
     updated_at = pg_now_text()
 WHERE id = $1 AND state = 'open'`

// kbDirectiveResolve closes an open directive by identifier.
//
// The resolution note the request carries goes nowhere. epistemic_directives
// has no column for it, and the C is explicit about dropping it -- the
// parameter is cast to void on the first line. A caller recording why a
// directive was resolved is writing into nothing, which is worth knowing before
// relying on it being kept.
func kbDirectiveResolve(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	directiveID, resolutionMemoryID, _, err :=
		db2contract.DecodeKBDirectiveResolveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, kbDirectiveResolveQuery,
		int64(directiveID), int64(resolutionMemoryID))
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeKBDirectiveResolveReply)
}

// The pair is symmetric in the source data, so both orderings are matched. A
// contradiction between A and B is the same contradiction as between B and A,
// and matching one direction would leave half of them unresolvable.
const resolveContradictionQuery = `UPDATE epistemic_directives
 SET state = 'resolved',
     resolution_memory_id = $3,
     resolved_at = pg_now_text(),
     updated_at = pg_now_text()
 WHERE state = 'open' AND cause = 'contradiction'
   AND ((memory_a_id = $1 AND memory_b_id = $2)
     OR (memory_a_id = $2 AND memory_b_id = $1))`

// resolveContradiction closes the open directive about two memories
// disagreeing.
//
// Restricted to open directives and to the contradiction cause, so resolving a
// pair cannot silently close some other question that happens to name the same
// two memories.
//
// The C returns how many rows changed; the reply carries a flag, so several
// directives about the same pair all resolve and the caller learns only that at
// least one did.
//
// This deliberately does not answer what the C answers. The C adapter tests
// that count for equality with ZERO and reports THAT as the acknowledgement, so
// it says a contradiction was resolved exactly when nothing was resolved, and
// says nothing happened whenever something did. The inversion is recorded as an
// accepted divergence in parity_test.go and against the C; copying it here
// would carry a live defect into the port, and a caller that believes a
// contradiction is closed stops re-raising it.
func resolveContradiction(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryA, memoryB, resolutionMemoryID, err :=
		db2contract.DecodeResolveContradictionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, resolveContradictionQuery,
		int64(memoryA), int64(memoryB), int64(resolutionMemoryID))
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeResolveContradictionReply)
}

const memoryConflictingL2Query = `SELECT confidence FROM memories
 WHERE key = $1 AND tier = 'L2' AND confidence >= 0.8
   AND content != $2 LIMIT 1`

// memoryConflictingL2 finds a confident memory under the same key saying
// something else.
//
// Three conditions make it a conflict rather than a duplicate: the same key,
// different content, and enough confidence to be worth arguing with. The 0.8
// floor is what stops every half-believed memory from raising a contradiction
// -- a memory nobody is sure of disagreeing with a new one is not evidence of
// anything.
//
// The confidence comes back so the caller can weigh the existing belief against
// what it is about to write, which is a different question from whether a
// conflict exists at all -- hence the flag beside it.
func memoryConflictingL2(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, content, err := db2contract.DecodeMemoryConflictingL2Request(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var confidence *float64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, memoryConflictingL2Query, key, content).
		Scan(&confidence); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		found = 0
	}
	reply, err := db2contract.EncodeMemoryConflictingL2Reply(found, decimal(confidence))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
