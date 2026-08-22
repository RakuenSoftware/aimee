package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageRelationsForEntity,
		db2contract.OperationRelationsForEntity, relationsForEntity)
	Register(db2contract.StageRelationsSearch,
		db2contract.OperationRelationsSearch, relationsSearch)
	Register(db2contract.StageRelationsSearchAsOf,
		db2contract.OperationRelationsSearchAsOf, relationsSearchAsOf)
	Register(db2contract.StageRelationsSupporting,
		db2contract.OperationRelationsSupporting, relationsSupporting)
	Register(db2contract.StageMemoryEpisodesSearch,
		db2contract.OperationMemoryEpisodesSearch, memoryEpisodesSearch)
	Register(db2contract.StageMemorySearchByPattern,
		db2contract.OperationMemorySearchByPattern, memorySearchByPattern)
	Register(db2contract.StageLifecycleStalePending,
		db2contract.OperationLifecycleStalePending, lifecycleStalePending)
	Register(db2contract.StageLifecycleNewlySuperseded,
		db2contract.OperationLifecycleNewlySuperseded, lifecycleNewlySuperseded)
}

// All four relation reads select the same eleven columns and scope on the
// memory the relation belongs to, not on the relation itself: a relation is as
// visible as the memory that asserts it, and memory_relations carries no scope
// tags of its own.
//
// COALESCE on episode_id because it is nullable and the reply is not; zero
// means "not from an episode", which is what the C encodes.
const relationColumns = `SELECT r.id, r.memory_id, COALESCE(r.episode_id, 0), r.src_entity,
 r.relation, r.dst_entity, r.fact_text, r.valid_at, r.invalid_at, r.weight, r.created_at
 FROM memory_relations r`

