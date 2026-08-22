package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCuratorInvalidationsSince,
		db2contract.OperationCuratorInvalidationsSince, curatorInvalidationsSince)
}

// The SQL is the C implementation's, verbatim except for the placeholder
// dialect: pgx binds $1 where the C layer's wrapper accepted ?1. Keeping the
// statement identical is what makes a C-vs-Go parity comparison mean something
// -- a Go implementation that reads the same rows a different way proves only
// that two queries happen to agree today.
const curatorInvalidationsSinceQuery = `SELECT id, source_kind, source_id, artifacts_stale, created_at
 FROM curator_invalidation_events WHERE id > $1 ORDER BY id ASC LIMIT $2`

// curatorInvalidationsSince reads what has invalidated curated artifacts since
// a caller last looked.
//
// A cursor read: strictly greater than the identifier given, oldest first, so a
// caller stores the last identifier it saw and never sees a row twice.
func curatorInvalidationsSince(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sinceID, err := db2contract.DecodeCuratorInvalidationsSinceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	rows, err := store.Query(ctx, curatorInvalidationsSinceQuery,
		int64(sinceID), db2contract.CuratorInvalidationsSinceMaxRows)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	// Capacity, not length: the ceiling is what the reply can hold, and
	// allocating it up front costs one allocation whatever the answer is.
	found := make([]db2contract.CuratorInvalidationsSinceRow, 0,
		db2contract.CuratorInvalidationsSinceMaxRows)
	for rows.Next() {
		var (
			id             int64
			sourceKind     string
			sourceID       string
			artifactsStale int32
			createdAt      string
		)
		if err := rows.Scan(&id, &sourceKind, &sourceID, &artifactsStale, &createdAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.CuratorInvalidationsSinceRow{
			InvalidationID: clampToU64(id),
			SourceKind:     sourceKind,
			SourceID:       sourceID,
			ArtifactsStale: clampToU32(int64(artifactsStale)),
			InvalidatedAt:  createdAt,
		})
	}
	// Checked after the loop, not only inside it: rows.Next reports false both
	// for "no more rows" and for a failure mid-stream, and without this a
	// truncated read would encode as a short page and look like an answer.
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeCuratorInvalidationsSinceReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// clampToU64 carries a database integer onto the wire's unsigned field.
//
// The column is a bigint and the wire field is unsigned, so a negative -- which
// an identity column will not produce and a corrupted row might -- becomes
// zero rather than a value near 2^64 that a caller would read as an enormous
// identifier.
func clampToU64(value int64) uint64 {
	if value <= 0 {
		return 0
	}
	return uint64(value)
}

func clampToU32(value int64) uint32 {
	if value <= 0 {
		return 0
	}
	if value > 0xffffffff {
		return 0xffffffff
	}
	return uint32(value)
}
