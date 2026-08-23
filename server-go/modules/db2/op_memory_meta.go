package db2

import (
	"context"
	"errors"

	"github.com/jackc/pgx/v5"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryStateFields,
		db2contract.OperationMemoryStateFields, memoryStateFields)
	Register(db2contract.StageMemoryProvenanceByID,
		db2contract.OperationMemoryProvenanceByID, memoryProvenanceByID)
	Register(db2contract.StageMemoryUnitActiveMeta,
		db2contract.OperationMemoryUnitActiveMeta, memoryUnitActiveMeta)
	Register(db2contract.StageLifecycleCounts,
		db2contract.OperationLifecycleCounts, lifecycleCounts)
	Register(db2contract.StageMemorySetArtifact,
		db2contract.OperationMemorySetArtifact, memorySetArtifact)
	Register(db2contract.StageMemoryEntityInsert,
		db2contract.OperationMemoryEntityInsert, memoryEntityInsert)
	Register(db2contract.StageMemoryTemporalInsert,
		db2contract.OperationMemoryTemporalInsert, memoryTemporalInsert)
}

const memoryStateFieldsQuery = `SELECT valid_until, observation_count, use_count
 FROM memories WHERE id = $1`

// memoryStateFields answers the three fields a decay pass reads together.
//
// valid_until comes back as a yes-or-no rather than the value: the caller is
// deciding whether the memory has an expiry at all, not when it is. The C
// answers the same way, and an empty string counts as absent alongside NULL --
// which matters, because the column is written both ways.
func memoryStateFields(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryStateFieldsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// valid_until is nullable and the two counts are not, so only the first
	// needs a pointer. Reaching for one on the others would say something untrue
	// about the schema.
	var validUntil *string
	var observations, uses int64
	found := uint32(1)
	hasValidUntil := uint32(0)
	if scanErr := store.QueryRow(ctx, memoryStateFieldsQuery, int64(memoryID)).
		Scan(&validUntil, &observations, &uses); scanErr != nil {
		found, observations, uses = 0, 0, 0
	} else if text(validUntil) != "" {
		hasValidUntil = 1
	}
	reply, encodeErr := db2contract.EncodeMemoryStateFieldsReply(found, hasValidUntil,
		uint32(observations), uint32(uses))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryProvenanceByIDQuery = `SELECT kind, source_session, updated_at
 FROM memories WHERE id = $1`

// The three answers this operation can give. Absent and failed are separate
// because they mean different things to a caller: a memory that was superseded
// or deleted has a provenance question with a real answer, and a read that went
// wrong has none.
const (
	provenanceAbsent uint32 = 0
	provenanceFound  uint32 = 1
	provenanceFailed uint32 = 2
)

// memoryProvenanceByID answers where a memory came from and when it last
// changed.
//
// updated_at stands in for a version: the memory has no version column, and the
// last time it changed is what a caller comparing two copies actually needs.
func memoryProvenanceByID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryProvenanceByIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// source_session is the nullable one of the three: a memory can be written
	// by something that is not a session.
	var kind, version string
	var session *string
	result := provenanceFound
	if scanErr := store.QueryRow(ctx, memoryProvenanceByIDQuery, int64(memoryID)).
		Scan(&kind, &session, &version); scanErr != nil {
		result = provenanceFailed
		if errors.Is(scanErr, pgx.ErrNoRows) {
			result = provenanceAbsent
		}
		kind, session, version = "", nil, ""
	}
	reply, encodeErr := db2contract.EncodeMemoryProvenanceByIDReply(
		result, kind, text(session), version)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The tier filter is the whole point of "active" in the name: a unit whose
// memory has fallen out of the three live tiers is not a unit to score, and the
// join is what makes that visible -- the unit row itself carries no tier.
const memoryUnitActiveMetaQuery = `SELECT u.weight, u.unit_type, u.memory_kind
 FROM memory_units u JOIN memories m ON m.id = u.memory_id
 WHERE u.id = $1 AND m.tier IN ('L1', 'L2', 'L3')`

// memoryUnitActiveMeta answers what a retrieval unit is and how much it weighs,
// for units belonging to a memory that is still live.
func memoryUnitActiveMeta(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	unitID, err := db2contract.DecodeMemoryUnitActiveMetaRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// All three columns are declared NOT NULL, so none of them needs a pointer.
	var weight float64
	var unitType, unitKind string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, memoryUnitActiveMetaQuery, int64(unitID)).
		Scan(&weight, &unitType, &unitKind); scanErr != nil {
		found, weight, unitType, unitKind = 0, 0, "", ""
	}
	reply, encodeErr := db2contract.EncodeMemoryUnitActiveMetaReply(
		found, weight, unitType, unitKind)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Filtered aggregates rather than the C's GROUP BY, for the reason
// directive_counts_by_state uses them: the reply has five fixed fields, and a
// grouped read fills only the states it happens to find. One row always comes
// back, so a state with no memories reads as zero rather than as nothing.
//
// A lifecycle_state outside the five is counted by neither, exactly as the C's
// chain of comparisons ignores it.
const lifecycleCountsQuery = `SELECT
 COUNT(*) FILTER (WHERE lifecycle_state = 'active'),
 COUNT(*) FILTER (WHERE lifecycle_state = 'pending'),
 COUNT(*) FILTER (WHERE lifecycle_state = 'fulfilled'),
 COUNT(*) FILTER (WHERE lifecycle_state = 'superseded'),
 COUNT(*) FILTER (WHERE lifecycle_state = 'archived')
 FROM memories`

// lifecycleCounts answers how many memories sit in each lifecycle state.
func lifecycleCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeLifecycleCountsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var active, pending, fulfilled, superseded, archived int64
	if err := store.QueryRow(ctx, lifecycleCountsQuery).Scan(
		&active, &pending, &fulfilled, &superseded, &archived); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeLifecycleCountsReply(uint64(active),
		uint64(pending), uint64(fulfilled), uint64(superseded), uint64(archived))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// NULLIF, so an absent hash is stored as NULL rather than as an empty string.
// That is not cosmetic: memory_artifact_hashed_list selects on artifact_hash IS
// NOT NULL, so an empty string here would put an unverifiable artifact in front
// of the verification pass. The C binds NULL for the same reason.
const memorySetArtifactQuery = `UPDATE memories
 SET artifact_type = $1, artifact_ref = $2, artifact_hash = NULLIF($3, '')
 WHERE id = $4`

// memorySetArtifact points a memory at the artifact it was derived from.
//
// The reply is the changed-row count rather than statement success, so a
// memory identifier that matches nothing reads as unchanged.
func memorySetArtifact(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, artifactType, artifactRef, artifactHash, err :=
		db2contract.DecodeMemorySetArtifactRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, memorySetArtifactQuery,
		artifactType, artifactRef, artifactHash, int64(memoryID))
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeMemorySetArtifactReply)
}

