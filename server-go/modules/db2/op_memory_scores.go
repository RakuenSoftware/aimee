package db2

import (
	"context"
	"errors"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageMemorySalience,
		db2contract.OperationMemorySalience, memorySalience)
	Register(db2contract.StageMemorySurprise,
		db2contract.OperationMemorySurprise, memorySurprise)
	Register(db2contract.StageProspectiveSetState,
		db2contract.OperationProspectiveSetState, prospectiveSetState)
	Register(db2contract.StageLifecycleMarkPending,
		db2contract.OperationLifecycleMarkPending, lifecycleMarkPending)
	Register(db2contract.StageOntologyEvalCount,
		db2contract.OperationOntologyEvalCount, ontologyEvalCount)
	Register(db2contract.StageDirectiveResolve,
		db2contract.OperationDirectiveResolve, directiveResolve)
	Register(db2contract.StageDecisionLogActiveID,
		db2contract.OperationDecisionLogActiveID, decisionLogActiveID)
}

const (
	memorySalienceQuery = `SELECT salience FROM memories WHERE id = $1`
	memorySurpriseQuery = `SELECT surprise FROM memories WHERE id = $1`
)

// readScoreOrDefault reads one score column, answering the caller's default
// when the memory is absent.
//
// The default covers absence, not an unset score: both columns are NOT NULL
// with a default of their own, so a memory that exists always has a number and
// the caller's default never stands in for it. Passing a default is how a
// caller says what an unknown memory should score, which is a different
// question from what a known one scores.
func readScoreOrDefault(ctx context.Context, store Store, query string,
	memoryID uint64, fallback float64,
) (float64, bus.ModuleStatus) {
	var score *float64
	if err := store.QueryRow(ctx, query, int64(memoryID)).Scan(&score); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return fallback, bus.ModuleStatusOK
		}
		return 0, bus.ModuleStatusInternal
	}
	return decimal(score), bus.ModuleStatusOK
}

// memorySalience reads how much a memory matters, or the caller's default.
func memorySalience(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, fallback, err := db2contract.DecodeMemorySalienceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	score, status := readScoreOrDefault(ctx, store, memorySalienceQuery, memoryID, fallback)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeMemorySalienceReply(score)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// memorySurprise reads how unexpected a memory was, or the caller's default.
func memorySurprise(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, fallback, err := db2contract.DecodeMemorySurpriseRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	score, status := readScoreOrDefault(ctx, store, memorySurpriseQuery, memoryID, fallback)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeMemorySurpriseReply(score)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const prospectiveSetStateQuery = `UPDATE prospective_memories
 SET state = $2, updated_at = pg_now_text()
 WHERE id = $1`

// prospectiveSetState moves a prospective memory between armed, fired and
// cancelled.
//
// No state machine and no row check, matching the C: any state replaces any
// other and an identifier nothing holds still acknowledges. A caller cancelling
// a trigger that has already fired is doing the right thing with stale
// information, and failing them would turn a harmless race into an error.
func prospectiveSetState(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	prospectiveID, state, err := db2contract.DecodeProspectiveSetStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The acknowledgement means a row moved, not that the statement ran: the C
	// returns success only when its update changed something, and a caller
	// arming or firing a prospective memory needs to know it addressed one.
	changed, execErr := store.Exec(ctx, prospectiveSetStateQuery,
		int64(prospectiveID), state)
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeProspectiveSetStateReply)
}

const lifecycleMarkPendingQuery = `UPDATE memories
 SET lifecycle_state = 'pending',
     ttl_at = pg_now_text($2),
     updated_at = pg_now_text()
 WHERE id = $1`

// lifecycleMarkPending gives a memory a deadline to be confirmed by.
//
// The C builds its statement with the day count printed into the SQL text,
// noting that it is a small integer it controls. The port binds it instead: the
// interval is assembled in Go from a number the contract has already bounded
// and handed over as a parameter, so no caller value reaches the statement
// text. Same answer, one fewer place where that reasoning has to hold.
//
// A zero window is refused rather than treated as "immediately". The C refuses
// it too, and a memory whose deadline is the moment it was set would be stale
// before anyone could confirm it.
func lifecycleMarkPending(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, ttlDays, err := db2contract.DecodeLifecycleMarkPendingRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if ttlDays == 0 {
		return acknowledgement(false, db2contract.EncodeLifecycleMarkPendingReply)
	}
	window := fmt.Sprintf("+%d days", ttlDays)
	_, execErr := store.Exec(ctx, lifecycleMarkPendingQuery, int64(memoryID), window)
	return acknowledgement(execErr == nil, db2contract.EncodeLifecycleMarkPendingReply)
}

const ontologyEvalCountQuery = `SELECT occurrence_count FROM ontology_evaluations
 WHERE rel_type = $1 LIMIT 1`

// ontologyEvalCount reads how often a proposed relation type has been seen.
//
// Found is separate from the count for the same reason confidence carries a
// flag: a relation seen zero times and a relation nobody has proposed are
// different states, and the promotion threshold reads them differently.
//
// The name is normalized first, because that is the form the table holds.
func ontologyEvalCount(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	relType, err := db2contract.DecodeOntologyEvalCountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	normalized := normalizeRelType(relType)
	if normalized == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var occurrences *int64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, ontologyEvalCountQuery, normalized).
		Scan(&occurrences); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		found = 0
	}
	reply, err := db2contract.EncodeOntologyEvalCountReply(found, clampToU64(number(occurrences)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const directiveResolveQuery = `UPDATE epistemic_directives
 SET state = 'resolved',
     resolution_memory_id = $2,
     resolved_at = pg_now_text(),
     updated_at = pg_now_text()
 WHERE id = $1 AND state = 'open'`

// directiveResolve closes an open question, recording the memory that answered
// it.
//
// Only while open, and only if a row changed. A directive resolved twice would
// overwrite which memory answered it, and the second answer is not more right
// than the first -- it is a second caller acting on a question already closed.
func directiveResolve(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	directiveID, resolutionMemoryID, err := db2contract.DecodeDirectiveResolveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	changed, execErr := store.Exec(ctx, directiveResolveQuery,
		int64(directiveID), int64(resolutionMemoryID))
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeDirectiveResolveReply)
}

// Matches idx_dl_active_scope exactly: subject and linked_policy_id, where the
// status is active. Reading through the same columns the index is built on is
// what keeps this a lookup rather than a scan.
const decisionLogActiveIDQuery = `SELECT id FROM decision_log
 WHERE subject = $1 AND linked_policy_id = $2 AND status = 'active' LIMIT 1`

// decisionLogActiveID finds the decision currently in force for a subject.
//
// Zero means none, which is how a caller learns it is free to record one. The
// index makes at most one row possible for a non-empty subject, so LIMIT 1 is
// belt and braces rather than a choice between candidates.
func decisionLogActiveID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	subject, linkedPolicy, err := db2contract.DecodeDecisionLogActiveIDRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	id, status := readOptionalInt(ctx, store, decisionLogActiveIDQuery,
		subject, int64(linkedPolicy))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeDecisionLogActiveIDReply(clampToU64(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
