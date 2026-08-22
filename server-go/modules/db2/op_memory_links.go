package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryLinkCreate,
		db2contract.OperationMemoryLinkCreate, memoryLinkCreate)
	Register(db2contract.StageMemoryScopeTagInsert,
		db2contract.OperationMemoryScopeTagInsert, memoryScopeTagInsert)
	Register(db2contract.StageMemoryMarkMergedInto,
		db2contract.OperationMemoryMarkMergedInto, memoryMarkMergedInto)
	Register(db2contract.StageMemoryTemporalRefsList,
		db2contract.OperationMemoryTemporalRefsList, memoryTemporalRefsList)
	Register(db2contract.StageMemorySupersededKeys,
		db2contract.OperationMemorySupersededKeys, memorySupersededKeys)
	Register(db2contract.StageSessionMemories,
		db2contract.OperationSessionMemories, sessionMemories)
	Register(db2contract.StageProspectiveCounts,
		db2contract.OperationProspectiveCounts, prospectiveCounts)
}

const (
	memoryLinkCreateQuery = `INSERT INTO memory_links (source_id, target_id, relation)
 VALUES ($1, $2, $3) ON CONFLICT DO NOTHING`
	memoryScopeTagInsertQuery = `INSERT INTO memory_scopes
 (memory_id, scope_type, scope_value)
 VALUES ($1, $2, $3) ON CONFLICT DO NOTHING`
)

// memoryLinkCreate records that one memory relates to another.
//
// Directed and unvalidated: nothing checks that the relation is one the reader
// understands, so what relations exist is decided by whoever writes them. The
// depends-on read is the only one that names a particular relation, and it
// looks for exactly "depends_on".
func memoryLinkCreate(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	sourceID, targetID, relation, err := db2contract.DecodeMemoryLinkCreateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryLinkCreateQuery,
		int64(sourceID), int64(targetID), relation)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryLinkCreateReply)
}

// memoryScopeTagInsert tags a memory as belonging to a scope.
//
// This is the write behind the visibility rank: a memory tagged 'project' with
// a project's name outranks one tagged 'workspace', and one tagged neither is
// treated as shared. So the scope_type written here decides who sees the memory
// first, and a typo in it silently demotes the memory to shared rather than
// failing -- nothing constrains the column to the three the rank knows.
func memoryScopeTagInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, scopeType, scopeValue, err :=
		db2contract.DecodeMemoryScopeTagInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryScopeTagInsertQuery,
		int64(memoryID), scopeType, scopeValue)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryScopeTagInsertReply)
}

const memoryMarkMergedIntoQuery = `UPDATE memories SET merged_into = $1
 WHERE tier = 'L1' AND confidence <= $2 AND kind = 'fact'
   AND source_session = $3 AND merged_into = 0`

// memoryMarkMergedInto folds a session's weak scratch facts into one memory.
//
// Five predicates, and each narrows what a session cleanup is allowed to touch:
// only L1, only facts, only below a confidence the caller sets, only this
// session's, and only rows not already merged. That last one is what makes the
// operation safe to run twice -- a second pass finds nothing left to fold, and
// without it a re-run would re-point memories already merged elsewhere.
//
// Note that the caller names the confidence ceiling rather than the operation
// fixing one. A caller passing a high ceiling folds nearly everything the
// session produced, and nothing here refuses that.
func memoryMarkMergedInto(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	mergedInto, sessionID, maxConfidence, err :=
		db2contract.DecodeMemoryMarkMergedIntoRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryMarkMergedIntoQuery,
		int64(mergedInto), maxConfidence, sessionID)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryMarkMergedIntoReply)
}

const memoryTemporalRefsListQuery = `SELECT ref_key, granularity, weight
 FROM memory_temporal_refs
 WHERE memory_id = $1 ORDER BY weight DESC, id ASC`

