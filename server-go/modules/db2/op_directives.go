package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageDirectiveFindByCauseTopic,
		db2contract.OperationDirectiveFindByCauseTopic, directiveFindByCauseTopic)
	Register(db2contract.StageDirectiveInsertIgnore,
		db2contract.OperationDirectiveInsertIgnore, directiveInsertIgnore)
}

// The C implementation's ED_SELECT_COLS. Every column here is declared NOT
// NULL, so each is scanned directly; the table's one nullable column is its
// full-text vector, which nothing selects.
const directiveSelectColumns = `id, question, topic, anchor_entity, anchor_file, cause,
 priority, state, memory_a_id, memory_b_id, resolution_memory_id, evidence, source_session,
 surfaced_count, last_surfaced_at, resolved_at, valid_until, created_at, updated_at`

const directiveFindByCauseTopicQuery = `SELECT ` + directiveSelectColumns +
	` FROM epistemic_directives WHERE cause = $1 AND topic = $2 LIMIT 1`

// directiveFindByCauseTopic finds a directive by what caused it and what it is
// about.
//
// The pair is not unique and the query names no ordering, so which directive
// answers when several share a cause and topic is the database's choice. That
// is tolerable only because of what this is for: the insert beside it uses this
// to tell an existing directive from a new one, and for that any of them proves
// the pair is taken.
func directiveFindByCauseTopic(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	cause, topic, err := db2contract.DecodeDirectiveFindByCauseTopicRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var (
		id                 int64
		question           string
		directiveTopic     string
		anchorEntity       string
		anchorFile         string
		directiveCause     string
		priority           int32
		state              string
		memoryA            int64
		memoryB            int64
		resolutionMemoryID int64
		evidence           string
		sourceSession      string
		surfacedCount      int32
		lastSurfacedAt     string
		resolvedAt         string
		validUntil         string
		createdAt          string
		updatedAt          string
	)
	scanErr := store.QueryRow(ctx, directiveFindByCauseTopicQuery, cause, topic).Scan(
		&id, &question, &directiveTopic, &anchorEntity, &anchorFile, &directiveCause,
		&priority, &state, &memoryA, &memoryB, &resolutionMemoryID, &evidence,
		&sourceSession, &surfacedCount, &lastSurfacedAt, &resolvedAt, &validUntil,
		&createdAt, &updatedAt)
	if errors.Is(scanErr, pgx.ErrNoRows) {
		reply, encodeErr := db2contract.EncodeDirectiveFindByCauseTopicReply(
			0, 0, "", "", "", 0, "", 0, 0, 0, "", "", 0, "", "", "", "", "")
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	if scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	// The cause and topic are not in the reply: the caller sent them, and this
	// answers with the directive they identify rather than with them again.
	reply, err := db2contract.EncodeDirectiveFindByCauseTopicReply(
		1, clampToU64(id), question, anchorEntity, anchorFile,
		clampToU32(int64(priority)), state, clampToU64(memoryA), clampToU64(memoryB),
		clampToU64(resolutionMemoryID), evidence, sourceSession,
		clampToU32(int64(surfacedCount)), lastSurfacedAt, resolvedAt, validUntil,
		createdAt, updatedAt)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const directiveInsertIgnoreQuery = `INSERT INTO epistemic_directives
 (question, topic, anchor_entity, anchor_file, cause, priority, state,
  memory_a_id, memory_b_id, evidence, source_session, valid_until)
 VALUES ($1, $2, $3, $4, $5, $6, 'open', $7, $8, $9, $10, $11)
 ON CONFLICT DO NOTHING RETURNING id`

// The lookup after a conflict, by the natural key. It is deliberately NOT the
// index that refused the insert -- see the note in directiveInsertIgnore.
const directiveExistingQuery = `SELECT id FROM epistemic_directives
 WHERE cause = $1 AND topic = $2 AND question = $3 LIMIT 1`

// directiveInsertIgnore raises a directive unless an equivalent one is already
// open, and says which happened.
//
// Uniqueness is not one rule. Partial indexes make a contradiction unique by
// its two memories and a retrieval failure unique by its topic, so what counts
// as the same directive depends on what caused it. ON CONFLICT DO NOTHING
// covers both, and RETURNING yields a row only on an actual insert.
//
// On a conflict the existing row is looked up by cause, topic and question --
// which is not the index that refused the insert. A contradiction deduped by
// its two memories can carry a different question, in which case the lookup
// finds nothing and the caller is told the directive existed with an identifier
// of zero. That is the C behaviour and the reply schema's reason records it.
func directiveInsertIgnore(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	question, topic, anchorEntity, anchorFile, cause, priority, memoryA, memoryB,
		evidence, sourceSession, validUntil, err :=
		db2contract.DecodeDirectiveInsertIgnoreRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if question == "" || cause == "" {
		return directiveInsertAnswer(0, 0, 0)
	}

	var id int64
	scanErr := store.QueryRow(ctx, directiveInsertIgnoreQuery, question, topic, anchorEntity,
		anchorFile, cause, int32(priority), int64(memoryA), int64(memoryB), evidence,
		sourceSession, validUntil).Scan(&id)
	if scanErr == nil {
		return directiveInsertAnswer(1, clampToU64(id), 0)
	}
	if !errors.Is(scanErr, pgx.ErrNoRows) {
		// A real failure, not a conflict. ON CONFLICT DO NOTHING answers no rows
		// when it declined to insert, which is the only reason this should be
		// reached without an error of its own.
		return directiveInsertAnswer(0, 0, 0)
	}

	// Declined. The directive exists; whether this can name it is another
	// question.
	var existing int64
	if err := store.QueryRow(ctx, directiveExistingQuery, cause, topic, question).
		Scan(&existing); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return directiveInsertAnswer(1, 0, 1)
		}
		return directiveInsertAnswer(0, 0, 0)
	}
	return directiveInsertAnswer(1, clampToU64(existing), 1)
}

func directiveInsertAnswer(acknowledged uint32, id uint64, existed uint32) (
	[]byte, bus.ModuleStatus,
) {
	reply, err := db2contract.EncodeDirectiveInsertIgnoreReply(acknowledged, id, existed)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
