package db2

import (
	"context"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageRuntimeStateGet,
		db2contract.OperationRuntimeStateGet, runtimeStateGet)
	Register(db2contract.StageCountEmbeddingsForVersion,
		db2contract.OperationCountEmbeddingsForVersion, countEmbeddingsForVersion)
	Register(db2contract.StageVectorIndexOpRemove,
		db2contract.OperationVectorIndexOpRemove, vectorIndexOpRemove)
	Register(db2contract.StageResetStuckVectorOps,
		db2contract.OperationResetStuckVectorOps, resetStuckVectorOps)
	Register(db2contract.StageKBReleasePromote,
		db2contract.OperationKBReleasePromote, kbReleasePromote)
	Register(db2contract.StageKBReleaseRollback,
		db2contract.OperationKBReleaseRollback, kbReleaseRollback)
}

const runtimeStateGetQuery = `SELECT state_value FROM kb_runtime_state WHERE state_key = $1`

// runtimeStateGet reads one runtime-state value, or empty when unset.
func runtimeStateGet(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	stateKey, err := db2contract.DecodeRuntimeStateGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	value, status := readOptionalText(ctx, store, runtimeStateGetQuery, stateKey)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeRuntimeStateGetReply(value)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// countEmbeddingsForVersion counts memory units successfully indexed at one
// embedder version.
//
// It used to ignore the version entirely: the C validated it was non-empty and
// then ran a statement that never mentioned it, so every version got the same
// answer. That mattered because of who asks. The embedder rollback
// (kb_handle_memory_reembed_rollback) takes this count, refuses when it is
// zero, and otherwise makes the requested version active. The gate is meant to
// say "embeddings exist at this version"; it said "some memory unit is indexed
// at all". A rollback to a version nothing had ever been embedded at passed it
// and left vector search reading a version with no vectors.
//
// vector_index_ops.embedding_version now records which embedder produced each
// vector, stamped on the success path from memory_active_embedder, so the
// question the name asks is answerable and this asks it.
const countEmbeddingsForVersionQuery = `SELECT COUNT(*) FROM vector_index_ops
 WHERE collection = 'memory_units'
   AND status = 'ok'
   AND memory_id IS NOT NULL
   AND embedding_version = $1`

func countEmbeddingsForVersion(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	version, err := db2contract.DecodeCountEmbeddingsForVersionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count, status := readOptionalInt(ctx, store, countEmbeddingsForVersionQuery, version)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, encodeErr := db2contract.EncodeCountEmbeddingsForVersionReply(clampToU32(count))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const vectorIndexOpRemoveQuery = `DELETE FROM vector_index_ops WHERE point_id = $1`

// vectorIndexOpRemove forgets one queued vector-index operation.
//
// Its reply flag is bounded to exactly 1 by the contract, so the payload has no
// way to say "this did not work" -- which matches the C, where the function
// returns void and discards every error. A failed statement therefore has to
// surface as a module status rather than as a value, because the alternative is
// encoding a success that did not happen.
//
// A point that is already gone is not a failure: deleting nothing is the
// expected case for a caller cleaning up, and the row count is not consulted.
func vectorIndexOpRemove(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pointID, err := db2contract.DecodeVectorIndexOpRemoveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, vectorIndexOpRemoveQuery, int64(pointID)); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeVectorIndexOpRemoveReply(1)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Two queues, one operation: `memory repair --reset-stuck` is meant to retry
// orphaned code embeds as well as memory ones, and the reply is their sum. A
// port that reset only the vector ops would look correct and quietly leave the
// code-chunk queue stuck.
const (
	resetStuckVectorOpsQuery = `UPDATE vector_index_ops SET attempts = 0
 WHERE status = 'failed' AND attempts >= $1`
	resetStuckCodeOpsQuery = `UPDATE code_index_ops SET attempts = 0
 WHERE status = 'failed' AND attempts >= $1`
)

// resetStuckVectorOps clears the attempt count on failed index operations so
// they are retried, answering how many rows it freed.
//
// A zero threshold resets nothing, and cannot arrive here anyway: the contract
// floors max_attempts at 1, so the envelope is rejected before dispatch. The
// guard stays because it is the reason the floor exists -- `attempts >= 0`
// matches every failed row, including the ones that have never been tried -- and
// because Op is callable without going through the wire.
func resetStuckVectorOps(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	maxAttempts, err := db2contract.DecodeResetStuckVectorOpsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var total int64
	if maxAttempts > 0 {
		// Independently, not in one transaction. The two queues do not
		// reference each other, so a half-completed reset is not an
		// inconsistent state -- it is simply less unsticking than was asked
		// for. Wrapping them would make a failure on the code queue throw away
		// a vector reset that had already worked, which is the worse answer to
		// give an operator who ran this because things were stuck.
		for _, query := range []string{resetStuckVectorOpsQuery, resetStuckCodeOpsQuery} {
			rows, execErr := store.Exec(ctx, query, int64(maxAttempts))
			if execErr != nil {
				continue
			}
			total += rows
		}
	}
	reply, err := db2contract.EncodeResetStuckVectorOpsReply(clampToU32(total))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const (
	releaseExistsQuery = `SELECT id FROM doc_releases WHERE id = $1`
	releaseActiveQuery = `SELECT state_value FROM kb_runtime_state
 WHERE state_key = 'active_release_id'`
	releaseRetireQuery = `UPDATE doc_releases
 SET state = 'retired',
     retired_at = pg_now_text()
 WHERE id = $1`
	releasePromoteQuery = `UPDATE doc_releases
 SET state = 'active',
     promoted_at = pg_now_text()
 WHERE id = $1`
	releasePointerQuery = `INSERT INTO kb_runtime_state (state_key, state_value)
 VALUES ('active_release_id', $1)
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`
	lastRetiredReleaseQuery = `SELECT id FROM doc_releases WHERE state = 'retired'
 ORDER BY retired_at DESC LIMIT 1`
)

var errReleaseAbsent = errors.New("db2: no release holds that identifier")

// kbReleasePromote makes one documentation release the active one.
//
// The existence check comes first and is load-bearing. The retire and the
// promote are both UPDATEs that succeed when they match nothing, so without it
// a promote naming an identifier nothing holds would retire the active release,
// report success, and leave the installation with no active release at all --
// while the runtime pointer named one that was never there. The C carries that
// reasoning as a comment; it is repeated because the check looks redundant.
//
// The retire, the promote and the pointer are one transaction for the same
// reason: any of them without the rest is a state no reader can make sense of.
func kbReleasePromote(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	releaseID, err := db2contract.DecodeKBReleasePromoteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		return promoteRelease(ctx, tx, int64(releaseID))
	})
	return acknowledgement(txErr == nil, db2contract.EncodeKBReleasePromoteReply)
}

// kbReleaseRollback promotes a named release, or the most recently retired one.
//
// A zero target means "go back one": the last release to be retired is the one
// that was active before the current one, so promoting it undoes the last
// promotion. It answers unacknowledged when nothing has ever been retired,
// because there is no previous state to return to.
func kbReleaseRollback(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetID, err := db2contract.DecodeKBReleaseRollbackRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		target := int64(targetID)
		if target <= 0 {
			var found *int64
			if scanErr := tx.QueryRow(ctx, lastRetiredReleaseQuery).Scan(&found); scanErr != nil {
				if errors.Is(scanErr, pgx.ErrNoRows) {
					return errReleaseAbsent
				}
				return scanErr
			}
			target = number(found)
			if target <= 0 {
				return errReleaseAbsent
			}
		}
		return promoteRelease(ctx, tx, target)
	})
	return acknowledgement(txErr == nil, db2contract.EncodeKBReleaseRollbackReply)
}

// promoteRelease is the body both entry points share, and it assumes it is
// already inside a transaction.
func promoteRelease(ctx context.Context, tx Store, releaseID int64) error {
	var existing *int64
	if err := tx.QueryRow(ctx, releaseExistsQuery, releaseID).Scan(&existing); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return errReleaseAbsent
		}
		return err
	}

	var pointer *string
	if err := tx.QueryRow(ctx, releaseActiveQuery).Scan(&pointer); err != nil &&
		!errors.Is(err, pgx.ErrNoRows) {
		return err
	}
	// A pointer that is unset, empty or not a number means nothing is active,
	// which is an ordinary state for a fresh installation rather than an error.
	// The C reaches the same conclusion through atoll, which answers zero for
	// anything it cannot parse.
	if active, convErr := strconv.ParseInt(text(pointer), 10, 64); convErr == nil && active > 0 {
		if _, err := tx.Exec(ctx, releaseRetireQuery, active); err != nil {
			return err
		}
	}
	if _, err := tx.Exec(ctx, releasePromoteQuery, releaseID); err != nil {
		return err
	}
	_, err := tx.Exec(ctx, releasePointerQuery, strconv.FormatInt(releaseID, 10))
	return err
}
