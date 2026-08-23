package db2

import (
	"context"

	"github.com/jackc/pgx/v5"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCallersFind,
		db2contract.OperationCallersFind, callersFind)
	Register(db2contract.StageCallersFindScoped,
		db2contract.OperationCallersFindScoped, callersFindScoped)
	Register(db2contract.StageCallersFindExcludingProject,
		db2contract.OperationCallersFindExcludingProject, callersFindExcludingProject)
	Register(db2contract.StageFileDefinitions,
		db2contract.OperationFileDefinitions, fileDefinitions)
	Register(db2contract.StageTermFind,
		db2contract.OperationTermFind, termFind)
}

// Every read here filters the same four ways, and each one is load-bearing.
//
// The generation pair -- a current project and a file from its current
// generation -- keeps a rescan's superseded rows out of the answer. A file that
// was deleted and reindexed still has its old rows, and reporting a caller at a
// line that no longer exists sends someone to the wrong place.
//
// The three path filters keep hidden files out: a dotted path segment anywhere,
// or a project rooted inside one. The C's note says why they are here rather
// than only in the scanner -- rows written before the scanner grew its own
// guard are still in the table, and a read API is where they would surface.
const codeIndexVisibilityFilter = `p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
   AND f.path NOT LIKE '.%'
   AND f.path NOT LIKE '%/.%'
   AND p.root NOT LIKE '%/.%'`

// The three caller reads differ only in what they say about the project: all
// of them, one of them, or all but one. Everything else -- the joins, the
// visibility filter, the ordering -- is shared, and keeping it in one place is
// what stops the three drifting apart.
const (
	callersSelect = `SELECT cc.line, p.name, f.path, cc.caller
 FROM code_calls cc
 JOIN files f ON f.id = cc.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE cc.callee = $1 AND `
	callersOrder = `
 ORDER BY p.name, f.path, cc.line
 LIMIT `
)

const (
	callersFindAllQuery = callersSelect + codeIndexVisibilityFilter +
		callersOrder + `$2`
	callersFindInProjectQuery = callersSelect + `p.name = $2 AND ` +
		codeIndexVisibilityFilter + callersOrder + `$3`
	callersFindExcludingProjectQuery = callersSelect + `p.name <> $2 AND ` +
		codeIndexVisibilityFilter + callersOrder + `$3`
)

// readCallerRows runs one of the three caller statements and maps its rows.
//
// The reply's field order is not the select's: the line leads the row, and the
// select puts it first for that reason rather than following the C's column
// order, which had the mapper doing the reordering.
func readCallerRows(ctx context.Context, store Store, query string, args ...any) (
	[]db2contract.CallersFindRow, error,
) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	callers := make([]db2contract.CallersFindRow, 0, 16)
	for rows.Next() {
		var line int64
		var project, filePath, caller string
		if scanErr := rows.Scan(&line, &project, &filePath, &caller); scanErr != nil {
			return nil, scanErr
		}
		callers = append(callers, db2contract.CallersFindRow{
			CallerLine:     uint32(line),
			CallerProject:  project,
			CallerFilePath: filePath,
			CallerSymbol:   caller,
		})
	}
	return callers, rows.Err()
}

