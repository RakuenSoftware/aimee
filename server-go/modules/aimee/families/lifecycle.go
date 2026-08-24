package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The lifecycle family: the work-item tree, its audit events, and the per-stage
// attempt counters.
const (
	EventLifecycle uint32 = 11792
	StageLifecycle uint32 = 16

	opWorkItemCreate                 uint32 = 1
	opWorkItemGet                    uint32 = 2
	opWorkItemIDByProposal           uint32 = 3
	opWorkItemIDByPRRef              uint32 = 4
	opWorkItemSetStage               uint32 = 5
	opWorkItemSetPRRef               uint32 = 6
	opWorkItemSetWorktree            uint32 = 7
	opWorkItemSetSubmitter           uint32 = 8
	opWorkItemSetParent              uint32 = 9
	opWorkItemAbandonChildren        uint32 = 10
	opWorkItemChildCounts            uint32 = 11
	opWorkItemCountActiveBySubmitter uint32 = 12
	opWorkItemCountRecentBySubmitter uint32 = 13
	opWorkItemSubmitCapped           uint32 = 14
	opWorkItemSetTerminal            uint32 = 15
	opWorkItemGateApply              uint32 = 16
	opWorkItemSetPause               uint32 = 17
	opWorkItemClearPause             uint32 = 18
	opWorkItemClearPauseIf           uint32 = 19
	opWorkItemAddCost                uint32 = 20
	opWorkItemSetCostCap             uint32 = 21
	opWorkItemIncOverride            uint32 = 22
	opWorkItemDelete                 uint32 = 23
	opWorkItemReapStaleParks         uint32 = 24
	opWorkItemList                   uint32 = 25
	opWorkItemListLRU                uint32 = 26
	opLifecycleEventAdd              uint32 = 27
	opLifecycleEventList             uint32 = 28
	opStageAttemptInc                uint32 = 29
	opStageAttemptReset              uint32 = 30
	opStageAttemptGet                uint32 = 31
	opWorkItemRecordOutcome          uint32 = 32
)

const lifecycleListMax = 512

// submitCapped outcomes. These ride in the reply field rather than the wire
// status: a cap is an answer, not a failure.
const (
	submitCreated        = 0
	submitConcurrencyCap = 1
	submitRateCap        = 2
)

// The dispositions a recorded outcome may carry.
const (
	outcomePause    = 0
	outcomeTerminal = 1
	outcomeAdvance  = 2
)

// terminalWorkItemStates are the states a work item does not come back from.
var terminalWorkItemStates = []string{"accepted", "rejected", "abandoned"}

// reapableParkReasons are the backstop parks a stale-park sweep may abandon.
//
// Human-review parks (pending_human) and operator_paused are LEGITIMATE waits --
// a person is expected to act -- and are never reaped. The self-clearing parks
// (ci_pending, merge_pending, panel_*) are left to their own sweeps.
var reapableParkReasons = []string{
	"stuck", "wall_cap_exceeded", "turn_cap_exceeded", "budget_exceeded",
}

// workItemCols is the twenty-two-cell row a work-item read answers with, in the
// order the C's WI_COLS macro fixed.
var workItemCols = []col{
	text("work_item_id", false), text("repo", false), text("proposal_path", false),
	text("workflow_name", false), text("workflow_version", false), text("current_stage", false),
	text("state", false), text("mode", false), text("pause_reason", false),
	text("paused_state", false), text("content_hash", false), text("pr_ref", false),
	text("worktree", false), text("submitter", false), text("parent_id", false),
	real_("cum_cost_usd", false), real_("work_item_max_cost_usd", false),
	num("override_count", false), real_("reserved_cost_usd", false),
	text("reservation_state", false),
	text("source_path", false), {name: "updated_at", kind: kStamp},
}

