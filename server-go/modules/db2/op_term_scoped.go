package db2

import (
	"context"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageTermFindInProject,
		db2contract.OperationTermFindInProject, termFindInProject)
	Register(db2contract.StageTermFindExcludingProject,
		db2contract.OperationTermFindExcludingProject, termFindExcludingProject)
}

// The scoped term searches share everything with the unscoped one except the
// project clause and the matching operator, so the four statements are built
// from one shape. The C keeps four copies, and they had already drifted -- one
// of the excluding pair breaks its ORDER BY across two lines and the other does
// not, which is harmless until someone changes only the one they are reading.
const (
	termScopedSelect = `SELECT t.line, t.line_end, p.name, f.path, t.kind
 FROM terms t
 JOIN files f ON f.id = t.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE `
	// The project filter is a parameter rather than a branch: an empty name
	// means every project, which is what the C's (?3 = '' OR p.name = ?3)
	// says. Keeping it in the statement means one prepared plan covers both.
	termScopedInProject = ` AND ($3 = '' OR p.name = $3)`
	termScopedExcluding = ` AND p.name <> $3`
	termScopedTail      = `
 GROUP BY p.name, f.path, t.line, t.kind, t.line_end
 ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END, p.name, f.path
 LIMIT $2`
)

const (
	termFindInProjectQuery = termScopedSelect + `t.name = $1 AND ` +
		codeIndexVisibilityFilter + termScopedInProject + termScopedTail
	termFindInProjectLikeQuery = termScopedSelect + `t.name LIKE $1 ESCAPE '\' AND ` +
		codeIndexVisibilityFilter + termScopedInProject + termScopedTail
	termFindExcludingProjectQuery = termScopedSelect + `t.name = $1 AND ` +
		codeIndexVisibilityFilter + termScopedExcluding + termScopedTail
	termFindExcludingProjectLikeQuery = termScopedSelect +
		`t.name LIKE $1 ESCAPE '\' AND ` + codeIndexVisibilityFilter +
		termScopedExcluding + termScopedTail
)

// likePattern escapes an identifier for use inside a LIKE pattern and wraps it.
//
// The escaping is the point: an identifier containing an underscore is the
// common case here, and unescaped it would match any character. Backslash is
// escaped too, and the statements say ESCAPE '\' so the escape character is the
// one being written.
func likePattern(identifier string, leading bool) string {
	var pattern strings.Builder
	if leading {
		pattern.WriteByte('%')
	}
	for index := 0; index < len(identifier); index++ {
		switch identifier[index] {
		case '%', '_', '\\':
			pattern.WriteByte('\\')
		}
		pattern.WriteByte(identifier[index])
	}
	pattern.WriteByte('%')
	return pattern.String()
}

// findTermsWithFallback runs the exact search, then two LIKE tiers.
//
// Each tier fires only when the one before it found nothing. Prefix first, so
// looking up "aimee_db_t" keeps its precision; then substring, so looking up
// "qdrant" still finds kb_test_qdrant_post_handler. Running the substring tier
// first would bury an exact prefix match under everything that merely contains
// the word.
func findTermsWithFallback(ctx context.Context, store Store,
	exactQuery, likeQuery, identifier, project string, ceiling int,
) ([]db2contract.TermFindRow, error) {
	hits, err := readTermHits(ctx, store, exactQuery, identifier, project, ceiling)
	if err != nil || len(hits) > 0 {
		return hits, err
	}
	for _, leading := range []bool{false, true} {
		hits, err = readTermHits(ctx, store, likeQuery,
			likePattern(identifier, leading), project, ceiling)
		if err != nil || len(hits) > 0 {
			return hits, err
		}
	}
	return hits, nil
}

func readTermHits(ctx context.Context, store Store, query, match, project string,
	ceiling int,
) ([]db2contract.TermFindRow, error) {
	rows, err := store.Query(ctx, query, match, int64(ceiling), project)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanTermHits(rows, ceiling)
}

// termFindInProject answers where an identifier appears, optionally within one
// project.
//
// An empty project means every project, which makes this the same question
// term_find answers -- except that this one falls back to pattern matching when
// the exact name finds nothing, and term_find does not. The two are separate
// operations because their callers want different things: one is a lookup, the
// other is a search.
func termFindInProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, identifier, err := db2contract.DecodeTermFindInProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	hits, queryErr := findTermsWithFallback(ctx, store,
		termFindInProjectQuery, termFindInProjectLikeQuery, identifier, project,
		db2contract.TermFindInProjectMaxRows)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	scoped := make([]db2contract.TermFindInProjectRow, len(hits))
	for index, hit := range hits {
		scoped[index] = db2contract.TermFindInProjectRow(hit)
	}
	reply, encodeErr := db2contract.EncodeTermFindInProjectReply(scoped)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// termFindExcludingProject answers where an identifier appears outside one
// project.
//
// The cross-repository search: what else uses this name. Excluding by name
// rather than by identity, as the caller searches do, because the index carries
// the name.
func termFindExcludingProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	excluded, identifier, err :=
		db2contract.DecodeTermFindExcludingProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	hits, queryErr := findTermsWithFallback(ctx, store,
		termFindExcludingProjectQuery, termFindExcludingProjectLikeQuery,
		identifier, excluded, db2contract.TermFindExcludingProjectMaxRows)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	elsewhere := make([]db2contract.TermFindExcludingProjectRow, len(hits))
	for index, hit := range hits {
		elsewhere[index] = db2contract.TermFindExcludingProjectRow(hit)
	}
	reply, encodeErr := db2contract.EncodeTermFindExcludingProjectReply(elsewhere)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
