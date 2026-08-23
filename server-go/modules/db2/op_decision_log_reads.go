package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageDecisionLogGet,
		db2contract.OperationDecisionLogGet, decisionLogGet)
	Register(db2contract.StageDecisionLogList,
		db2contract.OperationDecisionLogList, decisionLogList)
	Register(db2contract.StageDecisionLogListScoped,
		db2contract.OperationDecisionLogListScoped, decisionLogListScoped)
	Register(db2contract.StageLearningProposalGet,
		db2contract.OperationLearningProposalGet, learningProposalGet)
	Register(db2contract.StageTaskList,
		db2contract.OperationTaskList, taskList)
}

// The fourteen columns every decision read returns.
//
// task_id and outcome are the nullable pair -- a decision need not belong to a
// task, and one that has not played out yet has no outcome. Both are the shape
// that fails only against a real row, which is how decision_log's two nullable
// columns were found earlier in this port.
const decisionColumns = `SELECT id, COALESCE(task_id, 0), options, chosen,
 rationale, assumptions, COALESCE(outcome, ''), created_at, status,
 revisit_when, supersedes_id, subject, author, linked_policy_id
 FROM decision_log`

// Newest first, with the identifier breaking ties.
//
// The C's scoped list has that tiebreak and its unscoped list does not, so the
// unscoped one truncates unpredictably at the limit when two decisions share a
// timestamp -- and these are written by hand, so sharing one is not rare. The
// sibling establishes what the order was meant to be.
const decisionOrder = ` ORDER BY created_at DESC, id DESC LIMIT `

const (
	decisionLogGetQuery  = decisionColumns + ` WHERE id = $1`
	decisionLogListQuery = decisionColumns +
		` WHERE ($2 = '' OR outcome = $2)` + decisionOrder + `$1`
	decisionLogListScopedQuery = decisionColumns +
		` WHERE ($2 = '' OR subject = $2) AND ($3 = '' OR status = $3)` +
		decisionOrder + `$1`
)

// decisionScanner is the narrow slice of a row reader the three reads share.
type decisionScanner interface {
	Scan(dest ...any) error
}

func scanDecision(row decisionScanner) (db2contract.DecisionLogListRow, error) {
	var id, taskID, supersedes, policyID int64
	var options, chosen, rationale, assumptions, outcome string
	var createdAt, status, revisitWhen, subject, author string
	if err := row.Scan(&id, &taskID, &options, &chosen, &rationale, &assumptions,
		&outcome, &createdAt, &status, &revisitWhen, &supersedes, &subject,
		&author, &policyID); err != nil {
		return db2contract.DecisionLogListRow{}, err
	}
	return db2contract.DecisionLogListRow{
		DecisionID:        clampToU64(id),
		DecisionTaskID:    clampToU64(taskID),
		SupersedesID:      clampToU64(supersedes),
		LinkedPolicyID:    clampToU64(policyID),
		Options:           options,
		Chosen:            chosen,
		Rationale:         rationale,
		Assumptions:       assumptions,
		Outcome:           outcome,
		DecisionCreatedAt: createdAt,
		DecisionStatus:    status,
		RevisitWhen:       revisitWhen,
		DecisionSubject:   subject,
		DecisionAuthor:    author,
	}, nil
}

func readDecisions(ctx context.Context, store Store, query string, args []any,
	ceiling int,
) ([]db2contract.DecisionLogListRow, error) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	found := make([]db2contract.DecisionLogListRow, 0, 16)
	for rows.Next() && len(found) < ceiling {
		row, scanErr := scanDecision(rows)
		if scanErr != nil {
			return nil, scanErr
		}
		found = append(found, row)
	}
	return found, rows.Err()
}

// decisionLogGet answers one decision in full.
//
// The options and the rationale are what make the log worth keeping: what was
// chosen is recoverable from the code, and what was rejected and why is not.
func decisionLogGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, err := db2contract.DecodeDecisionLogGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	row, scanErr := scanDecision(
		store.QueryRow(ctx, decisionLogGetQuery, int64(decisionID)))
	found := uint32(1)
	if scanErr != nil {
		found, row = 0, db2contract.DecisionLogListRow{}
	}
	reply, encodeErr := db2contract.EncodeDecisionLogGetReply(found,
		row.DecisionTaskID, row.Options, row.Chosen, row.Rationale,
		row.Assumptions, row.Outcome, row.DecisionCreatedAt, row.DecisionStatus,
		row.RevisitWhen, row.SupersedesID, row.DecisionSubject,
		row.DecisionAuthor, row.LinkedPolicyID)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// decisionLogList lists decisions, optionally by how they turned out.