// lifecycleEventCols is the eight-cell audit row.
var lifecycleEventCols = []col{
	num("id", false), text("stage", false), text("kind", false), text("actor", false),
	text("detail", false), text("content_hash", false), real_("cost_usd", false),
	{name: "created_at", kind: kStamp},
}

const (
	workItemCreateSQL = `INSERT INTO lifecycle_work_item
	        (work_item_id, repo, proposal_path, workflow_name, workflow_version,
	         current_stage, mode)
	    VALUES ($1, $2, $3, $4, $5, $6, $7)`

	workItemIDByProposalSQL = `SELECT work_item_id FROM lifecycle_work_item
	                            WHERE repo = $1 AND proposal_path = $2`

	workItemIDByPRRefSQL = `SELECT work_item_id FROM lifecycle_work_item
	                         WHERE pr_ref = $1 ORDER BY id LIMIT 1`

	workItemSetStageSQL = `UPDATE lifecycle_work_item
	                          SET current_stage = $2, content_hash = $3, updated_at = now()
	                        WHERE work_item_id = $1`

	workItemSetPRRefSQL = `UPDATE lifecycle_work_item
	                          SET pr_ref = $2, updated_at = now() WHERE work_item_id = $1`

	workItemSetWorktreeSQL = `UPDATE lifecycle_work_item
	                             SET worktree = $2, updated_at = now() WHERE work_item_id = $1`

	workItemSetSubmitterSQL = `UPDATE lifecycle_work_item
	                              SET submitter = $2, updated_at = now() WHERE work_item_id = $1`

	workItemSetParentSQL = `UPDATE lifecycle_work_item
	                           SET parent_id = $2, updated_at = now() WHERE work_item_id = $1`

	// One level suffices: slices are leaf runs. An orphaned non-terminal child
	// keeps being scheduled and re-dispatches delegates forever -- observed
	// live, an abandoned parent whose slice looped leaking a container a round.
	workItemAbandonChildrenSQL = `UPDATE lifecycle_work_item
	                                 SET state = 'abandoned', pause_reason = '',
	                                     paused_state = '', updated_at = now()
	                               WHERE parent_id = $1 AND state <> ALL($2)`

	// One aggregate pass: total children, those terminal-accepted, and those
	// terminal NOT accepted. The foreach gate advances only when every slice
	// merged; any failed child means one never will, so the parent parks.
	workItemChildCountsSQL = `SELECT count(*),
	                                 count(*) FILTER (WHERE state = 'accepted'),
	                                 count(*) FILTER (WHERE state IN ('rejected', 'abandoned'))
	                            FROM lifecycle_work_item WHERE parent_id = $1`

	countActiveBySubmitterSQL = `SELECT count(*) FROM lifecycle_work_item
	                              WHERE submitter = $1 AND state = 'active'`

	// The window is a bound interval. The C spliced the seconds into the
	// statement text with snprintf, because a TEXT timestamp gave it no
	// interval arithmetic.
	countRecentBySubmitterSQL = `SELECT count(*) FROM lifecycle_work_item
	                              WHERE submitter = $1
	                                AND created_at >= now() - make_interval(secs => $2)`

	workItemSetTerminalSQL = `UPDATE lifecycle_work_item
	                             SET state = $2, pause_reason = '', paused_state = '',
	                                 updated_at = now()
	                           WHERE work_item_id = $1`

	workItemSetPauseSQL = `UPDATE lifecycle_work_item
	                          SET pause_reason = $2, paused_state = $3, updated_at = now()
	                        WHERE work_item_id = $1`

	workItemClearPauseSQL = `UPDATE lifecycle_work_item
	                            SET pause_reason = '', paused_state = '', updated_at = now()
	                          WHERE work_item_id = $1`

	// Compare-and-clear: clear the pause ONLY while the row still shows the
	// (pause_reason, current_stage) the caller observed, so two drivers of the
	// same parked item cannot both win.
	workItemClearPauseIfSQL = `UPDATE lifecycle_work_item
	                              SET pause_reason = '', paused_state = '', updated_at = now()
	                            WHERE work_item_id = $1 AND pause_reason = $2
	                              AND current_stage = $3`

	workItemAddCostSQL = `UPDATE lifecycle_work_item
	                         SET cum_cost_usd = cum_cost_usd + $2, updated_at = now()
	                       WHERE work_item_id = $1`

	workItemSetCostCapSQL = `UPDATE lifecycle_work_item
	                            SET work_item_max_cost_usd = $2, updated_at = now()
	                          WHERE work_item_id = $1`

	// RETURNING rather than an update-then-read: the incremented value is the
	// answer, and reading it back separately could report another writer's.
	workItemIncOverrideSQL = `UPDATE lifecycle_work_item
	                             SET override_count = override_count + 1, updated_at = now()
	                           WHERE work_item_id = $1
	                       RETURNING override_count`

	// The backstop reaper. Only autonomous runs in a runaway/failure park are
	// eligible; a human-review park is a legitimate wait.
	workItemReapStaleParksSQL = `UPDATE lifecycle_work_item
	                                SET state = 'abandoned', updated_at = now()
	                              WHERE state = 'active' AND mode = 'autonomous'
	                                AND pause_reason = ANY($1)
	                                AND updated_at < now() - make_interval(secs => $2)`

	lifecycleEventAddSQL = `INSERT INTO lifecycle_event
	        (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
	    VALUES ($1, $2, $3, $4, $5, $6, $7)`

	// Oldest first: this is a history, and a caller reads it forwards.
	lifecycleEventListSQL = `SELECT %s FROM lifecycle_event
	                          WHERE work_item_id = $1 ORDER BY id ASC LIMIT $2`

	stageAttemptIncSQL = `INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts)
	                      VALUES ($1, $2, 1)
	                      ON CONFLICT (work_item_id, stage)
	                      DO UPDATE SET attempts = lifecycle_stage_attempt.attempts + 1
	                RETURNING attempts`

	stageAttemptResetSQL = `DELETE FROM lifecycle_stage_attempt
	                         WHERE work_item_id = $1 AND stage = $2`

	stageAttemptGetSQL = `SELECT attempts FROM lifecycle_stage_attempt
	                       WHERE work_item_id = $1 AND stage = $2`
)

