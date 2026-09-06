package families

import (
	"context"
	"encoding/json"
	"math"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// A run's transitions. Each one is a guarded UPDATE plus its audit event in one
// transaction: recording history for a transition that did not happen is worse
// than recording nothing.
const (
	opWFEMove                   uint32 = 52
	opWFERecordRetry            uint32 = 53
	opWFEParkWithDetail         uint32 = 54
	opWFEResume                 uint32 = 55
	opWFEFinish                 uint32 = 56
	opWFEResolveGate            uint32 = 61
	opWFERejectGate             uint32 = 62
	opWFEParkRunnerFailure      uint32 = 63
	opWFERecoverLostReplay      uint32 = 64
	opWFERecordRequestedChanges uint32 = 65
	opWFEClaimFrozenCreates     uint32 = 66
	opWFECreateWorkItem         uint32 = 67
)

// The frozen-create wire is fixed-width: two identifiers, sixty-four
// (path, hash) slots, and a count.
const (
	frozenMax        = 64
	offFrozenCreates = 2
	offFrozenCount   = offFrozenCreates + frozenMax*2 // 130
	frozenFields     = offFrozenCount + 1             // 131
)

// operatorResumable are the pauses a human may release by hand.
//
// The list is the point. Most pauses are lifecycle-owned: the engine parked the
// run and the engine unparks it when the condition clears, and a human clearing
// it instead skips the check that was about to run. These are different -- they
// exist BECAUSE a human has to decide something, so a human is the only thing
// that can release them.
var operatorResumable = map[string]bool{
	"manual": true, "wall_cap": true, "turn_cap": true, "retry_limit": true,
	"convergence_limit": true, "convergence_no_progress": true, "budget_cap": true,
	"fanout_limit": true, "workflow_definition_invalid": true,
	"workflow_block_unavailable": true, "delegate_failed": true,
	"replay_unrecoverable": true, "base_integration_conflict": true,
	"request_unimplementable": true,
}

// clearReservation is what every transition clears: the reservation is spent
// once the invocation it authorised has ended.
const clearReservation = `reserved_cost_usd = 0, reservation_state = '',
	    reservation_owner = '', reservation_lease_until = NULL`

const (
	wfeMoveSQL = `UPDATE lifecycle_work_item
	                 SET current_stage = $1, content_hash = $2, pause_reason = '',
	                     paused_state = '', cum_cost_usd = cum_cost_usd + $3,
	                     ` + clearReservation + `, updated_at = now()
	               WHERE work_item_id = $4 AND current_stage = $5
	                 AND state = 'active' AND pause_reason = ''`

	wfeRetryMoveSQL = `UPDATE lifecycle_work_item
	                      SET current_stage = $1, pause_reason = $2, paused_state = $3,
	                          cum_cost_usd = cum_cost_usd + $4,
	                          ` + clearReservation + `, updated_at = now()
	                    WHERE work_item_id = $5 AND current_stage = $6
	                      AND state = 'active' AND pause_reason = ''`

	wfeParkWithDetailSQL = `UPDATE lifecycle_work_item
	                           SET pause_reason = $1, paused_state = $2,
	                               cum_cost_usd = cum_cost_usd + $3,
	                               ` + clearReservation + `, updated_at = now()
	                         WHERE work_item_id = $4 AND current_stage = $5
	                           AND state = 'active' AND pause_reason = ''`

	wfeResumeReadSQL = `SELECT current_stage, pause_reason FROM lifecycle_work_item
	                     WHERE work_item_id = $1 AND state = 'active'`

	wfeResumeClearSQL = `UPDATE lifecycle_work_item
	                        SET pause_reason = '', paused_state = '', updated_at = now()
	                      WHERE work_item_id = $1 AND state = 'active'`

	// Active and in the expected stage, but NOT "unpaused": a run parked for a
	// human is still finishable by the decision that unparks it.
	wfeFinishSQL = `UPDATE lifecycle_work_item
	                   SET state = $1, pause_reason = '', paused_state = '', content_hash = $2,
	                       cum_cost_usd = cum_cost_usd + $3,
	                       ` + clearReservation + `, updated_at = now()
	                 WHERE work_item_id = $4 AND current_stage = $5 AND state = 'active'`

	wfeResolveGateSQL = `UPDATE lifecycle_work_item
	                        SET current_stage = $1, pause_reason = '', paused_state = '',
	                            content_hash = $2, updated_at = now()
	                      WHERE work_item_id = $3 AND current_stage = $4
	                        AND state = 'active' AND pause_reason = 'human_gate'`

	wfeRejectGateSQL = `UPDATE lifecycle_work_item
	                       SET state = 'rejected', pause_reason = '', paused_state = '',
	                           content_hash = $1, updated_at = now()
	                     WHERE work_item_id = $2 AND current_stage = $3
	                       AND state = 'active' AND pause_reason = 'human_gate'`

	wfeConvergenceSeenSQL = `SELECT artifact_hash, feedback_hash, identical_repeats, blocker_set
	                           FROM wfe_convergence WHERE work_item_id = $1 AND gate = $2`

	wfeConvergenceObserveSQL = `INSERT INTO wfe_convergence
	        (work_item_id, gate, artifact_hash, feedback_hash, identical_repeats, blocker_set)
	    VALUES ($1, $2, $3, $4, $5, $6)
	    ON CONFLICT (work_item_id, gate) DO UPDATE SET
	        artifact_hash = EXCLUDED.artifact_hash,
	        feedback_hash = EXCLUDED.feedback_hash,
	        identical_repeats = EXCLUDED.identical_repeats,
	        blocker_set = EXCLUDED.blocker_set,
	        updated_at = now()`

	wfeConvergenceParkSQL = `UPDATE lifecycle_work_item
	                            SET current_stage = $1, pause_reason = $2, paused_state = $3,
	                                content_hash = $4, cum_cost_usd = cum_cost_usd + $5,
	                                ` + clearReservation + `, updated_at = now()
	                          WHERE work_item_id = $6`

	wfeConvergenceLoopSQL = `UPDATE lifecycle_work_item
	                            SET current_stage = $1, pause_reason = '', paused_state = '',
	                                content_hash = $2, cum_cost_usd = cum_cost_usd + $3,
	                                ` + clearReservation + `, updated_at = now()
	                          WHERE work_item_id = $4`

	// Only an active child of this parent may freeze. The family transaction
	// lock serializes sibling claims; locking different child rows alone cannot
	// prevent both siblings from observing an empty claim set.
	wfeFrozenOwnerLockSQL = `SELECT 1 FROM lifecycle_work_item
	                          WHERE work_item_id = $1 AND parent_id = $2 AND state = 'active'
	                          FOR UPDATE`

	// A sibling that froze this path with DIFFERENT content. Identical content
	// is two slices agreeing, and is allowed to coexist.
	wfeFrozenClashSQL = `SELECT work_item_id FROM wfe_frozen_create
	                      WHERE parent_id = $1 AND path = $2 AND work_item_id <> $3
	                        AND content_hash <> $4
	                      ORDER BY work_item_id LIMIT 1`

	wfeFrozenInsertSQL = `INSERT INTO wfe_frozen_create
	                          (parent_id, path, work_item_id, content_hash)
	                      VALUES ($1, $2, $3, $4)
	                      ON CONFLICT (parent_id, path, work_item_id)
	                      DO UPDATE SET content_hash = EXCLUDED.content_hash, updated_at = now()`

	wfeCreateRootSQL = `INSERT INTO lifecycle_work_item
	        (work_item_id, repo, proposal_path, workflow_name, workflow_version,
	         current_stage, mode, submitter, parent_id, source_path, work_item_max_cost_usd)
	    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)`

	// Parent eligibility spelled as INSERT..SELECT FROM the parent rather than
	// a check followed by an insert: a concurrent StopTree either includes this
	// child or wins first and makes the SELECT yield nothing. There is no
	// window in which a child is created under a parent that has just stopped.
	wfeCreateChildSQL = `INSERT INTO lifecycle_work_item
	        (work_item_id, repo, proposal_path, workflow_name, workflow_version,
	         current_stage, mode, submitter, parent_id, source_path, work_item_max_cost_usd)
	    SELECT $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11
	      FROM lifecycle_work_item parent
	     WHERE parent.work_item_id = $12 AND parent.state = 'active'`

	wfeEventSQL = `INSERT INTO lifecycle_event
	        (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
	    VALUES ($1, $2, $3, $4, $5, $6, $7)`
)

// costIsSane refuses a non-finite or negative cost at the durable boundary: a
// NaN in a cost column makes every later budget comparison fail.
func costIsSane(v float64) bool {
	return v >= 0 && !math.IsNaN(v) && !math.IsInf(v, 0)
}

// dropStageAttempts clears a stage's attempt counter.
func dropStageAttempts(ctx context.Context, tx store.Tx, workItemID, stage string) error {
	_, err := tx.Exec(ctx, stageAttemptResetSQL, workItemID, stage)
	return err
}

// wfeMove is op 52: advance or loop a run to another stage.
func wfeMove(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, fromStage, toStage, kind := f[0], f[1], f[2], f[3]
	detail, contentHash := f[4], f[5]
	cost, okCost := store.Atof(f[6])
	if workItemID == "" || fromStage == "" || toStage == "" || kind == "" ||
		!okCost || !costIsSane(cost) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	tag, err := tx.Exec(ctx, wfeMoveSQL, toStage, contentHash, cost, workItemID, fromStage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		// Someone else moved it, or it is paused or terminal. Recording the
		// event anyway would write history for a transition that did not
		// happen.
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, fromStage, kind, "go-wfe",
		detail, contentHash, cost); err != nil {
		return 0, nil, err
	}

	if kind != "loop" {
		// Clear the stage being LEFT, not the one being entered. The stage
		// completed, so a later revisit starts with a fresh budget. Clearing
		// the one being entered resets the counter that bounds a refinement
		// loop: a gate that loops back to its author is re-entered by the
		// author's own advance, and that advance was wiping the gate's
		// accumulated attempts, so the cap could never be reached. Observed
		// before the fix: a plan gate at 63 loops against a cap of 20.
		if err := dropStageAttempts(ctx, tx, workItemID, fromStage); err != nil {
			return 0, nil, err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeRecordRetry is op 53: another attempt at a stage, or a park if it has run
// out of them. 1 means parked.
func wfeRecordRetry(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, toStage, detail := f[0], f[1], f[2], f[3]
	maxAttempts, okMax := store.Atoi(f[4])
	cost, okCost := store.Atof(f[5])
	if workItemID == "" || stage == "" || !okMax || maxAttempts < 1 ||
		!okCost || !costIsSane(cost) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var attempts int64
	if err := tx.QueryRow(ctx, stageAttemptIncSQL, workItemID, stage).Scan(&attempts); err != nil {
		return 0, nil, err
	}

	// Out of attempts: park in place rather than advancing, and say why.
	parked := attempts >= int64(maxAttempts)
	reason, pausedState, target := "", "", toStage
	if parked {
		reason, pausedState, target = "retry_limit", stage, stage
	}
	if target == "" {
		target = stage
	}

	tag, err := tx.Exec(ctx, wfeRetryMoveSQL, target, reason, pausedState, cost, workItemID, stage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}

	kind := "loop"
	if parked {
		kind = "pause"
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, kind, "go-wfe",
		detail, "", cost); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(parked)}, nil
}

// wfeParkWithDetail is op 54.
func wfeParkWithDetail(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, reason, detail := f[0], f[1], f[2], f[3]
	cost, okCost := store.Atof(f[4])
	if workItemID == "" || !okCost || !costIsSane(cost) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	tag, err := tx.Exec(ctx, wfeParkWithDetailSQL, reason, stage, cost, workItemID, stage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, "pause", "go-wfe",
		detail, "", cost); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeResume is op 55: an operator releases a park by hand.
func wfeResume(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID := f[0]
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	// The stage it is parked in and why, read INSIDE the transaction: the event
	// this records names the reason being cleared, and reading it outside would
	// let it change underneath.
	var stage, reason string
	switch err := tx.QueryRow(ctx, wfeResumeReadSQL, workItemID).Scan(&stage, &reason); {
	case store.IsNoRows(err):
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	if reason == "" {
		// Not paused. Clearing nothing and recording a resume would put a
		// resume event in the history of a run that never stopped.
		return store.StatusFailed, nil, nil
	}
	if !operatorResumable[reason] {
		return store.StatusFailed, nil, nil
	}

	tag, err := tx.Exec(ctx, wfeResumeClearSQL, workItemID)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}

	// Only a retry_limit resume starts the stage over. A human resume there is
	// an explicit request for another bounded repair cycle; keeping the
	// exhausted count made the very next failed repair park again, reducing
	// recovery to one attempt per manual resume. Other reasons keep their
	// counts, because nothing about them says the attempts should be forgiven.
	if reason == "retry_limit" {
		if err := dropStageAttempts(ctx, tx, workItemID, stage); err != nil {
			return 0, nil, err
		}
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, "resume", "operator",
		reason, "", 0.0); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeFinish is op 56.
func wfeFinish(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, state, detail, contentHash := f[0], f[1], f[2], f[3], f[4]
	cost, okCost := store.Atof(f[5])
	if workItemID == "" || state == "" || !okCost || !costIsSane(cost) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	tag, err := tx.Exec(ctx, wfeFinishSQL, state, contentHash, cost, workItemID, stage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, "terminal", "go-wfe",
		detail, contentHash, cost); err != nil {
		return 0, nil, err
	}
	// A finished run's frozen creates are spent: they exist to make a re-run
	// reuse the children it already made, and there will be no re-run.
	if _, err := tx.Exec(ctx,
		`DELETE FROM wfe_frozen_create WHERE work_item_id = $1`, workItemID); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeResolveGate is op 61: a human sends a gated run onward.
func wfeResolveGate(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, fromStage, toStage, decision, contentHash := f[0], f[1], f[2], f[3], f[4]
	if workItemID == "" || toStage == "" {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	tag, err := tx.Exec(ctx, wfeResolveGateSQL, toStage, contentHash, workItemID, fromStage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, fromStage, "gate", "operator",
		decision, contentHash, 0.0); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeRejectGate is op 62.
func wfeRejectGate(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, contentHash := f[0], f[1], f[2]
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	tag, err := tx.Exec(ctx, wfeRejectGateSQL, contentHash, workItemID, stage)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, "terminal", "operator",
		"rejected", contentHash, 0.0); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeParkRunnerFailure is op 63: a runner failed, parked with whatever is known
// about what it cost.
//
// Three cases, and the difference between them is money:
//
//	not dispatched          the invocation never left, so nothing was spent and
//	                        the reservation is dropped outright.
//	dispatched, cost known  the measured cost is committed and the reservation
//	                        is dropped: the story is complete.
//	dispatched, cost NOT    the invocation crossed the provider boundary and may
//	known                   have spent an unknown amount. The known prefix is
//	                        committed and the REST of the authorization is
//	                        retained as 'unresolved' -- releasing it would let
//	                        the tree hand out money that may already be gone.
func wfeParkRunnerFailure(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, owner, reason, detail := f[0], f[1], f[2], f[3], f[4]
	dispatched, okDispatched := store.Atoi(f[5])
	costKnown, okKnown := store.Atoi(f[6])
	actual, okActual := store.Atof(f[7])
	if workItemID == "" || stage == "" || owner == "" || reason == "" ||
		!okDispatched || !okKnown || !okActual || !costIsSane(actual) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	state := ""
	amount, eventCost := 0.0, 0.0
	switch {
	case dispatched != 0 && costKnown != 0:
		eventCost = actual
	case dispatched != 0:
		state = reservationUnresolved
		// Whatever is still authorised stays authorised.
		if err := tx.QueryRow(ctx,
			`SELECT reserved_cost_usd FROM lifecycle_work_item
			  WHERE work_item_id = $1 AND reservation_owner = $2
			    AND reservation_state = ANY($3)`,
			workItemID, owner, reservationReplaceable).Scan(&amount); err != nil &&
			!store.IsNoRows(err) {
			return 0, nil, err
		}
	}

	if _, err := tx.Exec(ctx, `UPDATE lifecycle_work_item
	                              SET pause_reason = $1, paused_state = $2,
	                                  cum_cost_usd = cum_cost_usd + $3,
	                                  reserved_cost_usd = $4, reservation_state = $5,
	                                  reservation_owner = CASE WHEN $5 = '' THEN ''
	                                                           ELSE reservation_owner END,
	                                  reservation_lease_until = CASE WHEN $5 = '' THEN NULL
	                                                                 ELSE reservation_lease_until END,
	                                  updated_at = now()
	                            WHERE work_item_id = $6`,
		reason, stage, eventCost, amount, state, workItemID); err != nil {
		return 0, nil, err
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, "pause", "go-wfe",
		detail, "", eventCost); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeRecoverLostReplay is op 64: the replay of an invocation lost its result.
//
// 1 means the caller may re-dispatch fresh. 0 means it may not: the cost was
// already reconciled, so re-dispatching would spend it twice, and the run parks
// for a human instead.
func wfeRecoverLostReplay(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, stage, owner := f[0], f[1], f[2]
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var state, holder string
	var amount float64
	switch err := tx.QueryRow(ctx, loadReservationSQL, workItemID).
		Scan(&amount, &state, &holder); {
	case store.IsNoRows(err):
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if holder != owner {
		// Someone else's invocation. Recovering it would decide the fate of
		// money this caller did not authorise.
		return store.StatusFailed, nil, nil
	}

	var updateSQL, eventKind, eventDetail string
	eventCost := 0.0
	redispatch := false
	switch state {
	case reservationUnresolved:
		updateSQL = `UPDATE lifecycle_work_item
		                SET ` + clearReservation + `, updated_at = now()
		              WHERE work_item_id = $1 AND reservation_owner = $2
		                AND reservation_state = 'unresolved'`
		eventKind, eventDetail = "redispatch", "replay result lost; re-dispatching fresh"
		redispatch = true
	case reservationActual:
		updateSQL = `UPDATE lifecycle_work_item
		                SET cum_cost_usd = cum_cost_usd + reserved_cost_usd,
		                    ` + clearReservation + `,
		                    pause_reason = 'replay_unrecoverable',
		                    paused_state = current_stage, updated_at = now()
		              WHERE work_item_id = $1 AND reservation_owner = $2
		                AND reservation_state = 'actual'`
		eventKind = "pause"
		eventDetail = "replay_unrecoverable: reconciled result lost, parked for human"
		eventCost = amount
	default:
		return store.StatusFailed, nil, nil
	}

	tag, err := tx.Exec(ctx, updateSQL, workItemID, owner)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, stage, eventKind, "go-wfe",
		eventDetail, "", eventCost); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(redispatch)}, nil
}

// wfeRecordRequestedChanges is op 65: a reviewer asked for changes, and the
// question is whether the loop is still making progress.
type convergencePayloadV1 struct {
	Version    int    `json:"version"`
	Mode       string `json:"mode"`
	Summary    string `json:"summary"`
	BlockerSet string `json:"blocker_set"`
}

// convergenceFields accepts the versioned payload emitted by the workflow
// engine while preserving the old plain-text field for every other client.
func convergenceFields(raw string) (summary, blockerSet, mode string) {
	var payload convergencePayloadV1
	if json.Unmarshal([]byte(raw), &payload) != nil || payload.Version != 1 {
		return raw, "", "legacy"
	}
	if canonicalBlockerSet(payload.BlockerSet) {
		blockerSet = payload.BlockerSet
	}
	mode = strings.ToLower(strings.TrimSpace(payload.Mode))
	if mode != "enforce" {
		mode = "observe"
	}
	return payload.Summary, blockerSet, mode
}

func canonicalBlockerSet(raw string) bool {
	if raw == "" || len(raw) > 4160 {
		return false
	}
	parts := strings.Split(raw, ",")
	if len(parts) > 64 {
		return false
	}
	previous := ""
	for _, part := range parts {
		if len(part) != 64 || (previous != "" && part <= previous) {
			return false
		}
		for _, c := range part {
			if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
				return false
			}
		}
		previous = part
	}
	return true
}

func blockerSetRelationship(previous, current string) string {
	prior := make(map[string]struct{})
	for _, fingerprint := range strings.Split(previous, ",") {
		prior[fingerprint] = struct{}{}
	}
	present := make(map[string]struct{})
	for _, fingerprint := range strings.Split(current, ",") {
		present[fingerprint] = struct{}{}
	}
	currentInsidePrior := true
	for fingerprint := range present {
		if _, ok := prior[fingerprint]; !ok {
			currentInsidePrior = false
			break
		}
	}
	priorInsideCurrent := true
	for fingerprint := range prior {
		if _, ok := present[fingerprint]; !ok {
			priorInsideCurrent = false
			break
		}
	}
	switch {
	case currentInsidePrior && len(present) < len(prior):
		return "progress"
	case currentInsidePrior && priorInsideCurrent:
		return "stalled"
	case priorInsideCurrent:
		return "regression"
	default:
		return "churn"
	}
}

// Two separate limits, because they catch different failures: max_iterations
// bounds how many rounds a gate may take at all, and max_identical catches
// consecutive rounds without demonstrated blocker-set shrinkage. Legacy callers
// without a structured blocker set retain exact artifact+feedback comparison.
func wfeRecordRequestedChanges(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, gate, planStage := f[0], f[1], f[2]
	planHash, feedbackHash := f[3], f[4]
	unresolved, blockerSet, convergenceMode := convergenceFields(f[5])
	maxIterations, okIter := store.Atoi(f[6])
	maxIdentical, okIdent := store.Atoi(f[7])
	cost, okCost := store.Atof(f[8])
	if workItemID == "" || gate == "" || !okIter || !okIdent || !okCost || !costIsSane(cost) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	// Only an active run can be routed anywhere.
	var state string
	switch err := tx.QueryRow(ctx,
		`SELECT state FROM lifecycle_work_item WHERE work_item_id = $1`, workItemID).
		Scan(&state); {
	case store.IsNoRows(err):
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if state != "active" {
		return store.StatusFailed, nil, nil
	}

	var attempts int64
	if err := tx.QueryRow(ctx, stageAttemptIncSQL, workItemID, gate).Scan(&attempts); err != nil {
		return 0, nil, err
	}

	// Has this gate seen exactly this artifact against exactly this feedback
	// before? Missing is not an error: the first round has nothing to compare.
	repeats := int64(1)
	var oldPlan, oldFeedback, oldBlockerSet string
	var oldRepeats int64
	relationship := ""
	switch err := tx.QueryRow(ctx, wfeConvergenceSeenSQL, workItemID, gate).
		Scan(&oldPlan, &oldFeedback, &oldRepeats, &oldBlockerSet); {
	case err == nil:
		structuredComparison := convergenceMode == "enforce" && blockerSet != "" && oldBlockerSet != ""
		if blockerSet != "" && oldBlockerSet != "" {
			relationship = blockerSetRelationship(oldBlockerSet, blockerSet)
			if structuredComparison && relationship != "progress" {
				repeats = oldRepeats + 1
			}
		}
		// Enforced structured comparison is authoritative only when both rounds
		// supplied a valid set. A producer outage or rollout gap must retain the
		// pre-existing exact-hash safety rule instead of silently resetting the
		// no-progress counter.
		if !structuredComparison && oldPlan == planHash && oldFeedback == feedbackHash {
			repeats = oldRepeats + 1
		}
	case !store.IsNoRows(err):
		return 0, nil, err
	}

	if _, err := tx.Exec(ctx, wfeConvergenceObserveSQL, workItemID, gate,
		planHash, feedbackHash, repeats, blockerSet); err != nil {
		return 0, nil, err
	}

	parked, pauseReason := false, ""
	switch {
	case repeats >= int64(maxIdentical):
		parked, pauseReason = true, "convergence_no_progress"
	case attempts >= int64(maxIterations):
		parked, pauseReason = true, "convergence_limit"
	}

	if parked {
		if _, err := tx.Exec(ctx, wfeConvergenceParkSQL, planStage, pauseReason, gate,
			planHash, cost, workItemID); err != nil {
			return 0, nil, err
		}
		// The detail names what is still unresolved when the reviewer said so:
		// the person reading this event is deciding whether to intervene, and
		// "convergence_limit" alone does not tell them what to look at.
		detail := pauseReason
		if unresolved != "" {
			detail = pauseReason + " after " + store.I64toa(attempts) +
				" rounds; still unresolved: " + unresolved
		}
		if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, gate, "pause", "go-wfe",
			detail, planHash, cost); err != nil {
			return 0, nil, err
		}
	} else {
		if _, err := tx.Exec(ctx, wfeConvergenceLoopSQL, planStage, planHash, cost,
			workItemID); err != nil {
			return 0, nil, err
		}
		detail := "requested_changes"
		if relationship != "" {
			detail += ": blocker_set_" + relationship + " mode_" + convergenceMode
		} else if blockerSet == "" {
			detail += ": blocker_set_unavailable"
		}
		if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, gate, "loop", "go-wfe",
			detail, planHash, cost); err != nil {
			return 0, nil, err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(attempts), store.I64toa(repeats),
		store.Btoa(parked), pauseReason,
	}, nil
}

// wfeClaimFrozenCreates is op 66: a slice claims the child paths it will create.
//
// A conflict answers with the clashing path rather than failing: the caller
// reports which two slices disagreed. The whole claim rolls back on one clash,
// because a partial path set would let the loser's earlier paths stand while
// its later ones were refused.
func wfeClaimFrozenCreates(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	parentID, workItemID := f[0], f[1]
	count, okCount := store.Atoi(f[offFrozenCount])
	if parentID == "" || workItemID == "" || !okCount || count < 0 || count > frozenMax {
		return store.StatusInvalid, nil, nil
	}
	noConflict := []string{"", "", ""}
	if count == 0 {
		return store.StatusOK, noConflict, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var owned int
	switch err := tx.QueryRow(ctx, wfeFrozenOwnerLockSQL, workItemID, parentID).Scan(&owned); {
	case store.IsNoRows(err):
		// Not an active child of this parent, so not entitled to freeze.
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	for i := 0; i < count; i++ {
		path := f[offFrozenCreates+i*2]
		hash := f[offFrozenCreates+i*2+1]
		if path == "" || hash == "" {
			return store.StatusInvalid, nil, nil
		}
		var existing string
		switch err := tx.QueryRow(ctx, wfeFrozenClashSQL, parentID, path, workItemID, hash).
			Scan(&existing); {
		case err == nil:
			// Roll back by returning without committing.
			return store.StatusOK, []string{path, existing, workItemID}, nil
		case !store.IsNoRows(err):
			return 0, nil, err
		}
		if _, err := tx.Exec(ctx, wfeFrozenInsertSQL, parentID, path, workItemID, hash); err != nil {
			return 0, nil, err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, noConflict, nil
}

// wfeCreateWorkItem is op 67: the engine's create, which is not the daemon's.
//
// Three things happen here that the daemon's create does not do, and all three
// are in the same transaction as the insert:
//
//	the admission cap   counted over ACTIVE ROOTS, so a fan-out of slices does
//	                    not consume the operator's concurrency budget.
//	parent eligibility  an INSERT..SELECT from the parent rather than a check
//	                    then an insert, so there is no window in which a child
//	                    is created under a parent that has just stopped.
//	the create event    so a run's history starts at its creation rather than
//	                    at its first advance.
//
// 1 means the admission cap refused it -- backpressure the caller reports as
// such, not an error.
func wfeCreateWorkItem(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, repo, proposalPath := f[0], f[1], f[2]
	workflowName, workflowVersion, startStage := f[3], f[4], f[5]
	mode, submitter, parentID, sourcePath := f[6], f[7], f[8], f[9]
	maxCost, okCost := store.Atof(f[10])
	rootCap, okCap := store.Atoi(f[11])
	if workItemID == "" || proposalPath == "" || workflowName == "" || startStage == "" ||
		!okCost || !costIsSane(maxCost) || !okCap {
		return store.StatusInvalid, nil, nil
	}
	if mode == "" {
		mode = "autonomous"
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	if rootCap > 0 && parentID == "" {
		var activeRoots int64
		if err := tx.QueryRow(ctx, wfeActiveRootCountSQL).Scan(&activeRoots); err != nil {
			return 0, nil, err
		}
		if activeRoots >= int64(rootCap) {
			if err := tx.Commit(ctx); err != nil {
				return 0, nil, err
			}
			return store.StatusOK, []string{"1"}, nil
		}
	}

	args := []any{workItemID, repo, proposalPath, workflowName, workflowVersion,
		startStage, mode, submitter, parentID, sourcePath, maxCost}
	sql := wfeCreateRootSQL
	if parentID != "" {
		sql = wfeCreateChildSQL
		args = append(args, parentID)
	}
	tag, err := tx.Exec(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		// For a child, the parent stopped between the caller's decision and
		// this insert. For a root, the insert simply did not land.
		if parentID != "" {
			return store.StatusOK, []string{"2"}, nil
		}
		return store.StatusFailed, nil, nil
	}
	if _, err := tx.Exec(ctx, wfeEventSQL, workItemID, startStage, "create", "go-wfe",
		workflowName, "", 0.0); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{"0"}, nil
}

// Lifecycle is the family, ready to be bound to kind 11792.
var Lifecycle = store.Family{
	Name:  "lifecycle",
	Event: EventLifecycle,
	Stage: StageLifecycle,
	// SQLite's old single-writer transactions implicitly serialized admission,
	// tree mutation and sibling claims. PostgreSQL READ COMMITTED does not.
	// Scope this invariant to the lifecycle schema, across all module processes,
	// before their first read; neither memory traffic nor other schemas wait.
	TransactionLockSQL: "SELECT pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtext(current_schema()), pg_catalog.hashtext('aimee:lifecycle'))",
	Ops: map[uint32]store.Op{
		opWorkItemCreate:                 {Name: "work_item_create", Args: 7, Tx: true, Run: workItemCreate},
		opWorkItemGet:                    {Name: "work_item_get", Cells: 22, Args: 1, Run: workItemGet},
		opWorkItemIDByProposal:           {Name: "work_item_id_by_proposal", Args: 2, Run: workItemIDByProposal},
		opWorkItemIDByPRRef:              {Name: "work_item_id_by_pr_ref", Args: 1, Run: workItemIDByPRRef},
		opWorkItemSetStage:               {Name: "work_item_set_stage", Args: 3, Tx: true, Run: workItemSetStage},
		opWorkItemSetPRRef:               {Name: "work_item_set_pr_ref", Args: 2, Tx: true, Run: workItemSetPRRef},
		opWorkItemSetWorktree:            {Name: "work_item_set_worktree", Args: 2, Tx: true, Run: workItemSetWorktree},
		opWorkItemSetSubmitter:           {Name: "work_item_set_submitter", Args: 2, Tx: true, Run: workItemSetSubmitter},
		opWorkItemSetParent:              {Name: "work_item_set_parent", Args: 2, Tx: true, Run: workItemSetParent},
		opWorkItemAbandonChildren:        {Name: "work_item_abandon_children", Args: 1, Tx: true, Run: workItemAbandonChildren},
		opWorkItemChildCounts:            {Name: "work_item_child_counts", Cells: 3, Args: 1, Run: workItemChildCounts},
		opWorkItemCountActiveBySubmitter: {Name: "work_item_count_active_by_submitter", Args: 1, Run: workItemCountActive},
		opWorkItemCountRecentBySubmitter: {Name: "work_item_count_recent_by_submitter", Args: 2, Run: workItemCountRecent},
		opWorkItemSubmitCapped:           {Name: "work_item_submit_capped", Args: 10, RunDB: workItemSubmitCapped},
		opWorkItemSetTerminal:            {Name: "work_item_set_terminal", Args: 2, Tx: true, Run: workItemSetTerminal},
		opWorkItemGateApply:              {Name: "work_item_gate_apply", Args: 5, Tx: true, Run: workItemGateApply},
		opWorkItemSetPause:               {Name: "work_item_set_pause", Args: 3, Tx: true, Run: workItemSetPause},
		opWorkItemClearPause:             {Name: "work_item_clear_pause", Args: 1, Tx: true, Run: workItemClearPause},
		opWorkItemClearPauseIf:           {Name: "work_item_clear_pause_if", Args: 3, Tx: true, Run: workItemClearPauseIf},
		opWorkItemAddCost:                {Name: "work_item_add_cost", Args: 2, Tx: true, Run: workItemAddCost},
		opWorkItemSetCostCap:             {Name: "work_item_set_cost_cap", Args: 2, Tx: true, Run: workItemSetCostCap},
		opWorkItemIncOverride:            {Name: "work_item_inc_override", Args: 1, Tx: true, Run: workItemIncOverride},
		opWorkItemDelete:                 {Name: "work_item_delete", Args: 1, Tx: true, Run: workItemDelete},
		opWorkItemReapStaleParks:         {Name: "work_item_reap_stale_parks", Args: 1, Tx: true, Run: workItemReapStaleParks},
		opWorkItemList:                   {Name: "work_item_list", Cells: len(workItemCols), Args: 1, Run: workItemList},
		opWorkItemListLRU:                {Name: "work_item_list_lru", Cells: len(workItemCols), Args: 1, Run: workItemListLRU},
		opLifecycleEventAdd:              {Name: "lifecycle_event_add", Args: 7, Tx: true, Run: lifecycleEventAdd},
		opLifecycleEventList:             {Name: "lifecycle_event_list", Cells: len(lifecycleEventCols), Args: 2, Run: lifecycleEventList},
		opStageAttemptInc:                {Name: "stage_attempt_inc", Args: 2, Tx: true, Run: stageAttemptInc},
		opStageAttemptReset:              {Name: "stage_attempt_reset", Args: 2, Tx: true, Run: stageAttemptReset},
		opStageAttemptGet:                {Name: "stage_attempt_get", Args: 2, Run: stageAttemptGet},
		opWorkItemRecordOutcome:          {Name: "work_item_record_outcome", Args: 14, Tx: true, Run: workItemRecordOutcome},

		opWFEChildrenList:              {Name: "wfe_children_list", Cells: 1, Args: 2, Run: wfeChildrenList},
		opWFEActiveRootCount:           {Name: "wfe_active_root_count", Args: 0, Run: wfeActiveRootCount},
		opWFEWorkItemIDByGitProposal:   {Name: "wfe_work_item_id_by_git_proposal", Args: 3, Run: wfeIDByGitProposal},
		opWFEExecutedTurnCount:         {Name: "wfe_executed_turn_count", Args: 1, Run: wfeExecutedTurnCount},
		opWFEStageLoopCount:            {Name: "wfe_stage_loop_count", Args: 2, Run: wfeStageLoopCount},
		opWFERunnerFailuresSince:       {Name: "wfe_runner_failures_since_progress", Args: 2, Run: wfeRunnerFailuresSince},
		opWFECapacityWaitsSince:        {Name: "wfe_capacity_waits_since_progress", Args: 2, Run: wfeCapacityWaitsSince},
		opWFEDescendantIDs:             {Name: "wfe_descendant_ids", Cells: 1, Args: 2, Run: wfeDescendantIDs},
		opWFEResumeTransient:           {Name: "wfe_resume_transient", Args: 2, Tx: true, Run: wfeResumeTransient},
		opWFEResumeWallCaps:            {Name: "wfe_resume_wall_caps", Args: 1, Tx: true, Run: wfeResumeWallCaps},
		opWFEAbandonExhaustedWallCaps:  {Name: "wfe_abandon_exhausted_wall_caps", Args: 2, Tx: true, Run: wfeAbandonExhaustedWallCaps},
		opWFEResumeReadyParents:        {Name: "wfe_resume_ready_parents", Args: 0, Tx: true, Run: wfeResumeReadyParents},
		opWFEDelegateJobSave:           {Name: "wfe_delegate_job_save", Args: 4, Tx: true, Run: wfeDelegateJobSave},
		opWFEDelegateJobsTerminalClaim: {Name: "wfe_delegate_jobs_terminal_claim", Cells: 2, Args: 1, Tx: true, Run: wfeDelegateJobsTerminalClaim},

		opWFEBudgetReserve:   {Name: "wfe_budget_reserve", Cells: 6, Args: 2, RunDB: wfeBudgetReserve},
		opWFEBudgetTotals:    {Name: "wfe_budget_totals", Cells: 3, Args: 1, Run: wfeBudgetTotals},
		opWFEBudgetRelease:   {Name: "wfe_budget_release", Args: 2, Tx: true, Run: wfeBudgetRelease},
		opWFEBudgetHeartbeat: {Name: "wfe_budget_heartbeat", Args: 2, Tx: true, Run: wfeBudgetHeartbeat},
		opWFEBudgetReconcile: {Name: "wfe_budget_reconcile", Args: 3, RunDB: wfeBudgetReconcile},

		opWFEMove:                   {Name: "wfe_move", Args: 7, RunDB: wfeMove},
		opWFERecordRetry:            {Name: "wfe_record_retry", Args: 6, RunDB: wfeRecordRetry},
		opWFEParkWithDetail:         {Name: "wfe_park_with_detail", Args: 5, RunDB: wfeParkWithDetail},
		opWFEResume:                 {Name: "wfe_resume", Args: 1, RunDB: wfeResume},
		opWFEFinish:                 {Name: "wfe_finish", Args: 6, RunDB: wfeFinish},
		opWFEStopTree:               {Name: "wfe_stop_tree", Cells: 1, Args: 2, RunDB: wfeStopTree},
		opWFEReconcileOrphans:       {Name: "wfe_reconcile_orphans", Cells: 1, Args: 1, RunDB: wfeReconcileOrphans},
		opWFEParkBudgetTree:         {Name: "wfe_park_budget_tree", Args: 3, RunDB: wfeParkBudgetTree},
		opWFEDeleteTree:             {Name: "wfe_delete_tree", Args: 1, Tx: true, Run: wfeDeleteTree},
		opWFEResolveGate:            {Name: "wfe_resolve_gate", Args: 5, RunDB: wfeResolveGate},
		opWFERejectGate:             {Name: "wfe_reject_gate", Args: 3, RunDB: wfeRejectGate},
		opWFEParkRunnerFailure:      {Name: "wfe_park_runner_failure", Args: 8, RunDB: wfeParkRunnerFailure},
		opWFERecoverLostReplay:      {Name: "wfe_recover_lost_replay", Args: 3, RunDB: wfeRecoverLostReplay},
		opWFERecordRequestedChanges: {Name: "wfe_record_requested_changes", Cells: 4, Args: 9, RunDB: wfeRecordRequestedChanges},
		opWFEClaimFrozenCreates:     {Name: "wfe_claim_frozen_creates", Cells: 3, Args: frozenFields, RunDB: wfeClaimFrozenCreates},
		opWFECreateWorkItem:         {Name: "wfe_create_work_item", Args: 12, RunDB: wfeCreateWorkItem},
		opWFELatestStageRetryDetail: {Name: "wfe_latest_stage_retry_detail", Args: 2, Run: wfeLatestStageRetryDetail},
	},
}
