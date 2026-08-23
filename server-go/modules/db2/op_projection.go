package db2

import (
	"context"
	"errors"
	"log"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageProjectionGenerationCreate,
		db2contract.OperationProjectionGenerationCreate, projectionGenerationCreate)
	Register(db2contract.StageGenerationPublish,
		db2contract.OperationGenerationPublish, generationPublish)
	Register(db2contract.StageGenerationAbort,
		db2contract.OperationGenerationAbort, generationAbort)
	Register(db2contract.StageGenerationSetSourceHash,
		db2contract.OperationGenerationSetSourceHash, generationSetSourceHash)
	Register(db2contract.StageProjectDelete,
		db2contract.OperationProjectDelete, projectDelete)
}

// BuildVersion is stamped into every projection generation, so the
// snapshot-diff route can refuse to compare graphs produced by different
// extractors.
//
// The C module gets this from -DAIMEE_VERSION at compile time. The Go module's
// build does not set it yet, and the default is deliberately not a plausible
// version string: a generation stamped "unstamped-build" is one nobody will
// mistake for a real one, where "0.0.0" or "dev" would sit in the column
// looking like an answer. Wiring it is part of the cutover, not of a port.
var BuildVersion = "unstamped-build"

const projectionGenerationCreateQuery = `INSERT INTO code_projection_generations
 (project, state, started_at, extractor_version, pipeline_version)
 SELECT p.name, 'pending', to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'), $2, $2
 FROM projects p WHERE p.name=$1 AND p.lifecycle_state='current'
 RETURNING id`

// projectionGenerationCreate opens a pending projection generation.
//
// INSERT ... SELECT rather than INSERT ... VALUES, so a project that is not
// current inserts nothing at all: the generation cannot exist for a project
// that cannot receive it, and the caller is told zero rather than being handed
// an identifier that will never publish.
func projectionGenerationCreate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectionGenerationCreateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id int64
	if scanErr := store.QueryRow(ctx, projectionGenerationCreateQuery, project, BuildVersion).
		Scan(&id); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		id = 0
	}
	reply, err := db2contract.EncodeProjectionGenerationCreateReply(clampToU64(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const (
	generationSupersedeQuery = `UPDATE code_projection_generations
 SET state = 'superseded' WHERE project = $1 AND state = 'visible'`
	generationPublishQuery = `UPDATE code_projection_generations
 SET state = 'visible',
     visible_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE id=$1 AND project=$2 AND state='pending'
 AND EXISTS (SELECT 1 FROM projects p
 WHERE p.name=code_projection_generations.project
 AND p.lifecycle_state='current')`
	generationStampEdgesQuery = `UPDATE entity_edges SET projection_generation_id=$1
 WHERE edge_origin='code_projection' AND EXISTS (
 SELECT 1 FROM code_projection_edges cpe WHERE cpe.generation_id=$1
 AND cpe.source=entity_edges.source AND cpe.relation=entity_edges.relation
 AND cpe.target=entity_edges.target)`
)

// generationPublish makes a pending generation the visible one.
//
// Three statements in one transaction, and the middle one is why: it supersedes
// the project's current visible generation BEFORE flipping this one, so a stale
// or mismatched generation must not remove the project's last visible graph.
// The publish is checked for exactly one changed row -- it can match nothing
// because the generation is not pending, belongs to another project, or the
// project is no longer current, and in every one of those the supersede has
// already run and must be undone.
func generationPublish(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	generationID, project, err := db2contract.DecodeGenerationPublishRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, generationSupersedeQuery, project); err != nil {
			return err
		}
		published, err := tx.Exec(ctx, generationPublishQuery, int64(generationID), project)
		if err != nil {
			return err
		}
		if published != 1 {
			return errGenerationNotPublishable
		}
		// The C statement binds the generation twice because its wrapper numbers
		// placeholders positionally; $1 serves both halves here.
		_, err = tx.Exec(ctx, generationStampEdgesQuery, int64(generationID))
		return err
	})
	return acknowledgement(txErr == nil,
		db2contract.EncodeGenerationPublishReply)
}

var errGenerationNotPublishable = errors.New(
	"db2: the generation is not pending, not this project's, or the project is not current")

const generationAbortQuery = `UPDATE code_projection_generations
 SET state = 'aborted',
     aborted_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'),
     error = $2
 WHERE id = $1 AND state = 'pending'`

