package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageVisibleSourceHash,
		db2contract.OperationVisibleSourceHash, visibleSourceHash)
	Register(db2contract.StageProjectionVisibleID,
		db2contract.OperationProjectionVisibleID, projectionVisibleID)
	Register(db2contract.StageEntityProfileCard,
		db2contract.OperationEntityProfileCard, entityProfileCard)
	Register(db2contract.StageCodeFileHash,
		db2contract.OperationCodeFileHash, codeFileHash)
	Register(db2contract.StageFileIndexDeleteCurrentGeneration,
		db2contract.OperationFileIndexDeleteCurrentGeneration, fileIndexDeleteCurrentGeneration)
	Register(db2contract.StageMinhashDeleteCurrentGeneration,
		db2contract.OperationMinhashDeleteCurrentGeneration, minhashDeleteCurrentGeneration)
}

// A projection generation is visible only while its project is current, which
// is why both reads join projects rather than trusting the generation's own
// state: a generation left visible on a project that has been retired is not
// visible, and reading it would resurrect a retired project's graph.
const (
	visibleSourceHashQuery = `SELECT g.source_hash FROM code_projection_generations g
 JOIN projects p ON p.name=g.project
 WHERE g.project=$1 AND g.state='visible' AND p.lifecycle_state='current'`
	projectionVisibleIDQuery = `SELECT g.id FROM code_projection_generations g
 JOIN projects p ON p.name=g.project
 WHERE g.project=$1 AND g.state='visible' AND p.lifecycle_state='current'`
)

// visibleSourceHash reads the source hash of a project's visible projection.
//
// Empty when the project has no visible generation, which is how a caller
// learns a projection has never been published rather than that the read
// failed.
func visibleSourceHash(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeVisibleSourceHashRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	hash, status := readOptionalText(ctx, store, visibleSourceHashQuery, project)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeVisibleSourceHashReply(hash)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// projectionVisibleID reads the identifier of a project's visible projection
// generation, or zero when there is none.
func projectionVisibleID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectionVisibleIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	generation, status := readOptionalInt(ctx, store, projectionVisibleIDQuery, project)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeProjectionVisibleIDReply(clampToU64(generation))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const entityProfileCardQuery = `SELECT card_json FROM entity_profiles
 WHERE LOWER(entity_id) = LOWER($1)`

// entityProfileCard reads an entity's profile card.
//
// The comparison is case-insensitive on both sides, so a caller need not know
// which spelling was stored. That also means two entities differing only in
// case are one entity to this read, and which card answers is unstated.
func entityProfileCard(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entityID, err := db2contract.DecodeEntityProfileCardRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	card, status := readOptionalText(ctx, store, entityProfileCardQuery, entityID)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeEntityProfileCardReply(card)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const codeFileHashQuery = `SELECT f.hash FROM files f
 JOIN projects p ON p.id = f.project_id
 WHERE p.name = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation AND f.path = $2 LIMIT 1`

// codeFileHash reads one file's content hash in a project's current generation.
//
// Bound to the current generation, so a file that existed in an older scan and
// not in this one answers empty -- the hash of what is indexed now, not the
// last hash anybody recorded.
func codeFileHash(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err := db2contract.DecodeCodeFileHashRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	hash, status := readOptionalText(ctx, store, codeFileHashQuery, project, filePath)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeCodeFileHashReply(hash)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Both deletes scope themselves with a subquery for the current generation
// rather than taking one as an argument. A caller cannot ask them to clear a
// generation that is not current, which is the point: these run before a
// rescan repopulates, and clearing a published generation would empty the index
// somebody is reading.
const (
	fileIndexDeleteCurrentGenerationQuery = `DELETE FROM kb_file_index WHERE project=$1
 AND generation=(SELECT current_generation FROM projects
 WHERE name=$1 AND lifecycle_state='current')`
	minhashDeleteSignaturesQuery = `DELETE FROM kb_minhash_signatures WHERE project=$1
 AND generation=(SELECT current_generation FROM projects
 WHERE name=$1 AND lifecycle_state='current')`
	minhashDeleteBucketsQuery = `DELETE FROM kb_lsh_buckets WHERE project=$1
 AND generation=(SELECT current_generation FROM projects
 WHERE name=$1 AND lifecycle_state='current')`
)

// fileIndexDeleteCurrentGeneration clears a project's file index for the
// generation it is currently publishing.
func fileIndexDeleteCurrentGeneration(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeFileIndexDeleteCurrentGenerationRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, fileIndexDeleteCurrentGenerationQuery, project); execErr != nil {
		reply, encodeErr := db2contract.EncodeFileIndexDeleteCurrentGenerationReply(0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	reply, err := db2contract.EncodeFileIndexDeleteCurrentGenerationReply(1)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// minhashDeleteCurrentGeneration clears a project's minhash signatures and the
// LSH buckets built over them.
//
// Two statements in one transaction, which the C implementation does not do:
// there, a failure after the first leaves signatures gone and buckets pointing
// at them. Buckets that name signatures which no longer exist are not a
// half-cleared index, they are a wrong one, and the next similarity read would
// use them.
func minhashDeleteCurrentGeneration(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeMinhashDeleteCurrentGenerationRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, minhashDeleteSignaturesQuery, project); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, minhashDeleteBucketsQuery, project)
		return err
	})
	acknowledged := uint32(1)
	if txErr != nil {
		acknowledged = 0
	}
	reply, err := db2contract.EncodeMinhashDeleteCurrentGenerationReply(acknowledged)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// readOptionalText reads one text column, answering empty when no row matched.
//
// Absence is an answer for every read that uses this, so pgx.ErrNoRows is not a
// failure; anything else is.
func readOptionalText(ctx context.Context, store Store, query string, args ...any) (
	string, bus.ModuleStatus,
) {
	var value *string
	if err := store.QueryRow(ctx, query, args...).Scan(&value); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return "", bus.ModuleStatusOK
		}
		return "", bus.ModuleStatusInternal
	}
	return text(value), bus.ModuleStatusOK
}

// readOptionalInt is readOptionalText for an integer column, answering zero.
func readOptionalInt(ctx context.Context, store Store, query string, args ...any) (
	int64, bus.ModuleStatus,
) {
	var value *int64
	if err := store.QueryRow(ctx, query, args...).Scan(&value); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return 0, bus.ModuleStatusOK
		}
		return 0, bus.ModuleStatusInternal
	}
	return number(value), bus.ModuleStatusOK
}
