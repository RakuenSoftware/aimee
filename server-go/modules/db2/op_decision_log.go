package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageDecisionLogInsert,
		db2contract.OperationDecisionLogInsert, decisionLogInsert)
	Register(db2contract.StageDecisionLogRecord,
		db2contract.OperationDecisionLogRecord, decisionLogRecord)
}

// The row both writes hand back, read by identifier after the write. Column
// order is the C implementation's, and the reply schema follows it.
const decisionLogRowQuery = `SELECT id, task_id, options, chosen, rationale, assumptions,
 outcome, created_at, status, revisit_when, supersedes_id, subject, author, linked_policy_id
 FROM decision_log WHERE id = $1`

// Two statements rather than one, matching the C implementation: an empty
// timestamp is absence, not an empty value, and selects the form that lets the
// database stamp the row. The wire has no null, so the empty string is where
// the two meet.
const (
	decisionLogInsertStampedQuery = `INSERT INTO decision_log
 (task_id, options, chosen, rationale, assumptions, created_at)
 VALUES ($1, $2, $3, $4, $5, $6) RETURNING id`
	decisionLogInsertNowQuery = `INSERT INTO decision_log
 (task_id, options, chosen, rationale, assumptions, created_at)
 VALUES ($1, $2, $3, $4, $5, pg_now_text()) RETURNING id`
)

// decisionLogInsert records a decision against a task and returns the row it
// wrote, so a caller sees what landed rather than what it sent.
func decisionLogInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	taskID, options, chosen, rationale, assumptions, createdAt, err :=
		db2contract.DecodeDecisionLogInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var id int64
	var scanErr error
	if createdAt != "" {
		scanErr = store.QueryRow(ctx, decisionLogInsertStampedQuery, int64(taskID), options,
			chosen, rationale, assumptions, createdAt).Scan(&id)
	} else {
		scanErr = store.QueryRow(ctx, decisionLogInsertNowQuery, int64(taskID), options,
			chosen, rationale, assumptions).Scan(&id)
	}
	if scanErr != nil {
		return decisionLogUnacknowledged(db2contract.EncodeDecisionLogInsertReply)
	}
	return decisionLogReadBack(ctx, store, id, db2contract.EncodeDecisionLogInsertReply)
}

const (
	decisionLogSupersedeQuery = `UPDATE decision_log SET status = 'superseded'
 WHERE id = $1 AND status = 'active' AND subject = $2`
	decisionLogRecordQuery = `INSERT INTO decision_log
 (task_id, options, chosen, rationale, assumptions, created_at,
  status, revisit_when, supersedes_id, subject, author, linked_policy_id)
 VALUES (0, $1, $2, $3, '', pg_now_text(), 'active', $4, $5, $6, $7, $8) RETURNING id`
)

// errDecisionSupersedeMissed aborts the transaction when the supersede matched
// nothing. It never reaches a caller: what the caller sees is an unacknowledged
// reply, the same as any other refusal this operation can make.
var errDecisionSupersedeMissed = errors.New("db2: superseded no active decision in this subject")

// decisionLogRecord records a scoped decision and, when it supersedes one,
// retires that one in the same transaction.
//
// A subject never has two active decisions and never loses the old one without
// gaining the new. The supersede is constrained to an ACTIVE row in the SAME
// subject, so a decision cannot retire one from another scope, and an
// identifier matching nothing active fails the whole call rather than inserting
// beside it -- which is why the changed-row count is checked rather than the
// statement's success. An UPDATE that matches nothing succeeds.
func decisionLogRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	subject, options, chosen, rationale, author, linkedPolicyID, revisitWhen,
		supersedesID, err := db2contract.DecodeDecisionLogRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if subject == "" {
		// An empty subject would be one global slot rather than a scope.
		return decisionLogUnacknowledged(db2contract.EncodeDecisionLogRecordReply)
	}

	var id int64
	txErr := store.InTx(ctx, func(tx Store) error {
		if supersedesID > 0 {
			changed, err := tx.Exec(ctx, decisionLogSupersedeQuery, int64(supersedesID), subject)
			if err != nil {
				return err
			}
			if changed != 1 {
				return errDecisionSupersedeMissed
			}
		}
		return tx.QueryRow(ctx, decisionLogRecordQuery, options, chosen, rationale,
			revisitWhen, int64(supersedesID), subject, author, int64(linkedPolicyID)).Scan(&id)
	})
	if txErr != nil {
		return decisionLogUnacknowledged(db2contract.EncodeDecisionLogRecordReply)
	}
	return decisionLogReadBack(ctx, store, id, db2contract.EncodeDecisionLogRecordReply)
}

// decisionLogEncoder is the shape both replies share: an acknowledgement and
// the row. Naming it lets the read-back and the refusal be written once.
type decisionLogEncoder func(acknowledged uint32, id, taskID uint64, options, chosen,
	rationale, assumptions, outcome, createdAt, status, revisitWhen string,
	supersedes uint64, subject, author string, linkedPolicy uint64) ([]byte, error)

func decisionLogUnacknowledged(encode decisionLogEncoder) ([]byte, bus.ModuleStatus) {
	reply, err := encode(0, 0, 0, "", "", "", "", "", "", "", "", 0, "", "", 0)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// decisionLogReadBack reads the row that was just written.
//
// Outside the transaction deliberately, matching the C implementation: it
// commits and then reads. That leaves a window in which another writer could
// change the row, so what comes back is the row as it is rather than as it was
// written -- which is the more useful answer and the one a caller can act on.
func decisionLogReadBack(ctx context.Context, store Store, id int64,
	encode decisionLogEncoder) ([]byte, bus.ModuleStatus) {
	// task_id and outcome are the two nullable columns in this row, so they are
	// scanned through pointers; the rest are declared NOT NULL and are read
	// directly. See scan.go for why the distinction has to be made per column.
	var (
		rowID        int64
		taskID       *int64
		options      string
		chosen       string
		rationale    string
		assumptions  string
		outcome      *string
		createdAt    string
		status       string
		revisitWhen  string
		supersedes   int64
		subject      string
		author       string
		linkedPolicy int64
	)
	if err := store.QueryRow(ctx, decisionLogRowQuery, id).Scan(&rowID, &taskID, &options,
		&chosen, &rationale, &assumptions, &outcome, &createdAt, &status, &revisitWhen,
		&supersedes, &subject, &author, &linkedPolicy); err != nil {
		return decisionLogUnacknowledged(encode)
	}
	reply, err := encode(1, clampToU64(rowID), clampToU64(number(taskID)), options, chosen,
		rationale, assumptions, text(outcome), createdAt, status, revisitWhen,
		clampToU64(supersedes), subject, author, clampToU64(linkedPolicy))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
