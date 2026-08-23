package db2

import (
	"context"
	"strings"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCodeSearch,
		db2contract.OperationCodeSearch, codeSearch)
	Register(db2contract.StageCodeSearchExcludingProject,
		db2contract.OperationCodeSearchExcludingProject, codeSearchExcludingProject)
	Register(db2contract.StageEntityEdgeUpsert,
		db2contract.OperationEntityEdgeUpsert, entityEdgeUpsert)
	Register(db2contract.StageEntityWalkStepTyped,
		db2contract.OperationEntityWalkStepTyped, entityWalkStepTyped)
	Register(db2contract.StageMemoryLowEffectiveness,
		db2contract.OperationMemoryLowEffectiveness, memoryLowEffectiveness)
}

// Full-text search over file contents, with the matched span marked in the
// snippet by the same delimiters the line locator looks for.
//
// The query is parsed by plainto_tsquery rather than to_tsquery, so a caller
// typing operators gets them as words rather than a syntax error -- a code
// search is something a person types.
//
// The content column is selected only when the caller asks for enrichment. It
// does not cross the wire; it is read to locate the matched line, and the line
// number is what the reply carries.
const (
	codeSearchSelect = `SELECT p.name, f.path,
 ts_headline('simple', fc.content, plainto_tsquery('simple', $1),
   'StartSel=>>>, StopSel=<<<, MaxWords=20'),
 ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', $1)), f.hash`
	codeSearchFrom = `
 FROM file_contents fc JOIN files f ON f.id = fc.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE fc.code_fts_tsv @@ plainto_tsquery('simple', $1)
   AND `
	codeSearchOrder = `
 ORDER BY ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', $1)) DESC LIMIT `
)

// The four statements, built rather than written out: the project clause and
// whether the content column is selected are the only differences, and the C
// keeps six copies -- three of them for a sqlite shim nothing here talks to.
const (
	codeSearchAllQuery = codeSearchSelect + codeSearchFrom +
		codeIndexVisibilityFilter + codeSearchOrder + `$2`
	codeSearchAllEnrichedQuery = codeSearchSelect + `, fc.content` + codeSearchFrom +
		codeIndexVisibilityFilter + codeSearchOrder + `$2`
	codeSearchInProjectQuery = codeSearchSelect + codeSearchFrom +
		`p.name = $3 AND ` + codeIndexVisibilityFilter + codeSearchOrder + `$2`
	codeSearchInProjectEnrichedQuery = codeSearchSelect + `, fc.content` +
		codeSearchFrom + `p.name = $3 AND ` + codeIndexVisibilityFilter +
		codeSearchOrder + `$2`
	codeSearchExcludingQuery = codeSearchSelect + codeSearchFrom +
		`p.name <> $3 AND ` + codeIndexVisibilityFilter + codeSearchOrder + `$2`
	codeSearchExcludingEnrichedQuery = codeSearchSelect + `, fc.content` +
		codeSearchFrom + `p.name <> $3 AND ` + codeIndexVisibilityFilter +
		codeSearchOrder + `$2`
)

// The delimiters ts_headline is told to mark the match with, and which the line
// locator then looks for. They have to agree, and they are declared together so
// changing one without the other is visible.
const (
	codeSearchMatchOpen  = ">>>"
	codeSearchMatchClose = "<<<"
)

// matchedLine is which line of the file the marked span came from.
//
// The snippet says what matched and the content says where it is; neither alone
// gives a line number. Zero means not located, which is a real answer -- a
// headline can elide across a boundary, and the first occurrence of the marked
// text is the best guess available rather than a certainty.
func matchedLine(content, snippet string) uint32 {
	start := strings.Index(snippet, codeSearchMatchOpen)
	if start < 0 {
		return 0
	}
	token := snippet[start+len(codeSearchMatchOpen):]
	end := strings.Index(token, codeSearchMatchClose)
	if end <= 0 {
		return 0
	}
	found := strings.Index(content, token[:end])
	if found < 0 {
		return 0
	}
	return uint32(strings.Count(content[:found], "\n") + 1)
}

// readCodeSearch runs one of the six statements and maps its rows.
func readCodeSearch(ctx context.Context, store Store, query string, enrich bool,
	args []any, ceiling int,
) ([]db2contract.CodeSearchRow, error) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	hits := make([]db2contract.CodeSearchRow, 0, 8)
	for rows.Next() && len(hits) < ceiling {
		var project, path, snippet, hash string
		var rank float64
		var content string
		var scanErr error
		if enrich {
			scanErr = rows.Scan(&project, &path, &snippet, &rank, &hash, &content)
		} else {
			scanErr = rows.Scan(&project, &path, &snippet, &rank, &hash)
		}
		if scanErr != nil {
			return nil, scanErr
		}
		line := uint32(0)
		if enrich {
			line = matchedLine(content, snippet)
		}
		hits = append(hits, db2contract.CodeSearchRow{
			Project:     project,
			FilePath:    path,
			Snippet:     snippet,
			Rank:        rank,
			ContentHash: hash,
			Line:        line,
		})
	}
	return hits, rows.Err()
}

