package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMatchErrorKeys,
		db2contract.OperationMatchErrorKeys, matchErrorKeys)
	Register(db2contract.StageMemoryIdsByUpdated,
		db2contract.OperationMemoryIdsByUpdated, memoryIDsByUpdated)
	Register(db2contract.StageUnitIdsForMemory,
		db2contract.OperationUnitIdsForMemory, unitIDsForMemory)
	Register(db2contract.StageMemoryRelationDates,
		db2contract.OperationMemoryRelationDates, memoryRelationDates)
	Register(db2contract.StageMemoryDependsOnKeys,
		db2contract.OperationMemoryDependsOnKeys, memoryDependsOnKeys)
	Register(db2contract.StageMemorySessionContent,
		db2contract.OperationMemorySessionContent, memorySessionContent)
	Register(db2contract.StageMemorySessionCreatedAt,
		db2contract.OperationMemorySessionCreatedAt, memorySessionCreatedAt)
}

const matchErrorKeysQuery = `SELECT id FROM memories
 WHERE tier IN ('L1', 'L2') AND confidence > 0.3
   AND $1 LIKE '%' || LOWER(key) || '%'`

// matchErrorKeys finds memories whose key appears inside an error message.
//
// The comparison runs backwards from the usual direction: the error text is the
// subject and the memory key is the pattern, so a key is matched when the error
// contains it. Two consequences follow and neither is accidental.
//
// A key is used as a LIKE pattern, so a key containing % or _ matches more than
// its literal text. Nothing escapes it here because nothing escapes it in the C
// either, and a key wide enough to match everything would surface every error
// rather than silently matching none -- loud rather than quiet, which is the
// better of the two failures for a diagnostic read.
//
// The caller is expected to have lowercased the error already; only the key
// side is lowercased here. An error passed in mixed case matches nothing.
func matchErrorKeys(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	errorLowered, err := db2contract.DecodeMatchErrorKeysRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ids, status := readIntColumn(ctx, store,
		db2contract.MatchErrorKeysMaxRows, matchErrorKeysQuery, errorLowered)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MatchErrorKeysRow, 0, len(ids))
	for _, id := range ids {
		found = append(found, db2contract.MatchErrorKeysRow{MemoryID: clampToU64(id)})
	}
	reply, err := db2contract.EncodeMatchErrorKeysReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// NULLIF is how one statement covers both C branches: the C builds its SQL with
// a LIMIT clause only when the limit is positive, and LIMIT NULL in Postgres is
// no limit at all. A zero limit therefore means every memory, bounded only by
// the reply's own ceiling.
const memoryIDsByUpdatedQuery = `SELECT id FROM memories
 ORDER BY updated_at DESC LIMIT NULLIF($1, 0)`

// memoryIDsByUpdated lists memory identifiers, most recently updated first.
func memoryIDsByUpdated(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, err := db2contract.DecodeMemoryIdsByUpdatedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ids, status := readIntColumn(ctx, store,
		db2contract.MemoryIdsByUpdatedMaxRows, memoryIDsByUpdatedQuery, int64(limit))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MemoryIdsByUpdatedRow, 0, len(ids))
	for _, id := range ids {
		found = append(found, db2contract.MemoryIdsByUpdatedRow{MemoryID: clampToU64(id)})
	}
	reply, err := db2contract.EncodeMemoryIdsByUpdatedReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const unitIDsForMemoryQuery = `SELECT id FROM memory_units WHERE memory_id = $1`

