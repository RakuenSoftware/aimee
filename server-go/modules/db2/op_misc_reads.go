package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryUnitsList,
		db2contract.OperationMemoryUnitsList, memoryUnitsList)
	Register(db2contract.StageTaskEdges,
		db2contract.OperationTaskEdges, taskEdges)
	Register(db2contract.StageAntiPatternList,
		db2contract.OperationAntiPatternList, antiPatternList)
	Register(db2contract.StageConsoleOidcGet,
		db2contract.OperationConsoleOidcGet, consoleOIDCGet)
	Register(db2contract.StageProjectionGenerationMeta,
		db2contract.OperationProjectionGenerationMeta, projectionGenerationMeta)
}

// Ordered by id, which the C leaves to the planner. A memory's units are a
// decomposition of one text, and reading them in the order they were extracted
// is what makes the decomposition legible -- an unordered read can hand the
// same memory back in a different order on the next call.
const memoryUnitsListQuery = `SELECT id, unit_type, unit_key, unit_text, weight
 FROM memory_units WHERE memory_id = $1 ORDER BY id ASC LIMIT $2`

// memoryUnitsList lists the retrieval units a memory was broken into.
func memoryUnitsList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryUnitsListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryUnitsListMaxRows
	rows, queryErr := store.Query(ctx, memoryUnitsListQuery,
		int64(memoryID), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	units := make([]db2contract.MemoryUnitsListRow, 0, 8)
	for rows.Next() {
		var id int64
		var unitType, unitKey, unitText string
		var weight float64
		if scanErr := rows.Scan(
			&id, &unitType, &unitKey, &unitText, &weight); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		units = append(units, db2contract.MemoryUnitsListRow{
			UnitID:     uint64(id),
			UnitType:   unitType,
			UnitKey:    unitKey,
			UnitText:   unitText,
			UnitWeight: weight,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryUnitsListReply(units)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Either end, because an edge is a fact about a pair and a caller asking what a
// task is connected to means both what it depends on and what depends on it.
//
// One parameter where the C binds the task twice, which is the same predicate
// said once.
const taskEdgesQuery = `SELECT id, source_id, target_id, relation FROM task_edges
 WHERE source_id = $1 OR target_id = $1 ORDER BY id LIMIT $2`

// taskEdges lists the relations a task takes part in.
//
// Ordered by id, which the C leaves unordered. A caller rendering a dependency
// list wants it stable between calls, and there is no other ordering available
// -- task_edges carries no timestamp.
func taskEdges(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	taskID, limit, err := db2contract.DecodeTaskEdgesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.TaskEdgesMaxRows
	rows, queryErr := store.Query(ctx, taskEdgesQuery,
		int64(taskID), int64(pairLimit(limit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	edges := make([]db2contract.TaskEdgesRow, 0, 8)
	for rows.Next() {
		var id, source, target int64
		var relation string
		if scanErr := rows.Scan(&id, &source, &target, &relation); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		edges = append(edges, db2contract.TaskEdgesRow{
			TaskEdgeID:   uint64(id),
			SourceTaskID: uint64(source),
			TargetTaskID: uint64(target),
			TaskRelation: relation,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTaskEdgesReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Most-hit first, then most-confident. A pattern that has caught something
// repeatedly is worth showing before one that merely looks likely, and
// confidence breaks the tie between patterns nothing has hit yet.
const antiPatternListQuery = `SELECT id, pattern, description, source, source_ref,
 hit_count, confidence
 FROM anti_patterns ORDER BY hit_count DESC, confidence DESC LIMIT $1`

// antiPatternList lists everything recorded as not to do.
func antiPatternList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeAntiPatternListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.AntiPatternListMaxRows
	rows, queryErr := store.Query(ctx, antiPatternListQuery, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	patterns := make([]db2contract.AntiPatternListRow, 0, 16)
	for rows.Next() {
		var id, hits int64
		var pattern, description, source, sourceRef string
		var confidence float64
		if scanErr := rows.Scan(&id, &pattern, &description, &source,
			&sourceRef, &hits, &confidence); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		patterns = append(patterns, db2contract.AntiPatternListRow{
			AntiPatternID:      uint64(id),
			HitCount:           clampToU32(hits),
			Confidence:         confidence,
			Pattern:            pattern,
			PatternDescription: description,
			PatternSource:      source,
			SourceRef:          sourceRef,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeAntiPatternListReply(patterns)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const consoleOIDCGetQuery = `SELECT issuer, audience, jwks_url, admin_claim,
 admin_values, updated_at FROM kb_console_oidc WHERE id = 1`

// consoleOIDCGet answers the console's identity provider settings.
//
// The configured flag distinguishes a console nobody has set up from one set up
// with every field left blank. They encode identically otherwise, and only one
// of them is a mistake worth telling an operator about.
func consoleOIDCGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeConsoleOidcGetRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var issuer, audience, jwksURL, adminClaim, adminValues, updatedAt string
	configured := uint32(1)
	if scanErr := store.QueryRow(ctx, consoleOIDCGetQuery).Scan(&issuer, &audience,
		&jwksURL, &adminClaim, &adminValues, &updatedAt); scanErr != nil {
		configured = 0
		issuer, audience, jwksURL = "", "", ""
		adminClaim, adminValues, updatedAt = "", "", ""
	}
	reply, encodeErr := db2contract.EncodeConsoleOidcGetReply(configured, issuer,
		audience, jwksURL, adminClaim, adminValues, updatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The C selects the identifier and never reads it -- the caller supplied it --
// so it is dropped here.
const projectionGenerationMetaQuery = `SELECT project, state, source_hash,
 extractor_version, pipeline_version
 FROM code_projection_generations WHERE id = $1`

// projectionGenerationMeta answers what produced a projection generation.
//
// The two versions are what a snapshot diff checks before comparing
// generations: a parser change would read as a structural change, so comparing
// across extractor versions says something about the extractor rather than
// about the code. Both are empty on generations created before the columns
// existed, which is a real answer -- unknown rather than equal.
func projectionGenerationMeta(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	generationID, err :=
		db2contract.DecodeProjectionGenerationMetaRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var project, state, sourceHash, extractorVersion, pipelineVersion string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, projectionGenerationMetaQuery,
		int64(generationID)).Scan(&project, &state, &sourceHash,
		&extractorVersion, &pipelineVersion); scanErr != nil {
		found = 0
		project, state, sourceHash = "", "", ""
		extractorVersion, pipelineVersion = "", ""
	}
	reply, encodeErr := db2contract.EncodeProjectionGenerationMetaReply(found,
		project, state, sourceHash, extractorVersion, pipelineVersion)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