// gateApplySQL is three statements sharing one guard.
//
// The SET clause varies by decision; the WHERE guard is identical in all three
// -- the row must still be parked exactly as the caller observed -- so it is
// written once.
const gateGuard = ` updated_at = now()
	              WHERE work_item_id = $2 AND current_stage = $3 AND content_hash = $4
	                AND pause_reason = 'pending_human'`

const (
	gateLoopbackSQL = `UPDATE lifecycle_work_item
	                      SET current_stage = $1, pause_reason = '', paused_state = '',` + gateGuard
	gateTerminalSQL = `UPDATE lifecycle_work_item
	                      SET state = $1, pause_reason = '', paused_state = '',` + gateGuard
	// Approve clears the pause only; the stage is unchanged. $1 is still bound
	// so the three statements take the same arguments.
	gateApproveSQL = `UPDATE lifecycle_work_item
	                     SET pause_reason = '', paused_state = '', content_hash = content_hash,` +
		gateGuard
)

func workItemCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	workflow := f[3]
	if workflow == "" {
		workflow = "build"
	}
	mode := f[6]
	if mode == "" {
		mode = "interactive"
	}
	if _, err := q.Exec(ctx, workItemCreateSQL, f[0], f[1], f[2], workflow, f[4], f[5], mode); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func workItemGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(workItemCols)+` FROM lifecycle_work_item WHERE work_item_id = $1`,
		workItemCols, f[0])
}

func lookupID(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	var id string
	switch err := q.QueryRow(ctx, sql, args...).Scan(&id); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{id}, nil
}

func workItemIDByProposal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return lookupID(ctx, q, workItemIDByProposalSQL, f[0], f[1])
}

