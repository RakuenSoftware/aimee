package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageGlobalConstraints,
		db2contract.OperationGlobalConstraints, globalConstraints)
	Register(db2contract.StageKvSection,
		db2contract.OperationKvSection, kvSection)
	Register(db2contract.StageRecallSection,
		db2contract.OperationRecallSection, recallSection)
	Register(db2contract.StageMemoryCandidates,
		db2contract.OperationMemoryCandidates, memoryCandidates)
	Register(db2contract.StageBriefingActiveEntities,
		db2contract.OperationBriefingActiveEntities, briefingActiveEntities)
	Register(db2contract.StageBriefingKeyFacts,
		db2contract.OperationBriefingKeyFacts, briefingKeyFacts)
	Register(db2contract.StageBriefingRecentActivity,
		db2contract.OperationBriefingRecentActivity, briefingRecentActivity)
	Register(db2contract.StageMemoryKeyFactsProvenance,
		db2contract.OperationMemoryKeyFactsProvenance, memoryKeyFactsProvenance)
}

// Every statement here spends $1 on its row limit and then the scope four, so
// the scope helpers are always told one placeholder is already used. The number
// is named rather than written as a literal at each site, because getting it
// wrong binds the limit to the scope and still runs.
const scopedLimitPlaceholder = 1

// scopedStatement assembles the shape all sixteen scoped reads share: a body,
// the scope filter, an ordering that leads with the scope rank, and a limit.
//
// The rank appears twice on purpose -- once filtering, once ordering. Filtering
// decides which memories a caller may see; ordering decides which of those come
// first, and local-before-shared is the whole point of the ranking. A read that
// filtered without ordering would return the right rows in an order that buries
// the project's own.
func scopedStatement(body, memoryID, ordering string, scope Scope) (string, []any) {
	filter, scopeArgs := scope.filter(memoryID, scopedLimitPlaceholder)
	rank := scope.rankExpression(memoryID, scopedLimitPlaceholder)
	return body + filter + " ORDER BY " + rank + " DESC, " + ordering + " LIMIT $1", scopeArgs
}

// scopedArgs puts the limit in front of the scope, matching the placeholders.
func scopedArgs(limit int, scopeArgs []any) []any {
	return append([]any{int64(limit)}, scopeArgs...)
}

// kvRow is one key and its content, before either operation's row type. The
// two reply types are structurally identical and generated separately, so a
// shared reader answers in neither of them rather than borrowing one.
type kvRow struct{ key, content string }

// readScopedKV serves the two operations returning a key and a content column.
func readScopedKV(ctx context.Context, store Store, statement string, args []any, ceiling int) (
	[]kvRow, bus.ModuleStatus,
) {
	rows, err := store.Query(ctx, statement, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]kvRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var key, content *string
		if err := rows.Scan(&key, &content); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, kvRow{key: text(key), content: text(content)})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

const globalConstraintsBody = `SELECT m.key, m.content FROM memories m
 WHERE m.kind IN ('preference', 'policy')
 AND m.tier IN ('L1', 'L2', 'L3', 'L4')`

// globalConstraints lists the preferences and policies a caller is bound by.
//
// "Global" names the kind of memory, not the breadth of the read: these are the
// standing constraints rather than one task's facts, and which of them a caller
// sees still depends on the scope they asked under.
func globalConstraints(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	flags, workspace, project, err := db2contract.DecodeGlobalConstraintsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	statement, scopeArgs := scopedStatement(globalConstraintsBody, "m.id",
		"m.confidence DESC, m.use_count DESC", scope)
	found, status := readScopedKV(ctx, store, statement,
		scopedArgs(db2contract.GlobalConstraintsMaxRows, scopeArgs),
		db2contract.GlobalConstraintsMaxRows)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.GlobalConstraintsRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.GlobalConstraintsRow{
			MemoryKey:     row.key,
			MemoryContent: row.content,
		})
	}
	reply, err := db2contract.EncodeGlobalConstraintsReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The five sections a briefing is assembled from. The selector picks the