// callersFind answers who calls a symbol, optionally within one project.
//
// An empty project means every project rather than no project. That is the C's
// reading and it is the useful one: a caller asking "who calls this" without
// naming a repository wants the whole index, and the alternative -- an empty
// answer -- would look like a symbol nobody calls.
func callersFind(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, callee, err := db2contract.DecodeCallersFindRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CallersFindMaxRows

	var callers []db2contract.CallersFindRow
	var queryErr error
	if project == "" {
		callers, queryErr = readCallerRows(ctx, store,
			callersFindAllQuery, callee, int64(ceiling))
	} else {
		callers, queryErr = readCallerRows(ctx, store,
			callersFindInProjectQuery, callee, project, int64(ceiling))
	}
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCallersFindReply(callers)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// callersFindScoped answers who calls a symbol inside one project.
//
// The same question callers_find answers when given a project, reached through
// the canonical index rather than the code index. The two are kept as separate
// operations because their callers are separate; the statement is the same one,
// which is the point -- a scoped search that disagreed with the unscoped one
// filtered by hand would be a bug nobody could see.
func callersFindScoped(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, callee, err := db2contract.DecodeCallersFindScopedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CallersFindScopedMaxRows
	found, queryErr := readCallerRows(ctx, store,
		callersFindInProjectQuery, callee, project, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	scoped := make([]db2contract.CallersFindScopedRow, len(found))
	for index, caller := range found {
		scoped[index] = db2contract.CallersFindScopedRow(caller)
	}
	reply, encodeErr := db2contract.EncodeCallersFindScopedReply(scoped)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// callersFindExcludingProject answers who outside one project calls a symbol.
//
// This is the cross-repository question: a symbol's callers everywhere except
// where it lives. Excluding by name rather than by identity means a project
// renamed between the index and the read is no longer excluded, which is the
// C's behaviour and follows from the column the index carries.
func callersFindExcludingProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	excluded, callee, err :=
		db2contract.DecodeCallersFindExcludingProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CallersFindExcludingProjectMaxRows
	found, queryErr := readCallerRows(ctx, store,
		callersFindExcludingProjectQuery, callee, excluded, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	elsewhere := make([]db2contract.CallersFindExcludingProjectRow, len(found))
	for index, caller := range found {
		elsewhere[index] = db2contract.CallersFindExcludingProjectRow(caller)
	}
	reply, encodeErr := db2contract.EncodeCallersFindExcludingProjectReply(elsewhere)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// One statement where the C makes three: resolve the project, resolve the file,
// then read its terms. The joins say the same thing -- this file, of this
// project, in its current generation -- and an unknown project or file is an
// empty answer either way, which is what the C's early returns produce.
//
// No path filter here, unlike the caller reads. The caller named the file, so
// hiding it after they asked for it by name would be answering a different
// question.
const fileDefinitionsQuery = `SELECT t.name, t.kind, t.line, t.line_end
 FROM terms t
 JOIN files f ON f.id = t.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE p.name = $1 AND f.path = $2
 AND p.lifecycle_state = 'current'
 AND f.generation = p.current_generation
 AND t.kind = 'definition'
 ORDER BY t.line
 LIMIT $3`

// fileDefinitions lists what a file defines, in the order it defines them.
//
// Definitions only. A file's terms include every reference it makes, and a
// caller asking what a file defines wants its outline rather than its
// dependencies.
func fileDefinitions(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err := db2contract.DecodeFileDefinitionsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.FileDefinitionsMaxRows
	rows, queryErr := store.Query(ctx, fileDefinitionsQuery,
		project, filePath, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	definitions := make([]db2contract.FileDefinitionsRow, 0, 16)
	for rows.Next() {
		var name, kind string
		var line, lineEnd int64
		if scanErr := rows.Scan(&name, &kind, &line, &lineEnd); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		definitions = append(definitions, db2contract.FileDefinitionsRow{
			SymbolName: name,
			SymbolKind: kind,
			Line:       uint32(line),
			LineEnd:    uint32(lineEnd),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeFileDefinitionsReply(definitions)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Definitions first, then everything else, and alphabetical within each. A
// caller looking up an identifier wants the place it is defined before the
// places it is used, and the CASE is what puts it there without a second read.
//
// The GROUP BY has no aggregate over it: it is a DISTINCT written the long way,
// collapsing rows that agree on all five columns. Terms can be recorded more
// than once for one line -- a symbol both declared and defined there -- and
// without it the same location comes back twice.
const termFindQuery = `SELECT t.line, t.line_end, p.name, f.path, t.kind
 FROM terms t
 JOIN files f ON f.id = t.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE t.name = $1 AND ` + codeIndexVisibilityFilter + `
 GROUP BY p.name, f.path, t.line, t.kind, t.line_end
 ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END, p.name, f.path
 LIMIT $2`

// termFind answers where an identifier appears.
//
// Exact name only. The C carries a second statement matching with LIKE and
// never prepares it -- nothing reaches it, and porting a branch no caller can
// take would be inventing behaviour rather than moving it.
func termFind(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	identifier, err := db2contract.DecodeTermFindRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.TermFindMaxRows
	rows, queryErr := store.Query(ctx, termFindQuery, identifier, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	hits, scanErr := scanTermHits(rows, ceiling)
	if scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTermFindReply(hits)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func scanTermHits(rows pgx.Rows, ceiling int) ([]db2contract.TermFindRow, error) {
	hits := make([]db2contract.TermFindRow, 0, 16)
	for rows.Next() && len(hits) < ceiling {
		var line, lineEnd int64
		var project, filePath, kind string
		if err := rows.Scan(&line, &lineEnd, &project, &filePath, &kind); err != nil {
			return nil, err
		}
		hits = append(hits, db2contract.TermFindRow{
			Line:        uint32(line),
			LineEnd:     uint32(lineEnd),
			HitProject:  project,
			HitFilePath: filePath,
			TermKind:    kind,
		})
	}
	return hits, rows.Err()
}