// memoryTemporalRefsList lists when a memory refers to, most salient first.
//
// The same weight-then-insertion ordering the entity list uses, for the same
// reason: without the tiebreak, equally weighted references would come back in
// whatever order the planner chose.
func memoryTemporalRefsList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryTemporalRefsListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryTemporalRefsListMaxRows
	rows, queryErr := store.Query(ctx, memoryTemporalRefsListQuery, int64(memoryID))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryTemporalRefsListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var refKey, granularity *string
		var weight *float64
		if err := rows.Scan(&refKey, &granularity, &weight); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryTemporalRefsListRow{
			RefKey:      text(refKey),
			Granularity: text(granularity),
			RefWeight:   decimal(weight),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryTemporalRefsListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The COUNT(*) is repeated in the HAVING rather than referring to the alias,
// because PostgreSQL evaluates HAVING before the select list and rejects an
// alias there. The C carries the same note.
//
// The base key is everything before the "#v" marker, so "deploy:target#v3" and
// "deploy:target#v4" group together. A key containing "#v" for some other
// reason is grouped by whatever precedes it, which is a quirk of using a
// substring as a key rather than a column.
const memorySupersededKeysQuery = `SELECT SUBSTR(key, 1, STRPOS(key, '#v') - 1) AS base_key,
 COUNT(*) AS versions
 FROM memories WHERE key LIKE '%#v%'
 GROUP BY SUBSTR(key, 1, STRPOS(key, '#v') - 1)
 HAVING COUNT(*) >= $1
 ORDER BY COUNT(*) DESC LIMIT $2`

// memorySupersededKeys finds keys that have accumulated versions.
//
// Ordered by how many, so the most rewritten key comes first -- this is a
// maintenance read looking for churn, and the keys with the most versions are
// the ones worth consolidating.
func memorySupersededKeys(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	minVersions, limit, err := db2contract.DecodeMemorySupersededKeysRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := int(limit)
	if ceiling <= 0 || ceiling > db2contract.MemorySupersededKeysMaxRows {
		ceiling = db2contract.MemorySupersededKeysMaxRows
	}
	rows, queryErr := store.Query(ctx, memorySupersededKeysQuery,
		int64(minVersions), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemorySupersededKeysRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var baseKey *string
		var versions *int64
		if err := rows.Scan(&baseKey, &versions); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemorySupersededKeysRow{
			BaseKey:      text(baseKey),
			VersionCount: clampToU32(number(versions)),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemorySupersededKeysReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const sessionMemoriesQuery = `SELECT id, content FROM memories
 WHERE source_session = $1 ORDER BY id ASC LIMIT $2`

// sessionMemories lists what a session wrote, in the order it wrote it.
//
// Ascending by id, which is insertion order: a caller replaying what happened
// wants it forwards, unlike almost every other read here that answers newest
// first.
func sessionMemories(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	sessionID, limit, err := db2contract.DecodeSessionMemoriesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := int(limit)
	if ceiling <= 0 || ceiling > db2contract.SessionMemoriesMaxRows {
		ceiling = db2contract.SessionMemoriesMaxRows
	}
	rows, queryErr := store.Query(ctx, sessionMemoriesQuery, sessionID, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.SessionMemoriesRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var content *string
		if err := rows.Scan(&id, &content); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.SessionMemoriesRow{
			MemoryRowID:   clampToU64(number(id)),
			MemoryContent: text(content),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeSessionMemoriesReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Filtered aggregates rather than the C's grouped scan, for the reason the
// directive counts needed them: the reply has four fixed fields, and a grouped
// read fills only the states it happens to find. The four names are
// MEMORY_PROSPECTIVE_STATE_*, and a prospective memory in any other state is
// counted in none of them -- which is what the C does too.
const prospectiveCountsQuery = `SELECT
 COUNT(*) FILTER (WHERE state = 'armed'),
 COUNT(*) FILTER (WHERE state = 'triggered'),
 COUNT(*) FILTER (WHERE state = 'completed'),
 COUNT(*) FILTER (WHERE state = 'expired')
 FROM prospective_memories`

// prospectiveCounts reports how many triggers are waiting and what became of
// the rest.
func prospectiveCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeProspectiveCountsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var armed, triggered, completed, expired *int64
	if scanErr := store.QueryRow(ctx, prospectiveCountsQuery).
		Scan(&armed, &triggered, &completed, &expired); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProspectiveCountsReply(
		clampToU32(number(armed)), clampToU32(number(triggered)),
		clampToU32(number(completed)), clampToU32(number(expired)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
