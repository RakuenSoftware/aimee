package db2

import (
	"context"
	"strconv"
	"strings"
	"unicode"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageDirectiveGet,
		db2contract.OperationDirectiveGet, directiveGet)
	Register(db2contract.StageDirectiveList,
		db2contract.OperationDirectiveList, directiveList)
	Register(db2contract.StageDirectiveByEntity,
		db2contract.OperationDirectiveByEntity, directiveByEntity)
	Register(db2contract.StageDirectiveByFile,
		db2contract.OperationDirectiveByFile, directiveByFile)
	Register(db2contract.StageDirectiveByLexical,
		db2contract.OperationDirectiveByLexical, directiveByLexical)
}

// The nineteen columns every directive read returns, and the ordering the four
// list reads share.
//
// Priority first, then recency: a directive is a question the system wants
// answered, and the one it most wants answered should lead however long it has
// been waiting. The identifier breaks the remaining ties.
const (
	directiveColumns = `SELECT id, question, topic, anchor_entity, anchor_file,
 cause, priority, state, memory_a_id, memory_b_id, resolution_memory_id,
 evidence, source_session, surfaced_count, last_surfaced_at, resolved_at,
 valid_until, created_at, updated_at
 FROM epistemic_directives`
	directiveOrder = ` ORDER BY priority DESC, created_at DESC, id DESC LIMIT `
)

// The C writes four statements for the list -- state, cause, both, neither --
// and three more for the anchored lookups. The predicates say the same thing
// once: an empty filter admits everything.
const (
	directiveGetQuery  = directiveColumns + ` WHERE id = $1`
	directiveListQuery = directiveColumns +
		` WHERE ($2 = '' OR state = $2) AND ($3 = '' OR cause = $3)` +
		directiveOrder + `$1`
	directiveByEntityQuery = directiveColumns +
		` WHERE state = 'open' AND LOWER(anchor_entity) = $2` +
		directiveOrder + `$1`
	directiveByFileQuery = directiveColumns +
		` WHERE state = 'open' AND anchor_file = $2` + directiveOrder + `$1`
)

// directiveLexicalMaxTokens bounds how many terms a lexical clause contributes,
// as the C's does. A clause with a hundred words would otherwise build a
// hundred-way OR of unanchored LIKEs.
const directiveLexicalMaxTokens = 16

// directiveLexicalMinTokenLength is the shortest term worth matching on.
const directiveLexicalMinTokenLength = 2

// lexicalDirectiveQuery builds the statement and its arguments for a lexical
// clause.
//
// The clause arrives as the query builder wrote it -- quoted words joined by
// OR -- so the words are parsed out, lowercased, and the connector dropped.
// Each becomes an unanchored LIKE against the question and topic together,
// which is why they are concatenated: a directive matches when the words appear
// in either.
//
// Nothing survives means no statement. A clause of only connectors would
// otherwise build "WHERE state = 'open' AND ()", which does not parse.
func lexicalDirectiveQuery(clause string, limit, ceiling int) (string, []any) {
	var patterns []any
	seen := map[string]bool{}
	for _, field := range strings.FieldsFunc(clause, func(r rune) bool {
		return !unicode.IsLetter(r) && !unicode.IsDigit(r) && r != '_'
	}) {
		token := strings.ToLower(field)
		if len(token) < directiveLexicalMinTokenLength || token == "or" ||
			seen[token] {
			continue
		}
		seen[token] = true
		patterns = append(patterns, "%"+token+"%")
		if len(patterns) == directiveLexicalMaxTokens {
			break
		}
	}
	if len(patterns) == 0 {
		return "", nil
	}

	var predicate strings.Builder
	for index := range patterns {
		if index > 0 {
			predicate.WriteString(" OR ")
		}
		predicate.WriteString("LOWER(question || ' ' || topic) LIKE $")
		predicate.WriteString(strconv.Itoa(index + 2))
	}
	statement := directiveColumns + ` WHERE state = 'open' AND (` +
		predicate.String() + `)` + directiveOrder + `$1`
	return statement, append([]any{int64(limit)}, patterns...)
}

// readDirectives collects the shared nineteen-column shape.
func readDirectives(ctx context.Context, store Store, query string, args []any,
	ceiling int,
) ([]db2contract.DirectiveListRow, error) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	found := make([]db2contract.DirectiveListRow, 0, 16)
	for rows.Next() && len(found) < ceiling {
		row, scanErr := scanDirective(rows)
		if scanErr != nil {
			return nil, scanErr
		}
		found = append(found, row)
	}
	return found, rows.Err()
}

// directiveScanner is the narrow slice of a row reader both the list reads and
// the single-row read use.
type directiveScanner interface {
	Scan(dest ...any) error
}

