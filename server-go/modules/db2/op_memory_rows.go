package db2

import (
	"context"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageSessionNeighborsBefore,
		db2contract.OperationSessionNeighborsBefore, sessionNeighborsBefore)
	Register(db2contract.StageSessionNeighborsAfter,
		db2contract.OperationSessionNeighborsAfter, sessionNeighborsAfter)
	Register(db2contract.StageRowGet, db2contract.OperationRowGet, rowGet)
	Register(db2contract.StageRowGetByUnitID,
		db2contract.OperationRowGetByUnitID, rowGetByUnitID)
	Register(db2contract.StageSearchFactsPatternsByKeyword,
		db2contract.OperationSearchFactsPatternsByKeyword,
		searchFactsPatternsByKeyword)
	Register(db2contract.StageFactHistory,
		db2contract.OperationFactHistory, factHistory)
	Register(db2contract.StageListRows,
		db2contract.OperationListRows, listRows)
	Register(db2contract.StageAggregate,
		db2contract.OperationAggregate, aggregate)
	Register(db2contract.StageLoadEvalCorpus,
		db2contract.OperationLoadEvalCorpus, loadEvalCorpus)
}

// The neighbours read walks the session by identifier, which is the order rows
// were written in. There is no stamp precise enough to order a session's own
// memories -- they land within the same second -- so the sequence is the
// identifier's.
//
// The "before" read excludes zero explicitly, as the C does. It costs nothing
// and says that the identifier space starts at one, so a caller passing zero
// asking for what came before it gets nothing rather than everything.
const (
	sessionNeighborsBeforeQuery = `SELECT id FROM memories
 WHERE source_session = $1 AND id < $2 AND id > 0
 ORDER BY id DESC LIMIT $3`

	sessionNeighborsAfterQuery = `SELECT id FROM memories
 WHERE source_session = $1 AND id > $2
 ORDER BY id ASC LIMIT $3`
)

func sessionNeighborsBefore(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	session, anchor, limit, err :=
		db2contract.DecodeSessionNeighborsBeforeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := relationCeiling(limit, int(db2contract.SessionNeighborsBeforeMax))
	return collectorReply(ctx, store, sessionNeighborsBeforeQuery,
		[]any{session, int64(anchor), int64(ceiling)}, ceiling,
		db2contract.EncodeSessionNeighborsBeforeReply)
}

func sessionNeighborsAfter(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	session, anchor, limit, err :=
		db2contract.DecodeSessionNeighborsAfterRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := relationCeiling(limit, int(db2contract.SessionNeighborsAfterMax))
	return collectorReply(ctx, store, sessionNeighborsAfterQuery,
		[]any{session, int64(anchor), int64(ceiling)}, ceiling,
		db2contract.EncodeSessionNeighborsAfterReply)
}

// The full memory row, in the order the reply declares it.
//
// use_cases is selected here and not by the unit-id read, which is the C's
// difference between the two and not an oversight worth correcting: the
// unit-keyed read answers about a unit's parent and carries the eleven columns
// a unit's caller needs.
const (
	rowGetQuery = `SELECT id, tier, kind, key, content, confidence, use_count,
 last_used_at, created_at, updated_at, source_session, salience,
 provenance_category, COALESCE(use_cases, '')
 FROM memories WHERE id = $1`

	rowGetByUnitIDQuery = `SELECT m.id, m.tier, m.kind, m.key, m.content,
 m.confidence, m.use_count, m.last_used_at, m.created_at, m.updated_at,
 m.source_session, m.salience, m.provenance_category, COALESCE(m.use_cases, '')
 FROM memory_units u JOIN memories m ON m.id = u.memory_id
 WHERE u.id = $1`
)

func rowGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeRowGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return memoryRowReply(ctx, store, rowGetQuery,
		db2contract.EncodeRowGetReply, int64(memoryID))
}

func rowGetByUnitID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	unitID, err := db2contract.DecodeRowGetByUnitIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return memoryRowReply(ctx, store, rowGetByUnitIDQuery,
		db2contract.EncodeRowGetByUnitIDReply, int64(unitID))
}

