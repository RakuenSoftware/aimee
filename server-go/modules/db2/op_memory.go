package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageProspectiveGet,
		db2contract.OperationProspectiveGet, prospectiveGet)
	Register(db2contract.StageProspectiveInsert,
		db2contract.OperationProspectiveInsert, prospectiveInsert)
	Register(db2contract.StageProspectiveRecordTrigger,
		db2contract.OperationProspectiveRecordTrigger, prospectiveRecordTrigger)
}

// Column order is the C implementation's PM_SELECT_COLS, and the reply schema
// was written from the same order. Keeping all three aligned is what makes a
// row read here the row the C module would have read.
const prospectiveSelectColumns = `id, trigger_text, action_text, anchor_entity, anchor_file,
 recurrence, state, valid_until, source_session, trigger_count, last_triggered_at,
 created_at, updated_at`

const prospectiveGetQuery = `SELECT ` + prospectiveSelectColumns +
	` FROM prospective_memories WHERE id = $1`

// prospectiveGet reads one prospective memory: a trigger, and what to do when
// it fires.
//
// Absence is an answer here rather than a failure -- the reply's found flag
// carries it -- so a missing row is ModuleStatusOK with the flag clear, not an
// error a caller has to interpret.
func prospectiveGet(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	id, err := db2contract.DecodeProspectiveGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	// The identifier is selected because the column list is the C
	// implementation's, and discarded because the reply does not carry it: the
	// caller supplied it and would learn nothing from being told it back.
	var (
		discardedID     int64
		triggerText     string
		actionText      string
		anchorEntity    string
		anchorFile      string
		recurrence      string
		state           string
		validUntil      string
		sourceSession   string
		triggerCount    int32
		lastTriggeredAt string
		createdAt       string
		updatedAt       string
	)
	scanErr := store.QueryRow(ctx, prospectiveGetQuery, int64(id)).Scan(
		&discardedID, &triggerText, &actionText, &anchorEntity, &anchorFile, &recurrence,
		&state, &validUntil, &sourceSession, &triggerCount, &lastTriggeredAt,
		&createdAt, &updatedAt)
	if errors.Is(scanErr, pgx.ErrNoRows) {
		reply, encodeErr := db2contract.EncodeProspectiveGetReply(
			0, "", "", "", "", "", "", "", "", 0, "", "", "")
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	if scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeProspectiveGetReply(
		1, triggerText, actionText, anchorEntity, anchorFile, recurrence, state,
		validUntil, sourceSession, clampToU32(int64(triggerCount)), lastTriggeredAt,
		createdAt, updatedAt)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const prospectiveInsertQuery = `INSERT INTO prospective_memories
 (trigger_text, action_text, anchor_entity, anchor_file, recurrence, state, valid_until, source_session)
 VALUES ($1, $2, $3, $4, $5, 'armed', $6, $7) RETURNING id`

// prospectiveInsert arms a prospective memory.
//
// Nothing checks that the anchor names anything, so a prospective can be armed
// against a file that does not exist. That is the C behaviour and it is carried
// deliberately: tightening it here would make the Go module refuse writes the C
// one accepts, which a parity comparison would call a difference and an
// operator would call an outage.
func prospectiveInsert(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	triggerText, actionText, anchorEntity, anchorFile, recurrence, validUntil,
		sourceSession, err := db2contract.DecodeProspectiveInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var id int64
	scanErr := store.QueryRow(ctx, prospectiveInsertQuery, triggerText, actionText,
		anchorEntity, anchorFile, recurrence, validUntil, sourceSession).Scan(&id)
	if scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeProspectiveInsertReply(clampToU64(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Two statements rather than one with a conditional, matching the C
// implementation: a terminal firing closes the prospective, a non-terminal one
// only counts. Which of the two it is comes from the caller and not from the
// row -- the recurrence column is not consulted by either.
const (
	prospectiveTriggerTerminalQuery = `UPDATE prospective_memories
 SET state = 'triggered', trigger_count = trigger_count + 1,
     last_triggered_at = pg_now_text(), updated_at = pg_now_text()
 WHERE id = $1`
	prospectiveTriggerCountQuery = `UPDATE prospective_memories
 SET trigger_count = trigger_count + 1,
     last_triggered_at = pg_now_text(), updated_at = pg_now_text()
 WHERE id = $1`
)

// prospectiveRecordTrigger counts a firing, and closes the prospective when the
// firing is terminal.
//
// The acknowledgement means the statement ran, not that a row moved: an
// identifier naming nothing updates nothing and still succeeds. The C
// implementation reports the same, and the reply schema's reason says so.
func prospectiveRecordTrigger(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	id, terminal, err := db2contract.DecodeProspectiveRecordTriggerRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	query := prospectiveTriggerCountQuery
	if terminal != 0 {
		query = prospectiveTriggerTerminalQuery
	}
	if _, execErr := store.Exec(ctx, query, int64(id)); execErr != nil {
		reply, encodeErr := db2contract.EncodeProspectiveRecordTriggerReply(0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}

	reply, err := db2contract.EncodeProspectiveRecordTriggerReply(1)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