// readRelations collects the shared eleven-column shape.
func readRelations(ctx context.Context, store Store, statement string, args []any, ceiling int) (
	[]db2contract.RelationsSearchRow, bus.ModuleStatus,
) {
	rows, err := store.Query(ctx, statement, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.RelationsSearchRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id, memoryID, episodeID *int64
		var weight *float64
		var source, relation, target, fact, validAt, invalidAt, createdAt *string
		if err := rows.Scan(&id, &memoryID, &episodeID, &source, &relation, &target,
			&fact, &validAt, &invalidAt, &weight, &createdAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.RelationsSearchRow{
			RelationID:        clampToU64(number(id)),
			RelationMemoryID:  clampToU64(number(memoryID)),
			EpisodeID:         clampToU64(number(episodeID)),
			RelationWeight:    decimal(weight),
			SrcEntity:         text(source),
			RelationName:      text(relation),
			DstEntity:         text(target),
			FactText:          text(fact),
			ValidAt:           text(validAt),
			InvalidAt:         text(invalidAt),
			RelationCreatedAt: text(createdAt),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

// relationCeiling applies the C's rule that a limit outside the buffer means
// the buffer: zero or more than the reply holds becomes the reply's ceiling.
func relationCeiling(limit uint32, maximum int) int {
	ceiling := int(limit)
	if ceiling <= 0 || ceiling > maximum {
		ceiling = maximum
	}
	return ceiling
}

// relationsForEntity lists the relations naming an entity on either side.
//
// An exact match on either end, not a substring: this answers "what is known
// about this entity", where relationsSearch answers "what mentions this text".
// Case-insensitive on both sides so a caller need not know how it was written.
func relationsForEntity(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, flags, workspace, project, err :=
		db2contract.DecodeRelationsForEntityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	// $1 the limit, $2 the entity, then the scope four.
	filter, scopeArgs := scope.filter("r.memory_id", 2)
	rank := scope.rankExpression("r.memory_id", 2)
	statement := relationColumns +
		` WHERE (LOWER(r.src_entity) = LOWER($2) OR LOWER(r.dst_entity) = LOWER($2))` +
		filter + ` ORDER BY ` + rank + ` DESC, r.weight DESC, r.created_at DESC LIMIT $1`

	ceiling := relationCeiling(limit, db2contract.RelationsForEntityMaxRows)
	args := append([]any{int64(ceiling), entity}, scopeArgs...)
	found, status := readRelations(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.RelationsForEntityRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.RelationsForEntityRow(row))
	}
	reply, err := db2contract.EncodeRelationsForEntityReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// relationsSearchWhere is the substring match the two search reads share: any
// of the four text columns containing the query.
//
// The query is a LIKE pattern with its own wildcards intact, as in the C. A
// query of "%" matches everything, which is a wide answer rather than a wrong
// one, and the scope filter still applies to it.
const relationsSearchWhere = ` WHERE (LOWER(r.src_entity) LIKE '%' || LOWER($2) || '%'
 OR LOWER(r.relation) LIKE '%' || LOWER($2) || '%'
 OR LOWER(r.dst_entity) LIKE '%' || LOWER($2) || '%'
 OR LOWER(r.fact_text) LIKE '%' || LOWER($2) || '%')`

// relationsSearch finds relations mentioning a query anywhere.
//
// Relations carrying a valid_at sort before those without, so a fact that has
// been placed in time beats one that has not at equal weight.
func relationsSearch(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	query, limit, flags, workspace, project, err :=
		db2contract.DecodeRelationsSearchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("r.memory_id", 2)
	rank := scope.rankExpression("r.memory_id", 2)
	statement := relationColumns + relationsSearchWhere + filter +
		` ORDER BY ` + rank + ` DESC, r.weight DESC,` +
		` CASE WHEN r.valid_at <> '' THEN 0 ELSE 1 END, r.created_at DESC LIMIT $1`

	ceiling := relationCeiling(limit, db2contract.RelationsSearchMaxRows)
	args := append([]any{int64(ceiling), query}, scopeArgs...)
	found, status := readRelations(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeRelationsSearchReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// relationsSearchAsOf is relationsSearch restricted to what was true at a time.
//
// An empty as_of falls through to the unrestricted search, which the C does by
// calling it directly. The two validity checks admit a relation whose bound is
// empty as well as one whose bound contains the instant, because an empty bound
// means "not known to have started" or "not known to have ended" rather than
// "started at the epoch" -- treating it as a real timestamp would hide every
// relation nobody has dated.
func relationsSearchAsOf(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	query, asOf, limit, flags, workspace, project, err :=
		db2contract.DecodeRelationsSearchAsOfRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	ceiling := relationCeiling(limit, db2contract.RelationsSearchAsOfMaxRows)

	var statement string
	var args []any
	if asOf == "" {
		filter, scopeArgs := scope.filter("r.memory_id", 2)
		rank := scope.rankExpression("r.memory_id", 2)
		statement = relationColumns + relationsSearchWhere + filter +
			` ORDER BY ` + rank + ` DESC, r.weight DESC,` +
			` CASE WHEN r.valid_at <> '' THEN 0 ELSE 1 END, r.created_at DESC LIMIT $1`
		args = append([]any{int64(ceiling), query}, scopeArgs...)
	} else {
		// $1 the limit, $2 the query, $3 the instant, then the scope four.
		filter, scopeArgs := scope.filter("r.memory_id", 3)
		rank := scope.rankExpression("r.memory_id", 3)
		statement = relationColumns + relationsSearchWhere +
			` AND (r.valid_at = '' OR r.valid_at <= $3)` +
			` AND (r.invalid_at = '' OR r.invalid_at > $3)` + filter +
			` ORDER BY ` + rank + ` DESC, r.weight DESC, r.created_at DESC LIMIT $1`
		args = append([]any{int64(ceiling), query, asOf}, scopeArgs...)
	}

	found, status := readRelations(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.RelationsSearchAsOfRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.RelationsSearchAsOfRow(row))
	}
	reply, err := db2contract.EncodeRelationsSearchAsOfReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// relationsSupporting finds relations that could back an assertion about a
// token.
//
// Only relations carrying fact text, because a relation with none supports
// nothing a caller could quote. The token is wrapped in wildcards here rather
// than by the caller, matching the C, so a caller passing its own would get
// them doubled -- harmless in LIKE, and preserved rather than tidied.
func relationsSupporting(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	token, limit, flags, workspace, project, err :=
		db2contract.DecodeRelationsSupportingRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("r.memory_id", 2)
	rank := scope.rankExpression("r.memory_id", 2)
	statement := relationColumns +
		` WHERE (LOWER(r.src_entity) LIKE LOWER($2) OR LOWER(r.dst_entity) LIKE LOWER($2))` +
		` AND r.fact_text != ''` + filter +
		` ORDER BY ` + rank + ` DESC, r.weight DESC LIMIT $1`

	ceiling := relationCeiling(limit, db2contract.RelationsSupportingMaxRows)
	args := append([]any{int64(ceiling), "%" + token + "%"}, scopeArgs...)
	found, status := readRelations(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.RelationsSupportingRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.RelationsSupportingRow(row))
	}
	reply, err := db2contract.EncodeRelationsSupportingReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// memoryEpisodesSearch finds episodes matching a query.
//
// An empty query matches every episode rather than none -- the first disjunct
// is the query being empty -- so this doubles as "list the episodes". An exact
// key match sorts first, then episodes that carry a reference time, then the
// most recent.
func memoryEpisodesSearch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	query, limit, flags, workspace, project, err :=
		db2contract.DecodeMemoryEpisodesSearchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("e.memory_id", 2)
	rank := scope.rankExpression("e.memory_id", 2)
	statement := `SELECT e.id, e.memory_id, e.episode_key, e.episode_text, e.source_session,
 e.reference_time, e.created_at FROM memory_episodes e
 WHERE ($2 = '' OR LOWER(e.episode_key) LIKE '%' || LOWER($2) || '%'
 OR LOWER(e.episode_text) LIKE '%' || LOWER($2) || '%'
 OR LOWER(e.source_session) LIKE '%' || LOWER($2) || '%')` + filter +
		` ORDER BY ` + rank + ` DESC,` +
		` CASE WHEN LOWER(e.episode_key) = LOWER($2) THEN 0 ELSE 1 END,` +
		` CASE WHEN e.reference_time <> '' THEN 0 ELSE 1 END,` +
		` e.created_at DESC LIMIT $1`

	ceiling := relationCeiling(limit, db2contract.MemoryEpisodesSearchMaxRows)
	args := append([]any{int64(ceiling), query}, scopeArgs...)
	rows, queryErr := store.Query(ctx, statement, args...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryEpisodesSearchRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id, memoryID *int64
		var key, episodeText, session, reference, createdAt *string
		if err := rows.Scan(&id, &memoryID, &key, &episodeText, &session,
			&reference, &createdAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryEpisodesSearchRow{
			EpisodeID:        clampToU64(number(id)),
			MemoryID:         clampToU64(number(memoryID)),
			EpisodeKey:       text(key),
			EpisodeText:      text(episodeText),
			SourceSession:    text(session),
			ReferenceTime:    text(reference),
			EpisodeCreatedAt: text(createdAt),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryEpisodesSearchReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// memorySearchByPattern finds working memories whose key or content matches.
//
// The pattern is passed to LIKE verbatim, wildcards and all: this is the read a
// caller uses when it already knows the shape it wants, so escaping it here
// would take away the only thing it does.
//
// Ordered by scope rank alone, with no second key. Rows of equal rank come back
// in whatever order the database chooses, which is the C behaviour.
func memorySearchByPattern(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pattern, flags, workspace, project, err :=
		db2contract.DecodeMemorySearchByPatternRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id, m.key, m.content FROM memories m
 WHERE (m.tier = 'L1' OR m.tier = 'L2')
 AND (m.key LIKE $2 OR m.content LIKE $2)` + filter +
		` ORDER BY ` + rank + ` DESC LIMIT $1`

	ceiling := db2contract.MemorySearchByPatternMaxRows
	args := append([]any{int64(ceiling), pattern}, scopeArgs...)
	rows, queryErr := store.Query(ctx, statement, args...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemorySearchByPatternRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var key, content *string
		if err := rows.Scan(&id, &key, &content); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemorySearchByPatternRow{
			MemoryID:      clampToU64(number(id)),
			MemoryKey:     text(key),
			MemoryContent: text(content),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemorySearchByPatternReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// ageDays and windowDays are fractional deliberately. A pending memory eight
// tenths of the way through a two-day window is stale; rounding to whole days
// would make the same memory stale a day early or a day late depending on when
// it was written.
const lifecycleStalePendingStatement = `SELECT m.id, m.content, m.created_at, m.ttl_at,
 CAST((EXTRACT(EPOCH FROM 'now'::timestamp)/86400.0
       - EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0) AS REAL) AS age_days,
 CAST((EXTRACT(EPOCH FROM m.ttl_at::timestamp)/86400.0
       - EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0) AS REAL) AS window_days
 FROM memories m
 WHERE m.lifecycle_state = 'pending' AND m.ttl_at != ''
 AND EXTRACT(EPOCH FROM m.ttl_at::timestamp)/86400.0
     > EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0
 AND (EXTRACT(EPOCH FROM 'now'::timestamp)/86400.0
      - EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0)
     >= 0.8 * (EXTRACT(EPOCH FROM m.ttl_at::timestamp)/86400.0
               - EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0)`

// lifecycleStalePending lists pending memories near the end of their window.
//
// Eighty percent elapsed, not expired: this is the warning list, so a caller
// can resolve a commitment before it lapses rather than after. A memory whose
// ttl is not after its creation is excluded, because its window is not a window
// and every such row would look infinitely stale.
func lifecycleStalePending(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	flags, workspace, project, err := db2contract.DecodeLifecycleStalePendingRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", scopedLimitPlaceholder)
	rank := scope.rankExpression("m.id", scopedLimitPlaceholder)
	statement := lifecycleStalePendingStatement + filter +
		` ORDER BY ` + rank + ` DESC, EXTRACT(EPOCH FROM 'now'::timestamp)/86400.0` +
		` - EXTRACT(EPOCH FROM m.created_at::timestamp)/86400.0 DESC LIMIT $1`

	ceiling := db2contract.LifecycleStalePendingMaxRows
	rows, queryErr := store.Query(ctx, statement, scopedArgs(ceiling, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.LifecycleStalePendingRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var content, createdAt, ttlAt *string
		var age, window *float64
		if err := rows.Scan(&id, &content, &createdAt, &ttlAt, &age, &window); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.LifecycleStalePendingRow{
			MemoryID:        clampToU64(number(id)),
			MemoryText:      text(content),
			MemoryCreatedAt: text(createdAt),
			TtlAt:           text(ttlAt),
			AgeDays:         decimal(age),
			WindowDays:      decimal(window),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeLifecycleStalePendingReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// lifecycleNewlySuperseded lists what has been superseded since an instant.
//
// The C builds two statements, one binding the caller's instant and one
// defaulting to seven days ago. NULLIF collapses them: an empty `since` means
// the default rather than the beginning of time, which matters because this
// feeds a "what changed" notice and the beginning of time is not a change.
func lifecycleNewlySuperseded(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	since, flags, workspace, project, err :=
		db2contract.DecodeLifecycleNewlySupersededRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	// $1 the limit, $2 the instant, then the scope four.
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id, m.key, m.content, m.updated_at FROM memories m
 WHERE m.lifecycle_state = 'superseded'
 AND m.updated_at >= COALESCE(NULLIF($2, ''), pg_now_text('-7 days'))` + filter +
		` ORDER BY ` + rank + ` DESC, m.updated_at DESC LIMIT $1`

	ceiling := db2contract.LifecycleNewlySupersededMaxRows
	args := append([]any{int64(ceiling), since}, scopeArgs...)
	rows, queryErr := store.Query(ctx, statement, args...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.LifecycleNewlySupersededRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var key, content, updatedAt *string
		if err := rows.Scan(&id, &key, &content, &updatedAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.LifecycleNewlySupersededRow{
			MemoryID:     clampToU64(number(id)),
			MemoryKey:    text(key),
			MemoryText:   text(content),
			SupersededAt: text(updatedAt),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeLifecycleNewlySupersededReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