func workItemIDByPRRef(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return lookupID(ctx, q, workItemIDByPRRefSQL, f[0])
}

// setField is the shape of the seven simple setters.
func setField(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	if _, err := q.Exec(ctx, sql, args...); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func workItemSetStage(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetStageSQL, f[0], f[1], f[2])
}

func workItemSetPRRef(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetPRRefSQL, f[0], f[1])
}

func workItemSetWorktree(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetWorktreeSQL, f[0], f[1])
}

// workItemSetSubmitter fails closed on a row that does not exist: an update
// matching nothing would leave the item unattributed while reporting success.
func workItemSetSubmitter(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, workItemSetSubmitterSQL, f[0], f[1])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

// workItemSetParent refuses a self-parent before the store does.
//
// The column carries a CHECK against it, so this is belt and braces -- but a
// constraint violation is an error the caller cannot act on, and "invalid" is.
func workItemSetParent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[0] == f[1] {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, workItemSetParentSQL, f[0], f[1])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

func workItemAbandonChildren(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, workItemAbandonChildrenSQL, f[0], terminalWorkItemStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func workItemChildCounts(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var total, accepted, failed int64
	if err := q.QueryRow(ctx, workItemChildCountsSQL, f[0]).
		Scan(&total, &accepted, &failed); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(total), store.I64toa(accepted), store.I64toa(failed),
	}, nil
}

func workItemCountActive(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, countActiveBySubmitterSQL, f[0])
}

func workItemCountRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	secs, ok := store.Atoi(f[1])
	if f[0] == "" || !ok || secs < 0 {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, countRecentBySubmitterSQL, f[0], secs)
}

// workItemSubmitCapped is op 14: create a work item, but only within the
// submitter's concurrency and rate caps.
//
// The cap check and the create are ONE transaction so concurrent submits from
// one principal serialise: the second blocks, and its count then sees the
// first's row. Otherwise a request burst exceeds the cap by its own size.
//
// Every step is fail-closed: a failed count or write rolls the whole thing
// back, so a partial, uncapped or unattributed run never escapes.
func workItemSubmitCapped(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, repo, proposalPath := f[0], f[1], f[2]
	workflowName, workflowVersion, startStage := f[3], f[4], f[5]
	submitter := f[6]
	maxActive, okMax := store.Atoi(f[7])
	rateMax, okRate := store.Atoi(f[8])
	rateSecs, okSecs := store.Atoi(f[9])
	if workItemID == "" || submitter == "" || !okMax || !okRate || !okSecs {
		return store.StatusInvalid, nil, nil
	}
	if startStage == "" {
		startStage = "intake"
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	capped := func(code int) (uint32, []string, error) {
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return store.StatusOK, []string{store.Itoa(code)}, nil
	}

	var active int64
	if err := tx.QueryRow(ctx, countActiveBySubmitterSQL, submitter).Scan(&active); err != nil {
		return 0, nil, err
	}
	if maxActive > 0 && active >= int64(maxActive) {
		return capped(submitConcurrencyCap)
	}
	if rateMax > 0 && rateSecs > 0 {
		var recent int64
		if err := tx.QueryRow(ctx, countRecentBySubmitterSQL, submitter, rateSecs).
			Scan(&recent); err != nil {
			return 0, nil, err
		}
		if recent >= int64(rateMax) {
			return capped(submitRateCap)
		}
	}

	workflow := workflowName
	if workflow == "" {
		workflow = "build"
	}
	if _, err := tx.Exec(ctx, workItemCreateSQL, workItemID, repo, proposalPath,
		workflow, workflowVersion, startStage, "autonomous"); err != nil {
		return 0, nil, err
	}
	tag, err := tx.Exec(ctx, workItemSetSubmitterSQL, workItemID, submitter)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}

	// A parity "create" event matching the interactive path, plus the
	// attributed, self-locating "submit" audit row.
	detail := "submit repo=" + repo + " proposal=" + proposalPath + " id=" + workItemID
	if _, err := tx.Exec(ctx, lifecycleEventAddSQL, workItemID, startStage, "create",
		submitter, workflowName, workflowVersion, 0.0); err != nil {
		return 0, nil, err
	}
	if _, err := tx.Exec(ctx, lifecycleEventAddSQL, workItemID, startStage, "submit",
		submitter, detail, "", 0.0); err != nil {
		return 0, nil, err
	}
	return capped(submitCreated)
}