// ON CONFLICT DO NOTHING on both inserts: the same entity or the same temporal
// reference being extracted twice from one memory is the expected case, not a
// collision worth failing over. There is nothing to update -- a second
// extraction carries the same weight the first did.
const (
	memoryEntityInsertQuery = `INSERT INTO memory_entities
 (memory_id, entity, role, weight) VALUES ($1, $2, $3, $4)
 ON CONFLICT DO NOTHING`
	memoryTemporalInsertQuery = `INSERT INTO memory_temporal_refs
 (memory_id, ref_key, granularity, weight) VALUES ($1, $2, $3, $4)
 ON CONFLICT DO NOTHING`
)

// memoryEntityInsert records that a memory mentions an entity.
//
// The role is stored as given, including empty. The C substitutes "mention"
// for a NULL role, but the adapter decodes into a buffer so the pointer is
// never NULL and an absent role arrives as the empty string -- the default is
// unreachable through the module, and an empty role is what lands today.
//
// Acknowledgement means the statement ran. The C acknowledges unconditionally
// because its backend returns void and there is nothing left to report by the
// time the adapter sees it; that error is not lost here.
func memoryEntityInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, entityName, entityRole, entityWeight, err :=
		db2contract.DecodeMemoryEntityInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryEntityInsertQuery,
		int64(memoryID), entityName, entityRole, entityWeight)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryEntityInsertReply)
}

// memoryTemporalInsert records that a memory refers to a point in time.
//
// As with the entity role, the granularity is stored as given: the C's
// "relative" default sits behind a NULL the module never produces. It matters
// slightly more here, because the granularity is what orders temporal
// references when one has to be picked -- and an empty one sorts into the
// same bucket as anything else unrecognised, which is last.
func memoryTemporalInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, refKey, granularity, refWeight, err :=
		db2contract.DecodeMemoryTemporalInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryTemporalInsertQuery,
		int64(memoryID), refKey, granularity, refWeight)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryTemporalInsertReply)
}