// memoryRowReply reads one memory row and answers it, or says it is not there.
func memoryRowReply(ctx context.Context, store Store, query string,
	encode func(uint32, *db2contract.MemoryRow) ([]byte, error), args ...any) (
	[]byte, bus.ModuleStatus,
) {
	var row db2contract.MemoryRow
	var id, useCount *int64
	var confidence, salience *float64
	var tier, kind, key, content, lastUsed, created, updated *string
	var session, provenance, useCases *string
	switch err := store.QueryRow(ctx, query, args...).Scan(&id, &tier, &kind,
		&key, &content, &confidence, &useCount, &lastUsed, &created, &updated,
		&session, &salience, &provenance, &useCases); {
	case errors.Is(err, pgx.ErrNoRows):
		reply, encodeErr := encode(db2contract.ResultNotFound, nil)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	case err != nil:
		return nil, bus.ModuleStatusInternal
	}
	row = db2contract.MemoryRow{
		ID:                 clampToU64(number(id)),
		Confidence:         decimal(confidence),
		Salience:           decimal(salience),
		UseCount:           uint32(max(number(useCount), 0)),
		Tier:               text(tier),
		Kind:               text(kind),
		Key:                text(key),
		Content:            text(content),
		UseCases:           text(useCases),
		LastUsedAt:         text(lastUsed),
		CreatedAt:          text(created),
		UpdatedAt:          text(updated),
		SourceSession:      text(session),
		ProvenanceCategory: text(provenance),
	}
	reply, encodeErr := encode(db2contract.ResultOK, &row)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// searchFactsPatternsByKeyword searches the durable tiers for facts and
// patterns mentioning a keyword.
//
// L5 is in the tier list beside L2 and L3, and L1 is not: this read answers
// "what does the system know", and L1 is where things are still being decided.
func searchFactsPatternsByKeyword(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	keyword, limit, flags, workspace, project, err :=
		db2contract.DecodeSearchFactsPatternsByKeywordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("m.id", 2)
	rank := scope.rankExpression("m.id", 2)
	statement := `SELECT m.id FROM memories m
 WHERE m.tier IN ('L2', 'L3', 'L5') AND m.kind IN ('fact', 'pattern')
   AND (LOWER(m.content) LIKE '%' || LOWER($2) || '%'
        OR LOWER(m.key) LIKE '%' || LOWER($2) || '%')` + filter +
		` ORDER BY ` + rank + ` DESC, m.confidence DESC, m.use_count DESC LIMIT $1`
	ceiling := relationCeiling(limit,
		int(db2contract.SearchFactsPatternsByKeywordMax))
	args := append([]any{int64(ceiling), keyword}, scopeArgs...)
	return collectorReply(ctx, store, statement, args, ceiling,
		db2contract.EncodeSearchFactsPatternsByKeywordReply)
}

// factHistory lists every version of a fact, newest first.
//
// The versions are the key plus a "#v" suffix, which is how the schema records
// supersession: the current fact holds the bare key and its predecessors hold
// the numbered ones. Matching both is what makes this a history rather than a
// lookup.
const factHistoryQuery = `SELECT id FROM memories
 WHERE key = $1 OR key LIKE $2
 ORDER BY created_at DESC LIMIT $3`

func factHistory(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	key, limit, err := db2contract.DecodeFactHistoryRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := relationCeiling(limit, int(db2contract.FactHistoryMax))
	return collectorReply(ctx, store, factHistoryQuery,
		[]any{key, key + "#v%", int64(ceiling)}, ceiling,
		db2contract.EncodeFactHistoryReply)
}

// listRows lists memories under optional tier and kind filters.
//
// The filters are built into the statement rather than expressed as
// "$2 = ” OR tier = $2", which is what the C does and is worth keeping: a
// caller filtering on nothing gets a statement with no predicate on the column,
// and the planner is free to use whichever index the remaining predicates suit.
func listRows(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, flags, hideArchived, tier, kind, workspace, project, err :=
		db2contract.DecodeListRowsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var predicates strings.Builder
	args := []any{}
	if hideArchived != 0 {
		predicates.WriteString(` AND lifecycle_state <> 'archived'`)
	}
	if tier != "" {
		args = append(args, tier)
		predicates.WriteString(` AND tier = $` + itoa(uint32(len(args))))
	}
	if kind != "" {
		args = append(args, kind)
		predicates.WriteString(` AND kind = $` + itoa(uint32(len(args))))
	}
	scope := DecodeScope(flags, workspace, project)
	filter, scopeArgs := scope.filter("memories.id", len(args))
	rank := scope.rankExpression("memories.id", len(args))
	ceiling := relationCeiling(limit, int(db2contract.ListRowsMax))
	statement := `SELECT id FROM memories WHERE 1=1` + predicates.String() +
		filter + ` ORDER BY ` + rank + ` DESC, updated_at DESC LIMIT ` +
		itoa(uint32(ceiling))
	return collectorReply(ctx, store, statement, append(args, scopeArgs...),
		ceiling, db2contract.EncodeListRowsReply)
}

// The three shapes aggregate takes, by what the caller gave it.
//
// An entity and a term join to the entity table; a term alone does not, because
// the join exists only to match the entity exactly. With neither, the read is
// everything that is not scratch -- which is the caller asking for the corpus
// rather than for a search.
const (
	aggregateEntityQuery = `SELECT DISTINCT m.id,
   COALESCE(m.last_used_at, m.updated_at) AS touched
 FROM memories m
 LEFT JOIN memory_entities me ON me.memory_id = m.id
 WHERE (me.entity = $1 OR LOWER(m.content) LIKE $2 OR LOWER(m.key) LIKE $2)
 ORDER BY touched DESC, m.id DESC LIMIT $3`

	aggregateTermQuery = `SELECT m.id,
   COALESCE(m.last_used_at, m.updated_at) AS touched
 FROM memories m
 WHERE LOWER(m.content) LIKE $1 OR LOWER(m.key) LIKE $1
 ORDER BY touched DESC, m.id DESC LIMIT $2`

	aggregateCorpusQuery = `SELECT m.id,
   COALESCE(m.last_used_at, m.updated_at) AS touched
 FROM memories m
 WHERE m.kind <> 'scratch'
 ORDER BY touched DESC, m.id DESC LIMIT $1`
)

// aggregate answers the memories most recently touched, optionally about an
// entity or matching a term.
//
// The truncated flag says the reply is a page rather than the answer: the
// caller asked for a survey and got the ceiling, so there is more behind it.
func aggregate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, term, limit, err := db2contract.DecodeAggregateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := relationCeiling(limit, int(db2contract.AggregateMax))
	pattern := "%" + strings.ToLower(term) + "%"
	var statement string
	var args []any
	switch {
	case entity != "":
		statement, args = aggregateEntityQuery,
			[]any{entity, pattern, int64(ceiling)}
	case term != "":
		statement, args = aggregateTermQuery, []any{pattern, int64(ceiling)}
	default:
		statement, args = aggregateCorpusQuery, []any{int64(ceiling)}
	}
	found, status := readAggregateIDs(ctx, store, statement, args, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	truncated := uint32(0)
	if len(found) == ceiling {
		truncated = 1
	}
	reply, encodeErr := db2contract.EncodeAggregateReply(truncated, found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// readAggregateIDs reads the identifier and discards the ordering column, which
// the statement needs in its select list because it orders by it and, for the
// entity shape, distinguishes on it.
func readAggregateIDs(ctx context.Context, store Store, statement string,
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
		var touched *string
		if scanErr := rows.Scan(&id, &touched); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, clampToU64(number(id)))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

// The three corpora an evaluation falls back through, narrowest first.
//
// The label is part of the reply because the caller needs to know which of them
// answered: a score over "L2 facts" and a score over "durable memories" are not
// comparable, and without the label they look identical.
var evalCorpusPlans = []struct {
	label string
	query string
}{
	{"L2 facts", `SELECT id FROM memories
 WHERE tier = 'L2' AND kind = 'fact' LIMIT 100`},
	{"facts", `SELECT id FROM memories
 WHERE kind = 'fact' AND tier IN ('L1', 'L2', 'L3') LIMIT 100`},
	{"durable memories", `SELECT id FROM memories
 WHERE tier IN ('L1', 'L2', 'L3') AND kind NOT IN ('scratch') LIMIT 100`},
}

// loadEvalCorpus answers the narrowest corpus that has anything in it.
func loadEvalCorpus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	limit, err := db2contract.DecodeLoadEvalCorpusRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := relationCeiling(limit, int(db2contract.LoadEvalCorpusMax))
	for _, plan := range evalCorpusPlans {
		found, status := readMemoryIDs(ctx, store, plan.query, nil, ceiling)
		if status != bus.ModuleStatusOK {
			// A plan that will not run is skipped rather than fatal, which is
			// the C's behaviour: the next corpus is wider and may still answer.
			continue
		}
		if len(found) == 0 {
			continue
		}
		reply, encodeErr := db2contract.EncodeLoadEvalCorpusReply(plan.label, found)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	reply, encodeErr := db2contract.EncodeLoadEvalCorpusReply("", nil)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
