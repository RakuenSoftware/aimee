package db2

import (
	"context"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageTopL2Facts,
		db2contract.OperationTopL2Facts, topL2Facts)
	Register(db2contract.StageListSessionScopePriority,
		db2contract.OperationListSessionScopePriority, listSessionScopePriority)
	Register(db2contract.StageCollectAliasMatches,
		db2contract.OperationCollectAliasMatches, collectAliasMatches)
	Register(db2contract.StageCollectEntityMatches,
		db2contract.OperationCollectEntityMatches, collectEntityMatches)
	Register(db2contract.StageCollectEventFrameMatches,
		db2contract.OperationCollectEventFrameMatches, collectEventFrameMatches)
	Register(db2contract.StageCollectRelationTokenMatches,
		db2contract.OperationCollectRelationTokenMatches,
		collectRelationTokenMatches)
	Register(db2contract.StageCollectSummaryMatches,
		db2contract.OperationCollectSummaryMatches, collectSummaryMatches)
	Register(db2contract.StageCollectTemporalMatches,
		db2contract.OperationCollectTemporalMatches, collectTemporalMatches)
	Register(db2contract.StageFindFactsLike,
		db2contract.OperationFindFactsLike, findFactsLike)
	Register(db2contract.StageListSessionScopePriorityLike,
		db2contract.OperationListSessionScopePriorityLike,
		listSessionScopePriorityLike)
	Register(db2contract.StageNegationFtsSearch,
		db2contract.OperationNegationFtsSearch, negationFtsSearch)
}

// Every collector here answers a list of memory identifiers and nothing else.
//
// The C's statements select thirteen columns and the adapter keeps one of them.
// Selecting only the identifier says what the reply carries, and the ordering
// is what the other twelve were for -- so the ORDER BY is reproduced exactly,
// because it decides which identifiers come back and in what sequence.
//
// The tier preference recurs: L3 before L2 before L1 before anything else. It
// is a statement about durability, not recency, and several of these order by
// it after their own relevance test.
const tierPreference = `CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1
   WHEN 'L1' THEN 2 ELSE 3 END`

// readMemoryIDs runs a scoped collector and gathers its identifiers.
func readMemoryIDs(ctx context.Context, store Store, statement string,
	args []any, ceiling int) ([]uint64, bus.ModuleStatus) {
	rows, err := store.Query(ctx, statement, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()
	found := make([]uint64, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var id *int64
		if scanErr := rows.Scan(&id); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, clampToU64(number(id)))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

// collectorReply is the shape every one of these ends with.
func collectorReply(ctx context.Context, store Store, statement string,
	args []any, ceiling int, encode func([]uint64) ([]byte, error)) (
	[]byte, bus.ModuleStatus,
) {
	found, status := readMemoryIDs(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := encode(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// topL2Facts lists the facts most worth surfacing from the working tier.
func topL2Facts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, flags, workspace, project, err :=
		db2contract.DecodeTopL2FactsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("memories.id", 1)
	rank := scope.rankExpression("memories.id", 1)
	statement := `SELECT id FROM memories
 WHERE tier = 'L2' AND kind = 'fact'` + filter +
		` ORDER BY ` + rank + ` DESC, use_count DESC, confidence DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.TopL2FactsMax))
	args := append([]any{int64(ceiling)}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeTopL2FactsReply)
}

// The kind preference the two session-priority reads share: what to do first,
// then what was decided, then everything else.
const sessionKindPreference = `CASE m.kind WHEN 'workflow' THEN 0
   WHEN 'decision' THEN 1 ELSE 2 END`

const sessionTierPreference = `CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1
   ELSE 2 END`

// listSessionScopePriority lists what a session should be told first.
func listSessionScopePriority(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, flags, workspace, project, err :=
		db2contract.DecodeListSessionScopePriorityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 1)
	rank := scope.rankExpression("m.id", 1)
	statement := `SELECT m.id FROM memories m
 WHERE m.tier IN ('L1', 'L2', 'L3')` + filter +
		` ORDER BY ` + rank + ` DESC, ` + sessionKindPreference + `, ` +
		sessionTierPreference + `, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.ListSessionScopePriorityMax))
	args := append([]any{int64(ceiling)}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeListSessionScopePriorityReply)
}

// listSessionScopePriorityLike is the same list narrowed by a pattern.
//
// The pattern is the caller's, wildcards and all, and it is matched against the
// key and the content without case folding -- which is the C's, and differs
// from every other search here. A caller passing a bare word gets nothing
// unless it is the whole column.
func listSessionScopePriorityLike(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pattern, limit, flags, workspace, project, err :=
		db2contract.DecodeListSessionScopePriorityLikeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memories m
 WHERE m.tier IN ('L1', 'L2', 'L3')
   AND (m.key LIKE $2 OR m.content LIKE $2)` + filter +
		` ORDER BY ` + rank + ` DESC, ` + sessionKindPreference + `, ` +
		sessionTierPreference + `, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit,
		int(db2contract.ListSessionScopePriorityLikeMax))
	args := append([]any{int64(ceiling), pattern}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeListSessionScopePriorityLikeReply)
}

func collectAliasMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	alias, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectAliasMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_aliases a
 JOIN memories m ON m.id = a.memory_id
 WHERE (a.alias = $2 OR a.alias LIKE $2 || '%' OR a.alias LIKE '%' || $2 || '%')` +
		filter + ` ORDER BY ` + rank +
		` DESC, CASE WHEN a.alias = $2 THEN 0
     WHEN a.alias LIKE $2 || '%' THEN 1 ELSE 2 END,
   a.weight DESC, ` + tierPreference +
		`, m.use_count DESC, m.confidence DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.CollectAliasMatchesMax))
	args := append([]any{int64(ceiling), alias}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectAliasMatchesReply)
}

func collectEntityMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectEntityMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_entities e
 JOIN memories m ON m.id = e.memory_id
 WHERE (e.entity = $2 OR e.entity LIKE $2 || '%' OR e.entity LIKE '%' || $2 || '%')` +
		filter + ` ORDER BY ` + rank +
		` DESC, CASE WHEN e.entity = $2 THEN 0
     WHEN e.entity LIKE $2 || '%' THEN 1 ELSE 2 END,
   e.weight DESC, ` + tierPreference +
		`, m.use_count DESC, m.confidence DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.CollectEntityMatchesMax))
	args := append([]any{int64(ceiling), entity}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectEntityMatchesReply)
}

// The event frame matches on any of its parts, and only the object is a
// substring test: an actor, a location and a time are named things, and a
// partial match on them is a different event rather than a weaker one.
func collectEventFrameMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	term, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectEventFrameMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_event_frames e
 JOIN memories m ON m.id = e.memory_id
 WHERE (e.actor = $2 OR e.action = $2 OR e.object LIKE '%' || $2 || '%'
        OR e.location = $2 OR e.event_time = $2)` + filter +
		` ORDER BY ` + rank + ` DESC, m.confidence DESC, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.CollectEventFrameMatchesMax))
	args := append([]any{int64(ceiling), term}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectEventFrameMatchesReply)
}

// The relation collector groups, because one memory can assert several
// relations that all match: without the grouping the same memory comes back
// once per relation and fills the reply with itself. The strongest of its
// matching relations is what orders it.
func collectRelationTokenMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	token, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectRelationTokenMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_relations r
 JOIN memories m ON m.id = r.memory_id
 WHERE r.memory_id > 0
   AND (LOWER(r.src_entity) = LOWER($2) OR LOWER(r.dst_entity) = LOWER($2)
        OR LOWER(r.relation) = LOWER($2)
        OR LOWER(r.fact_text) LIKE '%' || LOWER($2) || '%')` + filter +
		` GROUP BY m.id, m.confidence, m.use_count
 ORDER BY ` + rank + ` DESC, MAX(r.weight) DESC, m.confidence DESC,
   m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit,
		int(db2contract.CollectRelationTokenMatchesMax))
	args := append([]any{int64(ceiling), token}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectRelationTokenMatchesReply)
}

func collectSummaryMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	term, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectSummaryMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_summaries s
 JOIN memories m ON m.id = s.memory_id
 WHERE LOWER(s.summary) LIKE '%' || LOWER($2) || '%'` + filter +
		` ORDER BY ` + rank +
		` DESC, CASE WHEN LOWER(s.summary) LIKE LOWER($2) || '%' THEN 0 ELSE 1 END,
   m.confidence DESC, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.CollectSummaryMatchesMax))
	args := append([]any{int64(ceiling), term}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectSummaryMatchesReply)
}

// The temporal collector matches a reference key exactly or as a prefix, and
// never anywhere within: "2026-08" should find August, not every key that
// happens to contain those characters somewhere.
func collectTemporalMatches(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	refKey, limit, flags, workspace, project, err :=
		db2contract.DecodeCollectTemporalMatchesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memory_temporal_refs t
 JOIN memories m ON m.id = t.memory_id
 WHERE (t.ref_key = $2 OR t.ref_key LIKE $2 || '%')` + filter +
		` ORDER BY ` + rank + ` DESC, CASE WHEN t.ref_key = $2 THEN 0 ELSE 1 END,
   t.weight DESC, m.confidence DESC, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.CollectTemporalMatchesMax))
	args := append([]any{int64(ceiling), refKey}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeCollectTemporalMatchesReply)
}

// findFactsLike searches the three places a fact's text lives, and ranks an
// exact match on any of them above a prefix, above a mention.
func findFactsLike(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	term, limit, flags, workspace, project, err :=
		db2contract.DecodeFindFactsLikeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memories m
 WHERE (LOWER(m.key) LIKE '%' || LOWER($2) || '%'
        OR LOWER(m.content) LIKE '%' || LOWER($2) || '%'
        OR LOWER(COALESCE(m.use_cases, '')) LIKE '%' || LOWER($2) || '%')` +
		filter + ` ORDER BY ` + rank + ` DESC,
   CASE WHEN LOWER(m.key) = LOWER($2) THEN 0
        WHEN LOWER(m.content) = LOWER($2) THEN 1
        WHEN LOWER(COALESCE(m.use_cases, '')) = LOWER($2) THEN 2
        WHEN LOWER(m.key) LIKE LOWER($2) || '%' THEN 3
        ELSE 4 END, ` + tierPreference +
		`, m.use_count DESC, m.confidence DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.FindFactsLikeMax))
	args := append([]any{int64(ceiling), term}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeFindFactsLikeReply)
}

// negationFtsSearch finds memories that negate any of the given concepts.
//
// The tokens are OR-ed rather than AND-ed, which is the C's decision and a
// recall one: a memory sharing any negated concept is worth surfacing, where
// requiring all of them would answer only on a superset.
//
// websearch_to_tsquery rather than to_tsquery because it never rejects its
// input. The tokens arrive from an extractor, not from a person, and a query
// language error in that path would take out the search rather than narrow it.
func negationFtsSearch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	tokens, limit, flags, workspace, project, err :=
		db2contract.DecodeNegationFtsSearchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	query := strings.Join(strings.Fields(tokens), " or ")
	if query == "" {
		// No tokens is no question, and the C answers an empty list rather
		// than searching for nothing.
		return collectorReply(ctx, store, `SELECT 0 WHERE false`, nil, 0,
			db2contract.EncodeNegationFtsSearchReply)
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memories m
 WHERE m.memory_negation_fts_tsv @@ websearch_to_tsquery('simple', $2)` +
		filter + ` ORDER BY ` + rank + ` DESC,
   ts_rank(m.memory_negation_fts_tsv, websearch_to_tsquery('simple', $2)) DESC,
   m.use_count DESC, m.confidence DESC LIMIT $1`
	ceiling := relationCeiling(limit, int(db2contract.NegationFtsSearchMax))
	args := append([]any{int64(ceiling), query}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeNegationFtsSearchReply)
}