// generationAbort marks a pending generation aborted, with the reason.
//
// Only a pending one: an aborted generation cannot be re-aborted and a visible
// one cannot be aborted at all, which is what stops a late failure from
// retracting a graph that has already been published.
func generationAbort(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	generationID, message, err := db2contract.DecodeGenerationAbortRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, generationAbortQuery, int64(generationID), message)
	return acknowledgement(execErr == nil, db2contract.EncodeGenerationAbortReply)
}

const generationSetSourceHashQuery = `UPDATE code_projection_generations
 SET source_hash = $2 WHERE id = $1`

// generationSetSourceHash records the source fingerprint a generation was built
// from.
//
// Unlike its neighbours this is not restricted by state, so it can be set on a
// generation that has already published. That is the C behaviour.
func generationSetSourceHash(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	generationID, sourceHash, err := db2contract.DecodeGenerationSetSourceHashRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, generationSetSourceHashQuery, int64(generationID), sourceHash)
	return acknowledgement(execErr == nil, db2contract.EncodeGenerationSetSourceHashReply)
}

const projectDeleteQuery = `DELETE FROM projects WHERE name = $1`

// projectDelete removes a project row and answers how many rows that was.
//
// The reply field is named "deleted" and bounded by the whole of u32, not by
// one: it is a count, and this answered a flag until the parity run compared it
// with the C -- which returns the removed-row count. A caller asking how much a
// delete removed cannot tell "one" from "a thousand" if the answer is always
// one, and cannot tell "none" from "some" if it is always success.
//
// What the delete takes with it is the schema's business: the tables that
// reference projects carry their own cascade rules, and this statement names
// only the one row. A project that is not there deletes nothing and still
// succeeds, which is now reported as the zero it is.
func projectDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, projectDeleteQuery, project)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(deleted, db2contract.EncodeProjectDeleteReply)
}

// removedCount answers a reply whose single field is how many rows a statement
// removed.
//
// A negative count cannot happen through this Store -- pgx answers a command
// tag -- but the C floors one anyway, and the floor costs nothing to keep: an
// unsigned field given a negative number would encode as an enormous one.
func removedCount(rows int64, encode func(uint32) ([]byte, error)) (
	[]byte, bus.ModuleStatus,
) {
	if rows < 0 {
		rows = 0
	}
	reply, err := encode(uint32(rows))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// dispatchAcknowledgement answers the one-flag reply of an operation whose C
// backend returns void.
//
// For these the flag cannot mean what it looks like it means. The adapter sets
// it from nothing but the fact that it made the call -- "the backend returns
// void, so this says the call was made", in its own words -- so a caller reading
// a one here has learned that the request was dispatched and nothing about
// whether a row was written. The parity run proved it: seven of these answered
// one for inserts that violated a foreign key and wrote nothing.
//
// Answering zero on a failed write would be more useful and is what this module
// did until the parity run compared it: it is a different answer to the same
// request, and a caller that switched implementations would see its meaning
// change under it. So the C's answer is reproduced, and the error it has nowhere
// to put is logged rather than dropped -- which is the most this side can do
// without changing the contract.
//
// Making the flag mean "written" is a contract change: the reply field's
// meaning is declared by the catalogue and implemented by both sides, and it is
// worth making, at the contract rather than in one implementation of it.
func dispatchAcknowledgement(execErr error, operation string,
	encode func(uint32) ([]byte, error)) ([]byte, bus.ModuleStatus) {
	if execErr != nil {
		logDroppedWrite(operation, execErr)
	}
	return acknowledgement(true, encode)
}

// logDroppedWrite is where a write failure goes when the reply cannot carry it.
//
// Several operations answer a reply the C fills without consulting the write:
// an acknowledgement it sets unconditionally, or no fields at all. Reproducing
// that answer is right -- the field's meaning is the contract's, not this
// implementation's -- but losing the error entirely is not, so it lands here.
func logDroppedWrite(operation string, cause error) {
	log.Printf("db2: %s wrote nothing and the reply cannot say so: %v",
		operation, cause)
}

// acknowledgement answers a one-flag reply.
//
// The flag says the statement ran, never that a row moved -- an UPDATE whose
// WHERE matched nothing succeeds, and several of these are written so that it
// can. Where a changed-row count is the outcome, as in generationPublish, that
// check belongs inside the transaction rather than in this flag.
func acknowledgement(ok bool, encode func(uint32) ([]byte, error)) ([]byte, bus.ModuleStatus) {
	value := uint32(0)
	if ok {
		value = 1
	}
	reply, err := encode(value)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
