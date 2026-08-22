package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageSceneMemberExists,
		db2contract.OperationSceneMemberExists, sceneMemberExists)
	Register(db2contract.StageUnitEdgeExists,
		db2contract.OperationUnitEdgeExists, unitEdgeExists)
	Register(db2contract.StageMemoriesByKey,
		db2contract.OperationMemoriesByKey, memoriesByKey)
	Register(db2contract.StageMemoryConfidenceByKey,
		db2contract.OperationMemoryConfidenceByKey, memoryConfidenceByKey)
	Register(db2contract.StageMemoryScopesList,
		db2contract.OperationMemoryScopesList, memoryScopesList)
	Register(db2contract.StageMemorySceneMemberships,
		db2contract.OperationMemorySceneMemberships, memorySceneMemberships)
	Register(db2contract.StageMemoryTierKindCounts,
		db2contract.OperationMemoryTierKindCounts, memoryTierKindCounts)
}

const sceneMemberExistsQuery = `SELECT 1 FROM memory_scene_members
 WHERE memory_id = $1 AND scene_id = $2 LIMIT 1`

// sceneMemberExists reports whether a memory belongs to a scene.
func sceneMemberExists(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, sceneID, err := db2contract.DecodeSceneMemberExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	present, status := rowExists(ctx, store, sceneMemberExistsQuery,
		int64(memoryID), int64(sceneID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeSceneMemberExistsReply(present)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const unitEdgeExistsQuery = `SELECT 1 FROM memory_unit_edges
 WHERE ((src_unit_id = $1 AND dst_unit_id = $2)
     OR (src_unit_id = $2 AND dst_unit_id = $1)) LIMIT 1`

// unitEdgeExists reports whether two units are connected, in either direction.
//
// The edge table stores a direction and this read ignores it, asking both ways
// round: a caller checking whether two units are already linked does not want a
// second edge created because the existing one points the other way.
func unitEdgeExists(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	unitA, unitB, err := db2contract.DecodeUnitEdgeExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	present, status := rowExists(ctx, store, unitEdgeExistsQuery, int64(unitA), int64(unitB))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeUnitEdgeExistsReply(present)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// rowExists answers the "is there one" shape these two share.
//
// SELECT 1 ... LIMIT 1, so the database stops at the first match rather than
// counting: the question is existence and a count would invite a caller to
// start reading meaning into the number.
func rowExists(ctx context.Context, store Store, query string, args ...any) (
	uint32, bus.ModuleStatus,
) {
	var marker *int64
	if err := store.QueryRow(ctx, query, args...).Scan(&marker); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return 0, bus.ModuleStatusOK
		}
		return 0, bus.ModuleStatusInternal
	}
	return 1, bus.ModuleStatusOK
}

const memoriesByKeyQuery = `SELECT id, content FROM memories WHERE key = $1`

// memoriesByKey lists every memory stored under a key, exactly.
//
// No normalisation and no case folding, unlike the entity reads: the caller is
// scanning the duplicates of one key looking for contradictions between them,
// so a key that differs by a character is a different key and belongs to a
// different question.
//
// Several rows for one key is the expected case rather than an error. Nothing
// makes keys unique, and finding the duplicates is the point.
func memoriesByKey(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	key, err := db2contract.DecodeMemoriesByKeyRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoriesByKeyMaxRows
	rows, queryErr := store.Query(ctx, memoriesByKeyQuery, key)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoriesByKeyRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var content *string
		if err := rows.Scan(&id, &content); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoriesByKeyRow{
			MemoryRowID:   clampToU64(number(id)),
			MemoryContent: text(content),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoriesByKeyReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryConfidenceByKeyQuery = `SELECT confidence FROM memories WHERE key = $1`

// memoryConfidenceByKey reads the confidence of the memory under a key.
//
// The found flag is separate from the value because zero is a real confidence:
// a memory nobody believes and a key nothing holds would otherwise be the same
// answer, and they lead to opposite decisions.
//
// No ordering and no limit, so a key with several memories answers with
// whichever the database returns first. That is the C behaviour and it is only
// sensible where the caller already knows the key is singular -- memoriesByKey
// is the read for when it is not.
func memoryConfidenceByKey(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, err := db2contract.DecodeMemoryConfidenceByKeyRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var confidence *float64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, memoryConfidenceByKeyQuery, key).
		Scan(&confidence); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		found = 0
	}
	reply, err := db2contract.EncodeMemoryConfidenceByKeyReply(found, decimal(confidence))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Ordered local-first, the same precedence the visibility rank uses: project,
// then workspace, then global, then anything else. A caller rendering a
// memory's scopes shows the narrowest first, which is the one that explains why
// they can see it.
const memoryScopesListQuery = `SELECT scope_type, scope_value FROM memory_scopes
 WHERE memory_id = $1
 ORDER BY CASE scope_type
            WHEN 'project' THEN 0
            WHEN 'workspace' THEN 1
            WHEN 'global' THEN 2
            ELSE 3 END, scope_value ASC`

// memoryScopesList lists the scopes a memory is tagged with.
func memoryScopesList(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, err := db2contract.DecodeMemoryScopesListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryScopesListMaxRows
	rows, queryErr := store.Query(ctx, memoryScopesListQuery, int64(memoryID))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryScopesListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var scopeType, scopeValue *string
		if err := rows.Scan(&scopeType, &scopeValue); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryScopesListRow{
			ScopeType:  text(scopeType),
			ScopeValue: text(scopeValue),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryScopesListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memorySceneMembershipsQuery = `SELECT scene_id, membership_strength
 FROM memory_scene_members WHERE memory_id = $1`

// memorySceneMemberships lists the scenes a memory belongs to and how strongly.
//
// No ordering, so a caller wanting the strongest sorts for itself. Imposing one
// here would be inventing a guarantee the C never made.
func memorySceneMemberships(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemorySceneMembershipsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemorySceneMembershipsMaxRows
	rows, queryErr := store.Query(ctx, memorySceneMembershipsQuery, int64(memoryID))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemorySceneMembershipsRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var sceneID *int64
		var strength *float64
		if err := rows.Scan(&sceneID, &strength); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemorySceneMembershipsRow{
			SceneID:            clampToU64(number(sceneID)),
			MembershipStrength: decimal(strength),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemorySceneMembershipsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryTierKindCountsQuery = `SELECT tier, kind, COUNT(*) FROM memories
 GROUP BY tier, kind ORDER BY tier, kind`

// memoryTierKindCounts reports how many memories sit at each tier and kind.
//
// Every memory is counted, including archived and superseded ones: this is the
// shape of the store rather than the shape of what is visible. A caller wanting
// only live memories has to ask a different question, and none of the reads
// here answers it.
func memoryTierKindCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeMemoryTierKindCountsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryTierKindCountsMaxRows
	rows, queryErr := store.Query(ctx, memoryTierKindCountsQuery)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryTierKindCountsRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var tier, kind *string
		var count *int64
		if err := rows.Scan(&tier, &kind, &count); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryTierKindCountsRow{
			MemoryTier:  text(tier),
			MemoryKind:  text(kind),
			MemoryCount: clampToU32(number(count)),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryTierKindCountsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