// unitIDsForMemory lists the units a memory was split into.
//
// No ORDER BY, so the order is whatever the database returns. Callers that care
// sort for themselves; imposing one here would be inventing an ordering the C
// never promised.
func unitIDsForMemory(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, err := db2contract.DecodeUnitIdsForMemoryRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ids, status := readIntColumn(ctx, store,
		db2contract.UnitIdsForMemoryMaxRows, unitIDsForMemoryQuery, int64(memoryID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.UnitIdsForMemoryRow, 0, len(ids))
	for _, id := range ids {
		found = append(found, db2contract.UnitIdsForMemoryRow{UnitID: clampToU64(id)})
	}
	reply, err := db2contract.EncodeUnitIdsForMemoryReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryRelationDatesQuery = `SELECT mr.valid_at FROM memory_relations mr
 WHERE mr.memory_id = $1 AND mr.valid_at != ''
 AND mr.relation IN ('OCCURRED_AT', 'occurred_at', 'valid_from')
 ORDER BY mr.valid_at ASC`

// memoryRelationDates lists the dates a memory is anchored to.
//
// The relation names are matched case-sensitively against a list of three. Both
// spellings of OCCURRED_AT are in it and only one of valid_from, which reads
// like an oversight and is not one to fix here: widening it with LOWER() would
// pull in relations this read has never returned, and what depends on that is
// not visible from this file.
func memoryRelationDates(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryRelationDatesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	dates, status := readTextColumn(ctx, store,
		db2contract.MemoryRelationDatesMaxRows, memoryRelationDatesQuery, int64(memoryID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MemoryRelationDatesRow, 0, len(dates))
	for _, date := range dates {
		found = append(found, db2contract.MemoryRelationDatesRow{RelationDate: date})
	}
	reply, err := db2contract.EncodeMemoryRelationDatesReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryDependsOnKeysQuery = `SELECT m.key FROM memory_links ml
 JOIN memories m ON m.id = ml.target_id
 WHERE ml.source_id = $1 AND ml.relation = 'depends_on' LIMIT $2`

// dependsOnKeysCeiling is three, and it is not the reply's row ceiling.
//
// Three is stated twice in the C: the only caller asks for three, and the
// implementation clamps to three regardless of what it is asked for. It renders
// a "(depends on: a, b, c)" hint into a fixed 128-byte buffer beside a memory,
// so a fourth key has nowhere to go. The request carries no limit, which is why
// the number lives here rather than travelling with the call. Left at the
// contract's 256 the port would return a longer list nothing reads, at a join
// per row.
const dependsOnKeysCeiling = 3

// memoryDependsOnKeys lists the keys of the memories a memory depends on.
func memoryDependsOnKeys(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryDependsOnKeysRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	keys, status := readTextColumn(ctx, store, dependsOnKeysCeiling,
		memoryDependsOnKeysQuery, int64(memoryID), dependsOnKeysCeiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MemoryDependsOnKeysRow, 0, len(keys))
	for _, key := range keys {
		found = append(found, db2contract.MemoryDependsOnKeysRow{DependsOnKey: key})
	}
	reply, err := db2contract.EncodeMemoryDependsOnKeysReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Both session reads are ordered by created_at ascending and filtered to L1, so
// what comes back is the session's own working memory in the order it was
// written. The two are separate operations returning parallel lists rather than
// one returning both columns, which means a caller pairing them by index is
// trusting two statements to have seen the same rows. That is the C shape.
const (
	memorySessionContentQuery = `SELECT content FROM memories
 WHERE tier = 'L1' AND source_session = $1
 ORDER BY created_at ASC`
	memorySessionCreatedAtQuery = `SELECT created_at FROM memories
 WHERE tier = 'L1' AND source_session = $1
 ORDER BY created_at ASC`
)

// memorySessionContent lists the content of a session's L1 memories.
func memorySessionContent(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sessionID, err := db2contract.DecodeMemorySessionContentRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	contents, status := readTextColumn(ctx, store,
		db2contract.MemorySessionContentMaxRows, memorySessionContentQuery, sessionID)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MemorySessionContentRow, 0, len(contents))
	for _, content := range contents {
		found = append(found, db2contract.MemorySessionContentRow{MemoryContent: content})
	}
	reply, err := db2contract.EncodeMemorySessionContentReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// memorySessionCreatedAt lists when a session's L1 memories were written.
func memorySessionCreatedAt(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sessionID, err := db2contract.DecodeMemorySessionCreatedAtRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	stamps, status := readTextColumn(ctx, store,
		db2contract.MemorySessionCreatedAtMaxRows, memorySessionCreatedAtQuery, sessionID)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.MemorySessionCreatedAtRow, 0, len(stamps))
	for _, stamp := range stamps {
		found = append(found, db2contract.MemorySessionCreatedAtRow{MemoryCreatedAt: stamp})
	}
	reply, err := db2contract.EncodeMemorySessionCreatedAtReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