func workItemSetTerminal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetTerminalSQL, f[0], f[1])
}

// workItemGateApply is op 16: apply a human's gate decision.
//
// 1 means applied, 0 means the precondition no longer holds -- the row moved
// since the caller looked at it, which a caller turns into a 409 rather than an
// error.
func workItemGateApply(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	workItemID, expectStage, expectHash := f[0], f[1], f[2]
	newStage, terminalState := f[3], f[4]
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}
	sql := gateApproveSQL
	variable := ""
	switch {
	case newStage != "":
		sql, variable = gateLoopbackSQL, newStage
	case terminalState != "":
		sql, variable = gateTerminalSQL, terminalState
	}
	tag, err := q.Exec(ctx, sql, variable, workItemID, expectStage, expectHash)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(tag.RowsAffected() == 1)}, nil
}

func workItemSetPause(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetPauseSQL, f[0], f[1], f[2])
}

func workItemClearPause(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemClearPauseSQL, f[0])
}

func workItemClearPauseIf(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, workItemClearPauseIfSQL, f[0], f[1], f[2])
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(tag.RowsAffected() == 1)}, nil
}

func workItemAddCost(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	cost, ok := store.Atof(f[1])
	if f[0] == "" || !ok || cost < 0 {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemAddCostSQL, f[0], cost)
}

func workItemSetCostCap(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	cap, ok := store.Atof(f[1])
	if f[0] == "" || !ok || cap < 0 {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, workItemSetCostCapSQL, f[0], cap)
}

func workItemIncOverride(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var count int64
	switch err := q.QueryRow(ctx, workItemIncOverrideSQL, f[0]).Scan(&count); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(count)}, nil
}

// workItemDelete removes the item and its history together.
//
// The three lifecycle tables have no foreign key between them, so this is three
// statements in one transaction: a partial failure must never leave an item row
// without its history, or history without its item.
func workItemDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	for _, sql := range []string{
		`DELETE FROM lifecycle_event WHERE work_item_id = $1`,
		`DELETE FROM lifecycle_stage_attempt WHERE work_item_id = $1`,
		`DELETE FROM lifecycle_work_item WHERE work_item_id = $1`,
	} {
		if _, err := q.Exec(ctx, sql, f[0]); err != nil {
			return 0, nil, err
		}
	}
	return store.StatusOK, nil, nil
}

func workItemReapStaleParks(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	grace, ok := store.Atoi(f[0])
	if !ok || grace <= 0 {
		return store.StatusOK, []string{"0"}, nil
	}
	tag, err := q.Exec(ctx, workItemReapStaleParksSQL, reapableParkReasons, grace)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func workItemList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	return readMany(ctx, q,
		`SELECT `+selectList(workItemCols)+` FROM lifecycle_work_item
		  ORDER BY id DESC LIMIT $1`, workItemCols, max)
}

// workItemListLRU is op 26: staleness first.
//
// work_item_id breaks the tie on updated_at, which keeps the order total: a
// fresh fan-out stamps every child in one batch, so same-instant rows are the
// normal case rather than the exception.
func workItemListLRU(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	return readMany(ctx, q,
		`SELECT `+selectList(workItemCols)+` FROM lifecycle_work_item
		  ORDER BY updated_at ASC, work_item_id ASC LIMIT $1`, workItemCols, max)
}