func decisionLogList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	outcome, limit, err := db2contract.DecodeDecisionLogListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DecisionLogListMaxRows
	found, queryErr := readDecisions(ctx, store, decisionLogListQuery,
		[]any{int64(pairLimit(limit, ceiling)), outcome}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeDecisionLogListReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// decisionLogListScoped lists decisions about one subject, optionally in one
// status.
//
// The subject is what makes a decision findable later: someone revisiting a
// choice knows what it was about long before they know when it was made.
func decisionLogListScoped(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	subject, status, limit, err :=
		db2contract.DecodeDecisionLogListScopedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.DecisionLogListScopedMaxRows
	found, queryErr := readDecisions(ctx, store, decisionLogListScopedQuery,
		[]any{int64(pairLimit(limit, ceiling)), subject, status}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	scoped := make([]db2contract.DecisionLogListScopedRow, len(found))
	for index, row := range found {
		scoped[index] = db2contract.DecisionLogListScopedRow(row)
	}
	reply, encodeErr := db2contract.EncodeDecisionLogListScopedReply(scoped)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const learningProposalGetQuery = `SELECT signal_id, sink, state, target_key,
 target_memory_id, action_json, evidence_refs, corroboration_count,
 expires_at, committed_at, archive_reason, created_at, updated_at
 FROM learning_proposals WHERE id = $1`

// learningProposalGet answers one proposal in full.
//
// The archive reason and the committed-at stamp are the two that say what
// happened to it: a proposal that was committed has the second, one that was
// dropped has the first, and one still pending has neither.
func learningProposalGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	proposalID, err := db2contract.DecodeLearningProposalGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var signalID, targetMemoryID, corroboration int64
	var sink, state, targetKey, action, evidence string
	var expiresAt, committedAt, archiveReason, createdAt, updatedAt string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, learningProposalGetQuery,
		int64(proposalID)).Scan(&signalID, &sink, &state, &targetKey,
		&targetMemoryID, &action, &evidence, &corroboration, &expiresAt,
		&committedAt, &archiveReason, &createdAt, &updatedAt); scanErr != nil {
		found, signalID, targetMemoryID, corroboration = 0, 0, 0, 0
		sink, state, targetKey, action, evidence = "", "", "", "", ""
		expiresAt, committedAt, archiveReason = "", "", ""
		createdAt, updatedAt = "", ""
	}
	reply, encodeErr := db2contract.EncodeLearningProposalGetReply(found,
		clampToU32(signalID), sink, state, targetKey,
		clampToU64(targetMemoryID), action, evidence,
		clampToU32(corroboration), expiresAt, committedAt, archiveReason,
		createdAt, updatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Most recently touched first, and by updated_at rather than created_at: a task
// list is read to see what is being worked on, and the last thing that moved is
// the most interesting.
//
// The identifier breaks ties, which the C leaves out here as it does on the
// decision list -- and two tasks updated in the same second is the common case
// when a batch of subtasks is created together.
const taskListQuery = taskColumns +
	` WHERE ($2 = '' OR state = $2) AND ($3 = '' OR session_id = $3)
 ORDER BY updated_at DESC, id DESC LIMIT $1`

// taskList lists tasks, optionally by state and session.
func taskList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	stateFilter, sessionFilter, limit, err :=
		db2contract.DecodeTaskListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.TaskListMaxRows
	rows, queryErr := store.Query(ctx, taskListQuery,
		int64(pairLimit(limit, ceiling)), stateFilter, sessionFilter)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	tasks := make([]db2contract.TaskListRow, 0, 16)
	for rows.Next() && len(tasks) < ceiling {
		var id, parentID int64
		var title, state, sessionID, createdAt, updatedAt string
		var confidence float64
		if scanErr := rows.Scan(&id, &parentID, &title, &state, &confidence,
			&sessionID, &createdAt, &updatedAt); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		tasks = append(tasks, db2contract.TaskListRow{
			TaskRowID:      clampToU64(id),
			ParentTaskID:   clampToU64(parentID),
			TaskConfidence: confidence,
			TaskTitle:      title,
			TaskState:      state,
			TaskCreatedAt:  createdAt,
			TaskUpdatedAt:  updatedAt,
			TaskSessionID:  sessionID,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTaskListReply(tasks)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
