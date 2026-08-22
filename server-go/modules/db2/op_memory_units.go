package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageLifecycleUpdateState,
		db2contract.OperationLifecycleUpdateState, lifecycleUpdateState)
	Register(db2contract.StageMemoryAliasInsert,
		db2contract.OperationMemoryAliasInsert, memoryAliasInsert)
	Register(db2contract.StageMemoryEpisodeCardInsert,
		db2contract.OperationMemoryEpisodeCardInsert, memoryEpisodeCardInsert)
	Register(db2contract.StageMemoryEntitiesList,
		db2contract.OperationMemoryEntitiesList, memoryEntitiesList)
	Register(db2contract.StageMemoryIDKeyContent,
		db2contract.OperationMemoryIDKeyContent, memoryIDKeyContent)
	Register(db2contract.StageMemoryEvidenceFields,
		db2contract.OperationMemoryEvidenceFields, memoryEvidenceFields)
	Register(db2contract.StageDirectiveCountsByState,
		db2contract.OperationDirectiveCountsByState, directiveCountsByState)
}

// Four columns move on one transition, and each CASE is a separate rule:
//
// Leaving 'pending' clears the deadline, because a memory that is no longer
// waiting to be confirmed has nothing to expire.
//
// Entering 'archived' records the reason. Any other transition leaves whatever
// reason was there, so archiving and then reviving does not erase why it was
// archived.
//
// Entering 'superseded' or 'archived' closes the row's event-time interval,
// and only when it is still open. lifecycle_state answers "is this true now"
// and nothing else -- a superseded row looks identically superseded whether it
// stopped being true yesterday or last year -- so valid_until is what lets an
// as-of read answer what was believed at a date. It is set only when empty
// because the stamp is when we learned the fact stopped holding, and a caller
// who knows the real end date must not have it overwritten by that.
const lifecycleUpdateStateQuery = `UPDATE memories
 SET lifecycle_state = $2,
     ttl_at = CASE WHEN $2 = 'pending' THEN ttl_at ELSE '' END,
     archive_reason = CASE WHEN $2 = 'archived' THEN $3 ELSE archive_reason END,
     valid_until = CASE WHEN $2 IN ('superseded','archived')
                         AND COALESCE(valid_until, '') = ''
                        THEN to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
                        ELSE valid_until END,
     updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE id = $1`

// lifecycleUpdateState moves a memory between lifecycle states.
//
// The C binds the same state four times because its wrapper numbers
// placeholders positionally; one parameter serves all four here.
func lifecycleUpdateState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, state, reason, err := db2contract.DecodeLifecycleUpdateStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, lifecycleUpdateStateQuery, int64(memoryID), state, reason)
	return acknowledgement(execErr == nil, db2contract.EncodeLifecycleUpdateStateReply)
}

const memoryAliasInsertQuery = `INSERT INTO memory_aliases (memory_id, alias, weight)
 VALUES ($1, $2, $3)
 ON CONFLICT (memory_id, alias) DO NOTHING`

// memoryAliasInsert records another name a memory is known by.
//
// The conflict does nothing rather than updating, so re-observing an alias
// keeps the weight first recorded. A later observation cannot raise it, which
// is what stops the same alias seen repeatedly from outranking one seen once
// with real confidence.
func memoryAliasInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, alias, weight, err := db2contract.DecodeMemoryAliasInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryAliasInsertQuery, int64(memoryID), alias, weight)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryAliasInsertReply)
}

// The weight of 2.0 and the episode-card flag are constants of the row shape
// rather than caller input: an episode card is one kind of unit and every one
// of them is weighted the same. A caller wanting a different weight is asking
// for a different kind of unit.
const memoryEpisodeCardInsertQuery = `INSERT INTO memory_units
 (memory_id, unit_type, memory_kind, unit_key, unit_text, weight, is_episode_card)
 VALUES ($1, 'episode_card', 'episodic', $2, $3, 2.0, 1)
 ON CONFLICT (memory_id, unit_type, unit_key, unit_text) DO NOTHING`