func lifecycleEventAdd(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	cost, ok := store.Atof(f[6])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, lifecycleEventAddSQL, f[0], f[1], f[2], f[3], f[4], f[5], cost)
}

func lifecycleEventList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if f[0] == "" || !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	return readMany(ctx, q,
		`SELECT `+selectList(lifecycleEventCols)+` FROM lifecycle_event
		  WHERE work_item_id = $1 ORDER BY id ASC LIMIT $2`,
		lifecycleEventCols, f[0], max)
}

func stageAttemptInc(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	var attempts int64
	if err := q.QueryRow(ctx, stageAttemptIncSQL, f[0], f[1]).Scan(&attempts); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(attempts)}, nil
}

func stageAttemptReset(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, stageAttemptResetSQL, f[0], f[1])
}

// stageAttemptGet answers 0 for a stage never attempted: the caller compares it
// against a limit, and "no row" is zero attempts.
func stageAttemptGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	var attempts int64
	switch err := q.QueryRow(ctx, stageAttemptGetSQL, f[0], f[1]).Scan(&attempts); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(attempts)}, nil
}

// workItemRecordOutcome is op 32: everything one engine step decided, in one
// transaction.
//
// Every STATE write is checked and any failure abandons the whole outcome. The
// audit writes are not: an event that fails to append is a missing line in a
// log, where a state write that fails is a work item nobody can explain.
func workItemRecordOutcome(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	workItemID := f[0]
	disposition, okDisp := store.Atoi(f[1])
	cost, okCost := store.Atof(f[2])
	state, pauseReason, pauseStage := f[3], f[4], f[5]
	nextStage, prRef, parkReason := f[6], f[7], f[8]
	nodeID, eventKind, eventDetail, eventHash := f[9], f[10], f[11], f[12]
	abandonChildren, okAbandon := store.Atoi(f[13])
	if workItemID == "" || !okDisp || !okCost || !okAbandon {
		return store.StatusInvalid, nil, nil
	}
	switch disposition {
	case outcomePause, outcomeTerminal, outcomeAdvance:
	default:
		return store.StatusInvalid, nil, nil
	}

	if cost > 0 {
		if _, err := q.Exec(ctx, workItemAddCostSQL, workItemID, cost); err != nil {
			return 0, nil, err
		}
	}

	switch disposition {
	case outcomeTerminal:
		if _, err := q.Exec(ctx, workItemSetTerminalSQL, workItemID, state); err != nil {
			return 0, nil, err
		}
		if abandonChildren != 0 {
			// A no-op for a leaf slice, and not checked for the same reason:
			// having no children is not a failure.
			_, _ = q.Exec(ctx, workItemAbandonChildrenSQL, workItemID, terminalWorkItemStates)
		}
	case outcomePause:
		if _, err := q.Exec(ctx, workItemSetPauseSQL, workItemID, pauseReason, pauseStage); err != nil {
			return 0, nil, err
		}
	default:
		if _, err := q.Exec(ctx, workItemSetStageSQL, workItemID, nextStage, eventHash); err != nil {
			return 0, nil, err
		}
		if prRef != "" {
			if _, err := q.Exec(ctx, workItemSetPRRefSQL, workItemID, prRef); err != nil {
				return 0, nil, err
			}
		}
	}

	_, _ = q.Exec(ctx, lifecycleEventAddSQL, workItemID, nodeID, eventKind, "engine",
		eventDetail, eventHash, cost)

	if disposition == outcomeAdvance && parkReason != "" {
		// The step advanced and its own cost crossed the cap, so it parks
		// before the next stage and a human resumes with --budget-bump.
		if _, err := q.Exec(ctx, workItemSetPauseSQL, workItemID, parkReason, nextStage); err != nil {
			return 0, nil, err
		}
		_, _ = q.Exec(ctx, lifecycleEventAddSQL, workItemID, nextStage, "pause", "engine",
			parkReason, "", 0.0)
	}
	return store.StatusOK, nil, nil
}
