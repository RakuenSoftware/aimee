package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageLevel3Count,
		db2contract.OperationLevel3Count, level3Count)
	Register(db2contract.StageLevel2Count,
		db2contract.OperationLevel2Count, level2Count)
	Register(db2contract.StageOrphanedL0Count,
		db2contract.OperationOrphanedL0Count, orphanedL0Count)
	Register(db2contract.StageTotalCount,
		db2contract.OperationTotalCount, totalCount)
	Register(db2contract.StageSessionL2Count,
		db2contract.OperationSessionL2Count, sessionL2Count)
	Register(db2contract.StageKeyExists,
		db2contract.OperationKeyExists, keyExists)
	Register(db2contract.StageFindIDByKeyKind,
		db2contract.OperationFindIDByKeyKind, findIDByKeyKind)
	Register(db2contract.StageKeyExistsInTierPair,
		db2contract.OperationKeyExistsInTierPair, keyExistsInTierPair)
}

// The tier counts, and the one that is not a tier count.
//
// An orphaned L0 is not "an L0 that is old": it is an L0 old enough that
// nothing was ever going to promote it. The seven days is the C's and the
// window is what makes the count mean anything -- without it every L0 written
// in the last minute would read as orphaned.
const (
	level3CountQuery = `SELECT COUNT(*) FROM memories WHERE tier = 'L3'`
	level2CountQuery = `SELECT COUNT(*) FROM memories WHERE tier = 'L2'`

	orphanedL0CountQuery = `SELECT COUNT(*) FROM memories
 WHERE tier = 'L0' AND created_at < pg_now_text('-7 days')`

	totalCountQuery = `SELECT COUNT(*) FROM memories`

	sessionL2CountQuery = `SELECT COUNT(*) FROM memories
 WHERE tier = 'L2' AND source_session = $1`

	keyExistsQuery = `SELECT EXISTS (SELECT 1 FROM memories WHERE key = $1)`

	findIDByKeyKindQuery = `SELECT id FROM memories
 WHERE key = $1 AND kind = $2 LIMIT 1`

	keyExistsInTierPairQuery = `SELECT EXISTS (SELECT 1 FROM memories
 WHERE key = $1 AND tier IN ($2, $3))`
)

func level3Count(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeLevel3CountRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, level3CountQuery,
		db2contract.EncodeLevel3CountReply)
}

func level2Count(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeLevel2CountRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, level2CountQuery,
		db2contract.EncodeLevel2CountReply)
}

func orphanedL0Count(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeOrphanedL0CountRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, orphanedL0CountQuery,
		db2contract.EncodeOrphanedL0CountReply)
}

// totalCount answers how many memories there are, in the one field wide enough
// to hold it -- the tier counts are u32 and this is u64, which is the
// catalogue's judgement about which of them could overflow.
func totalCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeTotalCountRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var total int64
	if err := store.QueryRow(ctx, totalCountQuery).Scan(&total); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if total < 0 {
		total = 0
	}
	reply, encodeErr := db2contract.EncodeTotalCountReply(uint64(total))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func sessionL2Count(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sourceSession, err := db2contract.DecodeSessionL2CountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, sessionL2CountQuery,
		db2contract.EncodeSessionL2CountReply, sourceSession)
}

// keyExists answers whether any memory holds this key, in any tier.
//
// EXISTS rather than the C's SELECT 1 ... LIMIT 1 read back as a row count:
// the two ask the same question and EXISTS says so in the statement, which
// stops it looking like a read whose row somebody forgot to use.
func keyExists(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, err := db2contract.DecodeKeyExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, keyExistsQuery,
		db2contract.EncodeKeyExistsReply, key)
}

// findIDByKeyKind answers the identifier of the memory under this key and kind.
//
// Two fields, and the found flag is why: an identifier of zero is a real
// identifier nowhere in this schema, but the reply says "not found" explicitly
// rather than leaving a caller to infer it from a zero.
func findIDByKeyKind(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, kind, err := db2contract.DecodeFindIDByKeyKindRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id int64
	found := uint32(1)
	switch scanErr := store.QueryRow(ctx, findIDByKeyKindQuery, key, kind).
		Scan(&id); {
	case errors.Is(scanErr, pgx.ErrNoRows):
		found, id = 0, 0
	case scanErr != nil:
		return nil, bus.ModuleStatusInternal
	}
	if id < 0 {
		id = 0
	}
	reply, encodeErr := db2contract.EncodeFindIDByKeyKindReply(found, uint64(id))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// keyExistsInTierPair answers whether the key is held in either of two tiers.
//
// Two tiers rather than a list because the caller's question is always a
// promotion question -- is this already at the tier above, or the one below --
// and a general list would invite a scan the index cannot help with.
func keyExistsInTierPair(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, tierA, tierB, err := db2contract.DecodeKeyExistsInTierPairRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, keyExistsInTierPairQuery,
		db2contract.EncodeKeyExistsInTierPairReply, key, tierA, tierB)
}

// countReply runs a scalar count and answers it in a u32 field.
//
// The floor is not decoration: COUNT never returns a negative, but the reply
// field is unsigned and a negative scanned into it would encode as an enormous
// count rather than as the error it is.
func countReply(ctx context.Context, store Store, query string,
	encode func(uint32) ([]byte, error), args ...any) ([]byte, bus.ModuleStatus) {
	var count int64
	if err := store.QueryRow(ctx, query, args...).Scan(&count); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if count < 0 {
		count = 0
	}
	reply, encodeErr := encode(uint32(count))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// existsReply runs an EXISTS and answers it as the contract's 0 or 1.
func existsReply(ctx context.Context, store Store, query string,
	encode func(uint32) ([]byte, error), args ...any) ([]byte, bus.ModuleStatus) {
	var exists bool
	if err := store.QueryRow(ctx, query, args...).Scan(&exists); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	value := uint32(0)
	if exists {
		value = 1
	}
	reply, encodeErr := encode(value)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