// statement rather than parameterising one, because the sections differ in
// which tiers and kinds they admit and in what they order by after the scope
// rank -- a single statement with the differences bound would be five
// statements wearing one name.
var kvSectionBodies = map[uint32]struct{ body, ordering string }{
	kvSectionActiveTasks: {
		`SELECT m.key, m.content FROM memories m
 WHERE (m.tier = 'L1' OR m.tier = 'L2') AND m.kind = 'task'`,
		"m.updated_at DESC",
	},
	kvSectionRecentContext: {
		`SELECT m.key, m.content FROM memories m
 WHERE m.tier = 'L1' AND m.kind = 'episode'`,
		"m.created_at DESC",
	},
	kvSectionConstraints: {
		`SELECT m.key, m.content FROM memories m
 WHERE (m.tier = 'L2' OR m.tier = 'L3')
 AND (m.kind = 'decision' OR m.kind = 'policy')`,
		"m.confidence DESC",
	},
	kvSectionProcedures: {
		`SELECT m.key, m.content FROM memories m
 WHERE (m.tier = 'L1' OR m.tier = 'L2') AND m.kind = 'procedure'`,
		"m.confidence DESC, m.use_count DESC",
	},
	kvSectionFailureWarnings: {
		`SELECT m.key, m.content FROM memories m
 WHERE m.tier = 'L3' AND m.kind = 'episode' AND m.confidence > 0.3`,
		"m.created_at DESC",
	},
}

// The selector values, matching db2_memory_section_t. Every one of these enums
// starts at 1 rather than 0, and the adapter casts the wire value straight to
// the enum, so the wire value is the enum value. They are written out rather
// than derived from iota for that reason: iota would start at zero and every
// section would silently become its neighbour.
const (
	kvSectionActiveTasks     uint32 = 1
	kvSectionRecentContext   uint32 = 2
	kvSectionConstraints     uint32 = 3
	kvSectionProcedures      uint32 = 4
	kvSectionFailureWarnings uint32 = 5
)

// kvSection lists one section of a briefing.
//
// A selector naming no section answers empty rather than failing, which is what
// the C does: its switch falls through to returning nothing.
func kvSection(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	section, flags, workspace, project, err := db2contract.DecodeKvSectionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	chosen, known := kvSectionBodies[section]
	found := []kvRow{}
	if known {
		scope := DecodeScope(flags, workspace, project)
		statement, scopeArgs := scopedStatement(chosen.body, "m.id", chosen.ordering, scope)
		var status bus.ModuleStatus
		found, status = readScopedKV(ctx, store, statement,
			scopedArgs(db2contract.KvSectionMaxRows, scopeArgs),
			db2contract.KvSectionMaxRows)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
	}
	converted := make([]db2contract.KvSectionRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.KvSectionRow{
			MemoryKey:     row.key,
			MemoryContent: row.content,
		})
	}
	reply, err := db2contract.EncodeKvSectionReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The four recall sections. Unlike the briefing sections these select the
// memory's identity as well as its text, because a caller reattaches content
// by id.
var recallSectionBodies = map[uint32]struct{ body, ordering string }{
	recallSectionIdentity: {
		`SELECT m.id, m.tier, m.kind, m.key, m.content FROM memories m
 WHERE m.tier IN ('L2','L3','L4','L5')
 AND m.kind = 'fact'
 AND m.lifecycle_state != 'archived' AND m.lifecycle_state != 'superseded'
 AND (m.key LIKE 'identity:%' OR m.key LIKE 'name:%' OR m.key LIKE 'role:%'
      OR m.key LIKE 'user:%' OR m.key LIKE 'self:%')`,
		"m.confidence DESC, m.evidence_strength DESC, m.id DESC",
	},
	recallSectionPreferences: {
		`SELECT m.id, m.tier, m.kind, m.key, m.content FROM memories m
 WHERE m.tier IN ('L2','L3','L4','L5')
 AND m.kind = 'preference'
 AND m.lifecycle_state != 'archived' AND m.lifecycle_state != 'superseded'`,
		"m.confidence DESC, m.observation_count DESC, m.id DESC",
	},
	recallSectionActiveContext: {
		`SELECT m.id, m.tier, m.kind, m.key, m.content FROM memories m
 WHERE m.tier IN ('L1','L2','L3','L4')
 AND m.kind IN ('fact','decision','policy','task')
 AND m.lifecycle_state != 'archived' AND m.lifecycle_state != 'superseded'
 AND COALESCE(m.last_used_at, m.updated_at) >= pg_now_text('-7 days')`,
		"COALESCE(m.last_used_at, m.updated_at) DESC, m.id DESC",
	},
	recallSectionOpenCommitments: {
		`SELECT m.id, m.tier, m.kind, m.key, m.content FROM memories m
 WHERE m.lifecycle_state = 'pending'`,
		"COALESCE(m.ttl_at, m.created_at) ASC, m.id ASC",
	},
}