// memoryEpisodeCardInsert stores the card summarising what happened.
//
// The conflict target includes the text, so a card whose wording changed is a
// new row rather than an update. Two cards for one memory are then both
// present, and the read beside this takes whichever comes first -- which is
// why re-summarising the same episode differently is not something to do
// casually.
func memoryEpisodeCardInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, unitKey, unitText, err :=
		db2contract.DecodeMemoryEpisodeCardInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryEpisodeCardInsertQuery,
		int64(memoryID), unitKey, unitText)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryEpisodeCardInsertReply)
}

const memoryEntitiesListQuery = `SELECT entity, role, weight FROM memory_entities
 WHERE memory_id = $1 ORDER BY weight DESC, id ASC`

// memoryEntitiesList lists what a memory is about, most salient first.
//
// The tiebreak on id is insertion order, which makes the answer stable across
// reads when several entities share a weight -- without it the order of equally
// weighted entities would be the planner's to choose, and a caller rendering
// the first few would see them shuffle.
func memoryEntitiesList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryEntitiesListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryEntitiesListMaxRows
	rows, queryErr := store.Query(ctx, memoryEntitiesListQuery, int64(memoryID))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryEntitiesListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var entity, role *string
		var weight *float64
		if err := rows.Scan(&entity, &role, &weight); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryEntitiesListRow{
			EntityName:   text(entity),
			EntityRole:   text(role),
			EntityWeight: decimal(weight),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryEntitiesListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// NULLIF for the same reason memory_ids_by_updated needs it: the C appends a
// LIMIT clause only when the limit is positive, so zero means every memory and
// a plain LIMIT $1 would answer none.
const memoryIDKeyContentQuery = `SELECT id, key, content FROM memories
 ORDER BY updated_at DESC LIMIT NULLIF($1, 0)`

// memoryIDKeyContent lists memories with their keys and text, most recently
// updated first.
func memoryIDKeyContent(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, err := db2contract.DecodeMemoryIDKeyContentRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryIDKeyContentMaxRows
	rows, queryErr := store.Query(ctx, memoryIDKeyContentQuery, int64(limit))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryIDKeyContentRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		var key, content *string
		if err := rows.Scan(&id, &key, &content); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryIDKeyContentRow{
			MemoryID:      clampToU64(number(id)),
			MemoryKey:     text(key),
			MemoryContent: text(content),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryIDKeyContentReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryEvidenceFieldsQuery = `SELECT evidence_strength, observation_count
 FROM memories WHERE id = $1`

// memoryEvidenceFields reads how well supported a memory is.
//
// Found is separate from the values for the reason it always is here: strength
// zero and observation count zero are a real state -- a memory asserted once and
// never corroborated -- and a memory nothing holds is another. A caller
// deciding whether to promote reads them differently.
func memoryEvidenceFields(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryEvidenceFieldsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var strength *float64
	var observations *int64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, memoryEvidenceFieldsQuery, int64(memoryID)).
		Scan(&strength, &observations); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		found = 0
	}
	reply, err := db2contract.EncodeMemoryEvidenceFieldsReply(
		found, decimal(strength), clampToU32(number(observations)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Filtered aggregates rather than the C's grouped scan, so a state with no
// directives reads as zero instead of being absent. The reply has four fixed
// fields, and a grouped read has to fill them by matching names it may not
// find -- which is how a state silently keeps its previous value.
const directiveCountsByStateQuery = `SELECT
 COUNT(*) FILTER (WHERE state = 'open'),
 COUNT(*) FILTER (WHERE state = 'suppressed'),
 COUNT(*) FILTER (WHERE state = 'resolved'),
 COUNT(*) FILTER (WHERE state = 'expired')
 FROM epistemic_directives`

// directiveCountsByState reports how many open questions there are and what
// became of the rest.
//
// The four states are named here rather than discovered, so a directive in some
// fifth state is counted in none of them. That is the C behaviour: its grouped
// scan matches the same four names and drops anything else.
func directiveCountsByState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeDirectiveCountsByStateRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var open, suppressed, resolved, expired *int64
	if scanErr := store.QueryRow(ctx, directiveCountsByStateQuery).
		Scan(&open, &suppressed, &resolved, &expired); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeDirectiveCountsByStateReply(
		clampToU64(number(open)), clampToU64(number(suppressed)),
		clampToU64(number(resolved)), clampToU64(number(expired)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
