package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryConflictList,
		db2contract.OperationMemoryConflictList, memoryConflictList)
	Register(db2contract.StageMemoryEventFramesList,
		db2contract.OperationMemoryEventFramesList, memoryEventFramesList)
	Register(db2contract.StageMemoryProvenanceList,
		db2contract.OperationMemoryProvenanceList, memoryProvenanceList)
	Register(db2contract.StageMemoryLookupByKey,
		db2contract.OperationMemoryLookupByKey, memoryLookupByKey)
	Register(db2contract.StageMemoryLineageInsert,
		db2contract.OperationMemoryLineageInsert, memoryLineageInsert)
	Register(db2contract.StageMemoryRelationInsert,
		db2contract.OperationMemoryRelationInsert, memoryRelationInsert)
}

// Unresolved only, newest first. A resolved conflict is history rather than
// work, and this list is what a resolution pass reads to decide what to do
// next -- so including settled rows would push the open ones off the end.
//
// resolved = 0 rather than a boolean: the column is a BIGINT, and the C
// compares it the same way.
const memoryConflictListQuery = `SELECT id, memory_a, memory_b, detected_at,
 resolved, resolution
 FROM memory_conflicts WHERE resolved = 0
 ORDER BY detected_at DESC LIMIT $1`

// memoryConflictList lists contradictions nobody has settled yet.
func memoryConflictList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeMemoryConflictListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryConflictListMaxRows
	rows, queryErr := store.Query(ctx, memoryConflictListQuery, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	conflicts := make([]db2contract.MemoryConflictListRow, 0, 16)
	for rows.Next() {
		var id, memoryA, memoryB, resolved int64
		var detectedAt string
		// resolution is the one nullable column here: a conflict that has not
		// been settled has no resolution to record.
		var resolution *string
		if scanErr := rows.Scan(&id, &memoryA, &memoryB, &detectedAt,
			&resolved, &resolution); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		conflicts = append(conflicts, db2contract.MemoryConflictListRow{
			ConflictID:         uint64(id),
			MemoryA:            uint64(memoryA),
			MemoryB:            uint64(memoryB),
			DetectedAt:         detectedAt,
			ConflictResolved:   clampToU32(resolved),
			ConflictResolution: text(resolution),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryConflictListReply(conflicts)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Ascending by id, so the frames read in the order they were extracted. A
// memory can carry several frames describing one event from different angles,
// and the extraction order is the only ordering they have -- event_time is
// free text and frequently empty.
const memoryEventFramesListQuery = `SELECT actor, action, object, location, event_time
 FROM memory_event_frames WHERE memory_id = $1 ORDER BY id ASC LIMIT $2`

// memoryEventFramesList lists who did what, to what, where and when, for one
// memory.
//
// Every column is declared NOT NULL with an empty default, so an unfilled slot
// is an empty string rather than an absent one. A frame with only an actor and
// an action is normal: the extractor fills what the text supports.
func memoryEventFramesList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryEventFramesListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryEventFramesListMaxRows
	rows, queryErr := store.Query(ctx, memoryEventFramesListQuery,
		int64(memoryID), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	frames := make([]db2contract.MemoryEventFramesListRow, 0, 8)
	for rows.Next() {
		var actor, action, object, location, eventTime string
		if scanErr := rows.Scan(
			&actor, &action, &object, &location, &eventTime); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		frames = append(frames, db2contract.MemoryEventFramesListRow{
			FrameActor:     actor,
			FrameAction:    action,
			FrameObject:    object,
			FrameLocation:  location,
			FrameEventTime: eventTime,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryEventFramesListReply(frames)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Oldest first, by the recorded time rather than by id. Provenance is a
// narrative -- created, then reinforced, then merged -- and reading it forwards
// is what makes it one.
//
// The C selects memory_id and never uses it, since the caller supplied it. It
// is dropped here rather than selected and discarded.
const memoryProvenanceListQuery = `SELECT id, session_id, action, details, created_at
 FROM memory_provenance WHERE memory_id = $1
 ORDER BY created_at ASC LIMIT $2`

// memoryProvenanceList lists what has happened to a memory and who did it.
func memoryProvenanceList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryProvenanceListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryProvenanceListMaxRows
	rows, queryErr := store.Query(ctx, memoryProvenanceListQuery,
		int64(memoryID), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	entries := make([]db2contract.MemoryProvenanceListRow, 0, 8)
	for rows.Next() {
		var id int64
		var session, action, createdAt string
		// details is nullable: an action can be recorded without one.
		var details *string
		if scanErr := rows.Scan(
			&id, &session, &action, &details, &createdAt); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		entries = append(entries, db2contract.MemoryProvenanceListRow{
			ProvenanceID:        uint64(id),
			SessionID:           session,
			ProvenanceAction:    action,
			ProvenanceDetails:   text(details),
			ProvenanceCreatedAt: createdAt,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryProvenanceListReply(entries)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Nothing constrains a key to one memory, so this can match several and the
// statement says nothing about which one it wants. That is the C's behaviour
// and it is left alone: choosing -- the newest, or the most confident -- would
// be a retrieval policy, and this operation is the one a caller reaches for
// when it believes the key is unique.
//
// The LIMIT is new. It does not decide which row comes back, since there is
// still no ordering; it stops the server materialising every memory under a
// key when the caller will read one.
const memoryLookupByKeyQuery = `SELECT id, content, confidence, tier
 FROM memories WHERE key = $1 LIMIT 1`

// memoryLookupByKey answers the memory filed under a key.
func memoryLookupByKey(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, err := db2contract.DecodeMemoryLookupByKeyRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id int64
	var content, tier string
	var confidence float64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, memoryLookupByKeyQuery, key).
		Scan(&id, &content, &confidence, &tier); scanErr != nil {
		found, id, content, confidence, tier = 0, 0, "", 0, ""
	}
	reply, encodeErr := db2contract.EncodeMemoryLookupByKeyReply(
		found, uint64(id), content, confidence, tier)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// ingested_at is left to the column's default rather than bound, as the C does.
const memoryLineageInsertQuery = `INSERT INTO memory_lineage
 (object_type, object_id, source_kind, source_ref, confidence)
 VALUES ($1, $2, $3, $4, $5) RETURNING id`

// memoryLineageInsert records where an object came from and answers the
// identifier of the record.
//
// The object is named by type and identifier rather than by a foreign key,
// because lineage is recorded for several kinds of object and no single table
// could carry the reference. Nothing checks that the object exists, so a
// lineage row can outlive what it describes.
func memoryLineageInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	objectType, objectID, sourceKind, sourceRef, confidence, err :=
		db2contract.DecodeMemoryLineageInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var lineageID int64
	if scanErr := store.QueryRow(ctx, memoryLineageInsertQuery,
		objectType, int64(objectID), sourceKind, sourceRef, confidence).
		Scan(&lineageID); scanErr != nil {
		// Zero is "no record", which is what the C's failure return becomes.
		lineageID = 0
	}
	if lineageID < 0 {
		lineageID = 0
	}
	reply, encodeErr := db2contract.EncodeMemoryLineageInsertReply(uint64(lineageID))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// No conflict clause, because the table has no constraint to collide with: the
// same relation extracted twice from one memory is two rows. Unlike the entity
// and temporal extractions, which do have one, this is a bare insert -- and
// that difference is the schema's rather than a choice made here.
const memoryRelationInsertQuery = `INSERT INTO memory_relations
 (memory_id, src_entity, relation, dst_entity, fact_text)
 VALUES ($1, $2, $3, $4, $5)`

// memoryRelationInsert records a subject-relation-object fact drawn from a
// memory.
//
// The fact text is stored alongside the triple rather than derived from it: the
// triple is what a graph query matches on, and the text is what a person reads
// when asking why the graph believes it.
func memoryRelationInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, srcEntity, relation, dstEntity, factText, err :=
		db2contract.DecodeMemoryRelationInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryRelationInsertQuery,
		int64(memoryID), srcEntity, relation, dstEntity, factText)
	return dispatchAcknowledgement(execErr, "memory_relation_insert",
		db2contract.EncodeMemoryRelationInsertReply)
}
