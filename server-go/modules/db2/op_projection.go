package db2

import (
	"context"
	"errors"

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

// projectDelete removes a project row.
//
// What that takes with it is the schema's business: the tables that reference
// projects carry their own cascade rules, and this statement names only the one
// row. A project that is not there deletes nothing and still succeeds.
func projectDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, projectDeleteQuery, project)
	return acknowledgement(execErr == nil, db2contract.EncodeProjectDeleteReply)
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