const (
	recallSectionIdentity        uint32 = 1
	recallSectionPreferences     uint32 = 2
	recallSectionActiveContext   uint32 = 3
	recallSectionOpenCommitments uint32 = 4
)

// recallSection lists one section of a recall assembly.
func recallSection(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	section, flags, workspace, project, err := db2contract.DecodeRecallSectionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	found := make([]db2contract.RecallSectionRow, 0, db2contract.RecallSectionMaxRows)
	if chosen, known := recallSectionBodies[section]; known {
		scope := DecodeScope(flags, workspace, project)
		statement, scopeArgs := scopedStatement(chosen.body, "m.id", chosen.ordering, scope)
		rows, queryErr := store.Query(ctx, statement,
			scopedArgs(db2contract.RecallSectionMaxRows, scopeArgs)...)
		if queryErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		defer rows.Close()
		for rows.Next() {
			if len(found) == db2contract.RecallSectionMaxRows {
				break
			}
			var id *int64
			var tier, kind, key, content *string
			if err := rows.Scan(&id, &tier, &kind, &key, &content); err != nil {
				return nil, bus.ModuleStatusInternal
			}
			found = append(found, db2contract.RecallSectionRow{
				MemoryRowID:   clampToU64(number(id)),
				MemoryTier:    text(tier),
				MemoryKey:     text(key),
				MemoryContent: text(content),
				MemoryKind:    text(kind),
			})
		}
		if rows.Err() != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	reply, err := db2contract.EncodeRecallSectionReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The two candidate pools. Primary is every user-facing tier; fallback reaches
// into L0 and drops L3 and L5, which is what makes it a fallback rather than a
// wider primary.
var memoryCandidateBodies = map[uint32]string{
	memoryCandidatesPrimary: `SELECT m.id, m.tier, m.key, m.content, m.kind, m.confidence,
 m.use_count FROM memories m WHERE m.tier IN ('L1', 'L2', 'L3', 'L4', 'L5')`,
	memoryCandidatesFallback: `SELECT m.id, m.tier, m.key, m.content, m.kind, m.confidence,
 m.use_count FROM memories m WHERE m.tier IN ('L0', 'L1', 'L2', 'L4')`,
}

const (
	memoryCandidatesPrimary  uint32 = 1
	memoryCandidatesFallback uint32 = 2
)

// memoryCandidates lists the memories a retrieval pass may draw from.
func memoryCandidates(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	filter, flags, workspace, project, err := db2contract.DecodeMemoryCandidatesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	found := make([]db2contract.MemoryCandidatesRow, 0, db2contract.MemoryCandidatesMaxRows)
	if body, known := memoryCandidateBodies[filter]; known {
		scope := DecodeScope(flags, workspace, project)
		statement, scopeArgs := scopedStatement(body, "m.id",
			"m.confidence DESC, m.use_count DESC", scope)
		rows, queryErr := store.Query(ctx, statement,
			scopedArgs(db2contract.MemoryCandidatesMaxRows, scopeArgs)...)
		if queryErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		defer rows.Close()
		for rows.Next() {
			if len(found) == db2contract.MemoryCandidatesMaxRows {
				break
			}
			var id, useCount *int64
			var confidence *float64
			var tier, key, content, kind *string
			if err := rows.Scan(&id, &tier, &key, &content, &kind,
				&confidence, &useCount); err != nil {
				return nil, bus.ModuleStatusInternal
			}
			found = append(found, db2contract.MemoryCandidatesRow{
				MemoryRowID:      clampToU64(number(id)),
				UseCount:         clampToU32(number(useCount)),
				MemoryConfidence: decimal(confidence),
				MemoryTier:       text(tier),
				MemoryKey:        text(key),
				MemoryContent:    text(content),
				MemoryKind:       text(kind),
			})
		}
		if rows.Err() != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	reply, err := db2contract.EncodeMemoryCandidatesReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const briefingActiveEntitiesBody = `SELECT me.entity, COUNT(*) AS mentions,
 MAX(COALESCE(m.last_used_at, m.updated_at)) AS last_seen
 FROM memory_entities me
 JOIN memories m ON m.id = me.memory_id
 WHERE me.entity <> ''
 AND (m.last_used_at IS NULL OR m.last_used_at >= pg_now_text('-30 days'))`

// briefingActiveEntities ranks entities by how often they are mentioned in
// memories touched recently.
//
// The only read here that groups, which changes where the scope rank goes: the
// filter still applies per memory before grouping, but the ordering takes
// MAX of the rank across each entity's memories. An entity is as local as its
// most local mention, which is why one project-scoped mention lifts an entity
// above one mentioned more often but only globally.
//
// Memories with no last_used_at count as recent, so a fresh write is not
// invisible until something reads it.
func briefingActiveEntities(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, flags, workspace, project, err :=
		db2contract.DecodeBriefingActiveEntitiesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", scopedLimitPlaceholder)
	rank := scope.rankExpression("m.id", scopedLimitPlaceholder)
	statement := briefingActiveEntitiesBody + filter +
		" GROUP BY me.entity ORDER BY MAX(" + rank + ") DESC," +
		" mentions DESC, last_seen DESC, me.entity ASC LIMIT $1"

	ceiling := int(limit)
	if ceiling > db2contract.BriefingActiveEntitiesMaxRows {
		ceiling = db2contract.BriefingActiveEntitiesMaxRows
	}
	rows, queryErr := store.Query(ctx, statement, scopedArgs(ceiling, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.BriefingActiveEntitiesRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var entity, lastSeen *string
		var mentions *int64
		if err := rows.Scan(&entity, &mentions, &lastSeen); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.BriefingActiveEntitiesRow{
			Entity:   text(entity),
			Mentions: clampToU32(number(mentions)),
			LastSeen: text(lastSeen),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeBriefingActiveEntitiesReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const briefingKeyFactsBody = `SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence,
 m.evidence_strength, m.observation_count, m.use_count,
 COALESCE(m.last_used_at, m.updated_at)
 FROM memories m
 WHERE m.tier IN ('L2','L3','L4','L5')
 AND m.kind != 'scratch'
 AND (m.sensitivity IS NULL OR m.sensitivity != 'secret')`

// briefingKeyFacts lists the facts worth putting in front of a caller.
//
// L0 and L1 are excluded because they are ephemeral scratch, and anything
// marked secret is excluded outright -- a briefing is assembled for display, so
// the sensitivity check is the last thing that should become optional. The
// final tiebreak on id keeps the ordering stable across runs when two rows
// score identically.
func briefingKeyFacts(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	flags, workspace, project, err := db2contract.DecodeBriefingKeyFactsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	statement, scopeArgs := scopedStatement(briefingKeyFactsBody, "m.id",
		"(m.confidence + m.evidence_strength) DESC,"+
			" m.observation_count DESC, m.use_count DESC, m.id DESC", scope)

	rows, queryErr := store.Query(ctx, statement,
		scopedArgs(db2contract.BriefingKeyFactsMaxRows, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.BriefingKeyFactsRow, 0, db2contract.BriefingKeyFactsMaxRows)
	for rows.Next() {
		if len(found) == db2contract.BriefingKeyFactsMaxRows {
			break
		}
		var id, observations, uses *int64
		var confidence, evidence *float64
		var tier, kind, key, content, lastSeen *string
		if err := rows.Scan(&id, &tier, &kind, &key, &content, &confidence,
			&evidence, &observations, &uses, &lastSeen); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.BriefingKeyFactsRow{
			MemoryID:         clampToU64(number(id)),
			MemoryTier:       text(tier),
			MemoryKind:       text(kind),
			MemoryKey:        text(key),
			MemoryText:       text(content),
			MemoryConfidence: decimal(confidence),
			EvidenceStrength: decimal(evidence),
			ObservationCount: clampToU32(number(observations)),
			UseCount:         clampToU32(number(uses)),
			LastSeenAt:       text(lastSeen),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeBriefingKeyFactsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// briefingRecentActivity lists one episode per session, most local first.
//
// The window function is why this is not the usual shape. Picking each
// session's highest-scoped episode before applying the limit stops a newer
// global episode from evicting an older project-scoped representative of the
// same session -- with a plain ORDER BY and LIMIT the global one wins on
// recency and the project's own work disappears from its own briefing.
func briefingRecentActivity(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	flags, workspace, project, err := db2contract.DecodeBriefingRecentActivityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("e.memory_id", scopedLimitPlaceholder)
	rank := scope.rankExpression("e.memory_id", scopedLimitPlaceholder)
	statement := `WITH ranked_episodes AS (
 SELECT e.source_session, e.episode_text, e.reference_time, e.created_at,
 ` + rank + ` AS scope_rank,
 ROW_NUMBER() OVER (PARTITION BY e.source_session
 ORDER BY ` + rank + ` DESC, e.created_at DESC) AS rn
 FROM memory_episodes e WHERE e.source_session <> ''` + filter + `)
 SELECT source_session, episode_text, reference_time, created_at
 FROM ranked_episodes WHERE rn = 1
 ORDER BY scope_rank DESC, created_at DESC, source_session DESC LIMIT $1`

	rows, queryErr := store.Query(ctx, statement,
		scopedArgs(db2contract.BriefingRecentActivityMaxRows, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.BriefingRecentActivityRow, 0,
		db2contract.BriefingRecentActivityMaxRows)
	for rows.Next() {
		if len(found) == db2contract.BriefingRecentActivityMaxRows {
			break
		}
		var session, summary, reference, createdAt *string
		if err := rows.Scan(&session, &summary, &reference, &createdAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.BriefingRecentActivityRow{
			SessionID:         text(session),
			ActivitySummary:   text(summary),
			ReferenceTime:     text(reference),
			ActivityCreatedAt: text(createdAt),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeBriefingRecentActivityReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryKeyFactsProvenanceBody = `SELECT m.id, m.key, m.content,
 (SELECT MAX(p.created_at) FROM memory_provenance p
  WHERE p.memory_id = m.id AND p.action = 'supersede') AS supersede_date,
 (SELECT p.action || ' during ' || p.session_id FROM memory_provenance p
  WHERE p.memory_id = m.id ORDER BY p.created_at DESC LIMIT 1) AS provenance
 FROM memories m
 WHERE m.tier IN ('L2', 'L3') AND (m.kind = 'fact' OR m.kind = 'preference')`

// memoryKeyFactsProvenance lists key facts with where they came from.
//
// Both provenance columns are correlated subqueries and both may answer NULL: a
// fact that has never been superseded has no supersede date, and one written
// before provenance was recorded has no last action. Neither is a failure, so
// both scan through pointers and spell absence as empty.
func memoryKeyFactsProvenance(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	flags, workspace, project, err := db2contract.DecodeMemoryKeyFactsProvenanceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	statement, scopeArgs := scopedStatement(memoryKeyFactsProvenanceBody, "m.id",
		"m.confidence DESC, m.use_count DESC", scope)

	rows, queryErr := store.Query(ctx, statement,
		scopedArgs(db2contract.MemoryKeyFactsProvenanceMaxRows, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryKeyFactsProvenanceRow, 0,
		db2contract.MemoryKeyFactsProvenanceMaxRows)
	for rows.Next() {
		if len(found) == db2contract.MemoryKeyFactsProvenanceMaxRows {
			break
		}
		var id *int64
		var key, content, supersede, provenance *string
		if err := rows.Scan(&id, &key, &content, &supersede, &provenance); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryKeyFactsProvenanceRow{
			MemoryID:         clampToU64(number(id)),
			MemoryKey:        text(key),
			MemoryContent:    text(content),
			SupersedeDate:    text(supersede),
			MemoryProvenance: text(provenance),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryKeyFactsProvenanceReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