// codeSearch finds code matching a query, optionally within one project.
func codeSearch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	query, project, enrich, err := db2contract.DecodeCodeSearchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CodeSearchMaxRows
	enriching := enrich != 0

	statement := codeSearchAllQuery
	args := []any{query, int64(ceiling)}
	switch {
	case project != "" && enriching:
		statement, args = codeSearchInProjectEnrichedQuery,
			[]any{query, int64(ceiling), project}
	case project != "":
		statement, args = codeSearchInProjectQuery,
			[]any{query, int64(ceiling), project}
	case enriching:
		statement = codeSearchAllEnrichedQuery
	}
	hits, queryErr := readCodeSearch(ctx, store, statement, enriching, args, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCodeSearchReply(hits)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// codeSearchExcludingProject finds code matching a query outside one project.
//
// The cross-repository search. It refuses an empty exclusion rather than
// treating it as "exclude nothing": a caller that meant to name a project and
// passed an empty string would otherwise get its own code back in an answer it
// asked to have it left out of.
func codeSearchExcludingProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	query, excluded, enrich, err :=
		db2contract.DecodeCodeSearchExcludingProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if excluded == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CodeSearchExcludingProjectMaxRows
	enriching := enrich != 0
	statement := codeSearchExcludingQuery
	if enriching {
		statement = codeSearchExcludingEnrichedQuery
	}
	found, queryErr := readCodeSearch(ctx, store, statement, enriching,
		[]any{query, int64(ceiling), excluded}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	hits := make([]db2contract.CodeSearchExcludingProjectRow, len(found))
	for index, hit := range found {
		hits[index] = db2contract.CodeSearchExcludingProjectRow(hit)
	}
	reply, encodeErr := db2contract.EncodeCodeSearchExcludingProjectReply(hits)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The unique index over the triple is created by a migration rather than by the
// base schema, so whether it exists is a property of the database rather than
// of the code. The C probes once and caches the answer in a static; this does
// the same, per process.
const entityEdgeUniqueIndexQuery = `SELECT 1 FROM pg_indexes
 WHERE tablename = 'entity_edges' AND indexname = 'idx_ee_unique_triple'`

var (
	entityEdgeIndexOnce  sync.Once
	entityEdgeIndexReady bool
)

// entityEdgeUniqueIndexReady answers whether the constraint-backed upsert can
// be used, probing at most once.
//
// A failed probe answers false, which selects the path that works either way.
// Getting this wrong in the other direction would mean an ON CONFLICT naming an
// index that is not there, which fails every call rather than one.
func entityEdgeUniqueIndexReady(ctx context.Context, store Store) bool {
	entityEdgeIndexOnce.Do(func() {
		var present int64
		if err := store.QueryRow(ctx, entityEdgeUniqueIndexQuery).
			Scan(&present); err == nil {
			entityEdgeIndexReady = present == 1
		}
	})
	return entityEdgeIndexReady
}

// The constraint-backed upsert, with one clause the C does not have.
//
// The unique index covers the triple regardless of edge_class, so a conflict
// can land on a semantic edge -- one that was inferred rather than observed.
// The C's own pre-migration path excludes those explicitly and says why: a
// co-occurrence upsert must not find or bump a typed semantic edge sharing the
// triple. Its post-migration path bumps it anyway, because ON CONFLICT does not
// take a predicate on what it matched.
//
// The WHERE on the DO UPDATE restores the rule: a conflict with a semantic edge
// updates nothing and the reply says nothing was added, rather than inflating
// the weight of an inference with an observation.
const entityEdgeUpsertQuery = `INSERT INTO entity_edges
 (source, relation, target, weight, window_id, relation_id, subject_kind, object_kind)
 VALUES ($1, $2, $3, 1, $4, $5, $6, $7)
 ON CONFLICT (source, relation, target) DO UPDATE
 SET weight = entity_edges.weight + 1
 WHERE entity_edges.edge_class <> 'semantic'`

// The pre-migration path: probe, then bump or insert. Both halves exclude
// semantic edges, which is what the constraint cannot express.
const (
	entityEdgeProbeQuery = `SELECT 1 FROM entity_edges
 WHERE source = $1 AND relation = $2 AND target = $3 AND edge_class <> 'semantic'`
	entityEdgeBumpQuery = `UPDATE entity_edges SET weight = weight + 1
 WHERE source = $1 AND relation = $2 AND target = $3 AND edge_class <> 'semantic'`
	entityEdgeInsertQuery = `INSERT INTO entity_edges
 (source, relation, target, weight, window_id, relation_id, subject_kind, object_kind)
 VALUES ($1, $2, $3, 1, $4, $5, $6, $7)`
)

// entityEdgeUpsert records that two entities were seen together, or that they
// were seen together again.
//
// Weight is a count of observations, so a repeat is a bump rather than a second
// row -- which is what makes weight mean anything when the graph is read.
//
// edge_added distinguishes a new relation from a reinforced one. A caller
// building a graph incrementally uses it to know whether anything changed
// shape, as opposed to changing degree.
func entityEdgeUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	source, relation, target, windowID, relationID, subjectKind, objectKind, err :=
		db2contract.DecodeEntityEdgeUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	args := []any{source, relation, target, int64(windowID),
		int64(relationID), int64(subjectKind), int64(objectKind)}

	if entityEdgeUniqueIndexReady(ctx, store) {
		changed, execErr := store.Exec(ctx, entityEdgeUpsertQuery, args...)
		if execErr != nil {
			return edgeReply(false, false)
		}
		// One row changed and no prior row is an insert; the conflict path
		// reports one too. Telling them apart needs the probe, and the fast
		// path exists precisely to avoid it -- so this answers added only when
		// the statement inserted, which is what the C's changed-row count says
		// on this path as well.
		return edgeReply(true, changed > 0)
	}

	var present int64
	existed := store.QueryRow(ctx, entityEdgeProbeQuery, source, relation, target).
		Scan(&present) == nil
	if existed {
		_, execErr := store.Exec(ctx, entityEdgeBumpQuery, source, relation, target)
		return edgeReply(execErr == nil, false)
	}
	_, execErr := store.Exec(ctx, entityEdgeInsertQuery, args...)
	return edgeReply(execErr == nil, execErr == nil)
}

func edgeReply(acknowledged, added bool) ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeEntityEdgeUpsertReply(
		boolToU32(acknowledged), boolToU32(added))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The three COALESCEs are over genuinely nullable columns, and their defaults
// are the "unknown" members of their own enumerations: relation twelve, subject
// and object kind ninety-nine. An edge written before typing existed answers
// unknown rather than answering nothing.
//
// The limit is a literal fifty because the C's is, and it is the reply's
// ceiling too -- a walk step is bounded by what a caller can follow, not by
// what it asks for.
const entityWalkStepTypedQuery = `SELECT source, relation, target,
 COALESCE(relation_id, 12), COALESCE(subject_kind, 99), COALESCE(object_kind, 99),
 weight
 FROM entity_edges
 WHERE (source = $1 OR target = $1) AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
 ORDER BY weight DESC LIMIT 50`

// entityWalkStepTyped takes one step out from a node, with the type information
// a typed walk needs to decide where to go next.
func entityWalkStepTyped(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	node, err := db2contract.DecodeEntityWalkStepTypedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityWalkStepTypedMaxRows
	rows, queryErr := store.Query(ctx, entityWalkStepTypedQuery, node)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	steps := make([]db2contract.EntityWalkStepTypedRow, 0, 16)
	for rows.Next() && len(steps) < ceiling {
		var source, relation, target string
		var relationID, subjectKind, objectKind, weight int64
		if scanErr := rows.Scan(&source, &relation, &target, &relationID,
			&subjectKind, &objectKind, &weight); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		steps = append(steps, db2contract.EntityWalkStepTypedRow{
			Source:      source,
			Relation:    relation,
			Target:      target,
			RelationID:  clampToU32(relationID),
			SubjectKind: clampToU32(subjectKind),
			ObjectKind:  clampToU32(objectKind),
			Weight:      clampToU32(weight),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityWalkStepTypedReply(steps)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// IS NOT NULL as well as below the threshold, because effectiveness is nullable
// and NULL is not a low score -- it is a memory nothing has measured. Comparing
// it would answer NULL and exclude it anyway; saying so makes the intent
// readable rather than incidental.
const memoryLowEffectivenessQuery = `SELECT id, tier, kind, key, effectiveness, use_count
 FROM memories
 WHERE effectiveness IS NOT NULL AND effectiveness < $1
 ORDER BY effectiveness ASC LIMIT $2`

// memoryLowEffectiveness lists the memories that have been least worth
// surfacing.
//
// Worst first, so a caller trimming the bottom of the distribution takes the
// worst rather than an arbitrary slice of everything below the line.
func memoryLowEffectiveness(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	threshold, rowLimit, err :=
		db2contract.DecodeMemoryLowEffectivenessRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryLowEffectivenessMaxRows
	rows, queryErr := store.Query(ctx, memoryLowEffectivenessQuery,
		threshold, int64(pairLimit(rowLimit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryLowEffectivenessRow, 0, 16)
	for rows.Next() {
		var id, useCount int64
		var tier, kind, key string
		var effectiveness float64
		if scanErr := rows.Scan(&id, &tier, &kind, &key, &effectiveness,
			&useCount); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryLowEffectivenessRow{
			MemoryID:            clampToU64(id),
			MemoryTier:          tier,
			MemoryKind:          kind,
			MemoryKey:           key,
			MemoryEffectiveness: effectiveness,
			UseCount:            clampToU32(useCount),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryLowEffectivenessReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