func scanDirective(row directiveScanner) (db2contract.DirectiveListRow, error) {
	var id, priority, memoryA, memoryB, resolution, surfaced int64
	var question, topic, entity, file, cause, state string
	var evidence, session, lastSurfaced, resolvedAt string
	var validUntil, createdAt, updatedAt string
	if err := row.Scan(&id, &question, &topic, &entity, &file, &cause, &priority,
		&state, &memoryA, &memoryB, &resolution, &evidence, &session, &surfaced,
		&lastSurfaced, &resolvedAt, &validUntil, &createdAt,
		&updatedAt); err != nil {
		return db2contract.DirectiveListRow{}, err
	}
	return db2contract.DirectiveListRow{
		DirectiveID:        clampToU64(id),
		Question:           question,
		Topic:              topic,
		AnchorEntity:       entity,
		AnchorFile:         file,
		Cause:              cause,
		Priority:           clampToU32(priority),
		State:              state,
		MemoryAID:          clampToU64(memoryA),
		MemoryBID:          clampToU64(memoryB),
		ResolutionMemoryID: clampToU64(resolution),
		Evidence:           evidence,
		SourceSession:      session,
		SurfacedCount:      clampToU32(surfaced),
		LastSurfacedAt:     lastSurfaced,
		ResolvedAt:         resolvedAt,
		ValidUntil:         validUntil,
		CreatedAt:          createdAt,
		UpdatedAt:          updatedAt,
	}, nil
}

// directiveGet answers one directive in full.
func directiveGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	directiveID, err := db2contract.DecodeDirectiveGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	row, scanErr := scanDirective(
		store.QueryRow(ctx, directiveGetQuery, int64(directiveID)))
	found := uint32(1)
	if scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, row = 0, db2contract.DirectiveListRow{}
	}
	reply, encodeErr := db2contract.EncodeDirectiveGetReply(found, row.Question,
		row.Topic, row.AnchorEntity, row.AnchorFile, row.Cause, row.Priority,
		row.State, row.MemoryAID, row.MemoryBID, row.ResolutionMemoryID,
		row.Evidence, row.SourceSession, row.SurfacedCount, row.LastSurfacedAt,
		row.ResolvedAt, row.ValidUntil, row.CreatedAt, row.UpdatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// directiveList lists directives, optionally by state and cause.
//
// Either filter can be left out and both can be, which is the C's four
// statements said once. Unlike the three anchored reads, this one will show a
// resolved or suppressed directive -- it is the review view rather than a
// surfacing one.
func directiveList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	state, cause, limit, err := db2contract.DecodeDirectiveListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DirectiveListMaxRows
	found, queryErr := readDirectives(ctx, store, directiveListQuery,
		[]any{int64(pairLimit(limit, ceiling)), state, cause}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeDirectiveListReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// directiveByEntity lists open directives anchored to an entity.
func directiveByEntity(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, err := db2contract.DecodeDirectiveByEntityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DirectiveByEntityMaxRows
	found, queryErr := readDirectives(ctx, store, directiveByEntityQuery,
		[]any{int64(pairLimit(limit, ceiling)), entity}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	byEntity := make([]db2contract.DirectiveByEntityRow, len(found))
	for index, row := range found {
		byEntity[index] = db2contract.DirectiveByEntityRow(row)
	}
	reply, encodeErr := db2contract.EncodeDirectiveByEntityReply(byEntity)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// directiveByFile lists open directives anchored to a file.
func directiveByFile(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	filePath, limit, err := db2contract.DecodeDirectiveByFileRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DirectiveByFileMaxRows
	found, queryErr := readDirectives(ctx, store, directiveByFileQuery,
		[]any{int64(pairLimit(limit, ceiling)), filePath}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	byFile := make([]db2contract.DirectiveByFileRow, len(found))
	for index, row := range found {
		byFile[index] = db2contract.DirectiveByFileRow(row)
	}
	reply, encodeErr := db2contract.EncodeDirectiveByFileReply(byFile)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// directiveByLexical lists open directives whose question or topic mentions
// what a caller is talking about.
//
// A clause with nothing matchable in it answers nothing without running a
// statement, because the statement it would build has an empty parenthesis in
// it and does not parse.
func directiveByLexical(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	clause, limit, err := db2contract.DecodeDirectiveByLexicalRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DirectiveByLexicalMaxRows

	found := []db2contract.DirectiveListRow{}
	statement, args := lexicalDirectiveQuery(clause,
		pairLimit(limit, ceiling), ceiling)
	if statement != "" {
		var queryErr error
		found, queryErr = readDirectives(ctx, store, statement, args, ceiling)
		if queryErr != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	byLexical := make([]db2contract.DirectiveByLexicalRow, len(found))
	for index, row := range found {
		byLexical[index] = db2contract.DirectiveByLexicalRow(row)
	}
	reply, encodeErr := db2contract.EncodeDirectiveByLexicalReply(byLexical)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
