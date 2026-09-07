package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The engine's view of the work-item tree: what it can see, what it may resume,
// and what it may stop.
const (
	opWFEChildrenList              uint32 = 33
	opWFEActiveRootCount           uint32 = 34
	opWFEWorkItemIDByGitProposal   uint32 = 35
	opWFEExecutedTurnCount         uint32 = 36
	opWFEStageLoopCount            uint32 = 37
	opWFERunnerFailuresSince       uint32 = 38
	opWFECapacityWaitsSince        uint32 = 39
	opWFEDescendantIDs             uint32 = 40
	opWFEResumeTransient           uint32 = 41
	opWFEResumeWallCaps            uint32 = 42
	opWFEAbandonExhaustedWallCaps  uint32 = 43
	opWFEResumeReadyParents        uint32 = 44
	opWFEDelegateJobSave           uint32 = 45
	opWFEDelegateJobsTerminalClaim uint32 = 46
	opWFEStopTree                  uint32 = 57
	opWFEReconcileOrphans          uint32 = 58
	opWFEParkBudgetTree            uint32 = 59
	opWFEDeleteTree                uint32 = 60
	opWFELatestStageRetryDetail    uint32 = 68
)

// treeCTE is the subtree of a run, the run included.
//
// UNION, not UNION ALL, and no depth column. The C used UNION ALL with no
// termination at all: parent_id had no cycle prevention on write, so one bad
// parent made every tree operation spin forever.
//
// UNION is what stops it. A recursive CTE ends when an iteration yields no NEW
// rows, and on a cycle these ids repeat exactly. Carrying a depth to bound it
// instead would make every row distinct, defeat the dedup, and turn a three-row
// answer into sixty-five identical ones -- terminating, but wrong.
const treeCTE = `WITH RECURSIVE tree(id) AS (
        SELECT $1::text
        UNION
        SELECT child.work_item_id
          FROM lifecycle_work_item child
          JOIN tree parent ON child.parent_id = parent.id
    ) `

// orphanCTE finds active runs whose parent has already reached a terminal state,
// and their active descendants. Seeded from the children rather than a root:
// this is a sweep looking for orphans anywhere, not a walk from somewhere known.
const orphanCTE = `WITH RECURSIVE orphan(id) AS (
        SELECT child.work_item_id
          FROM lifecycle_work_item child
          JOIN lifecycle_work_item parent ON parent.work_item_id = child.parent_id
         WHERE child.state = 'active'
           AND parent.state IN ('accepted', 'rejected', 'stopped', 'abandoned')
        UNION
        SELECT child.work_item_id
          FROM lifecycle_work_item child
          JOIN orphan parent ON child.parent_id = parent.id
         WHERE child.state = 'active'
    ) `

const (
	wfeChildrenListSQL = `SELECT work_item_id FROM lifecycle_work_item
	                       WHERE parent_id = $1 ORDER BY id LIMIT $2`

	wfeActiveRootCountSQL = `SELECT count(*) FROM lifecycle_work_item
	                          WHERE parent_id = '' AND state = 'active'`

	// Roots only: a slice child carries its parent's proposal and would answer
	// for a lookup that means the run as a whole.
	wfeIDByGitProposalSQL = `SELECT work_item_id FROM lifecycle_work_item
	                          WHERE repo = $1 AND parent_id = ''
	                            AND (proposal_path = $2
	                                 OR (left(proposal_path, 4) = 'git:'
	                                     AND right(proposal_path, length($3)) = $3))
	                          ORDER BY id LIMIT 1`

	wfeExecutedTurnCountSQL = `SELECT count(*) FROM lifecycle_event
	                            WHERE work_item_id = $1 AND kind IN ('advance', 'loop')`

	wfeStageLoopCountSQL = `SELECT count(*) FROM lifecycle_event
	                         WHERE work_item_id = $1 AND stage = $2
	                           AND kind IN ('loop', 'advance')`

	// The two "since progress" counts differ only in which side of the capacity
	// predicate they take, so the statement is written once with the predicate
	// as a parameter. Splitting them would leave two nearly-identical
	// statements to keep in step, and they are the kind that drift.
	wfeSinceProgressSQL = `SELECT count(*) FROM lifecycle_event
	                        WHERE work_item_id = $1 AND stage = $2 AND kind = 'pause'
	                          AND (detail LIKE 'capacity_backpressure:%') = $3
	                          AND id > coalesce((SELECT max(id) FROM lifecycle_event
	                                              WHERE work_item_id = $1
	                                                AND kind IN ('advance', 'loop', 'create')), 0)`

	wfeDescendantIDsSQL = treeCTE + `SELECT id FROM tree ORDER BY id LIMIT $2`

	// The window is a bound interval; the C spliced the seconds into the text.
	wfeResumeTransientSQL = `UPDATE lifecycle_work_item
	                            SET pause_reason = '', paused_state = '', updated_at = now()
	                          WHERE state = 'active' AND pause_reason = $1
	                            AND updated_at <= now() - make_interval(secs => $2)`

	// override_count is bumped as it resumes: the run gets max_resumes chances
	// and the counter is what spends them.
	wfeResumeWallCapsSQL = `UPDATE lifecycle_work_item
	                           SET pause_reason = '', paused_state = '',
	                               override_count = override_count + 1, updated_at = now()
	                         WHERE state = 'active' AND pause_reason = 'wall_cap'
	                           AND override_count < $1`

	// The grace period is measured from the last time the row moved, so a run
	// resumed a moment ago is not abandoned by the same sweep that resumed it.
	wfeAbandonExhaustedSQL = `UPDATE lifecycle_work_item
	                             SET state = 'abandoned', pause_reason = '', paused_state = '',
	                                 updated_at = now()
	                           WHERE state = 'active' AND pause_reason = 'wall_cap'
	                             AND override_count >= $1
	                             AND updated_at < now() - make_interval(secs => $2)`

	// Both halves matter: EXISTS proves the parent actually fanned out, and NOT
	// EXISTS(active) proves every slice has finished. Dropping the first would
	// resume parents whose children were never created.
	wfeResumeReadyParentsSQL = `UPDATE lifecycle_work_item AS parent
	                               SET pause_reason = '', paused_state = '', updated_at = now()
	                             WHERE parent.state = 'active'
	                               AND parent.pause_reason = 'slices_running'
	                               AND EXISTS (SELECT 1 FROM lifecycle_work_item child
	                                            WHERE child.parent_id = parent.work_item_id)
	                               AND NOT EXISTS (SELECT 1 FROM lifecycle_work_item child
	                                                WHERE child.parent_id = parent.work_item_id
	                                                  AND child.state = 'active')`

	// NULLIF maps the wire's "no job yet" sentinel onto the null the column
	// stores, which is what lets job_id carry a reference to agent_jobs.
	wfeDelegateJobSaveSQL = `INSERT INTO lifecycle_delegate_job
	                             (execution_key, job_id, work_item_id, participant_token)
	                         VALUES ($1, NULLIF($2::bigint, 0), $3, $4)
	                         ON CONFLICT (execution_key) DO UPDATE SET
	                             job_id = EXCLUDED.job_id,
	                             work_item_id = EXCLUDED.work_item_id,
	                             participant_token = EXCLUDED.participant_token,
	                             updated_at = now()`

	// Read and claim in one statement. The caller cancels what this returns; if
	// the read committed and the claim did not, the same jobs would come back
	// on every sweep forever and the cancel_attempts ordering that spreads
	// retries would never advance.
	//
	// The C did this as a SELECT, then one UPDATE per row, inside a
	// transaction. A data-modifying CTE is the same thing atomically and in one
	// round trip.
	wfeTerminalClaimSQL = `WITH claimable AS (
	        SELECT mapping.execution_key, mapping.job_id
	          FROM lifecycle_delegate_job mapping
	          JOIN lifecycle_work_item item ON item.work_item_id = mapping.work_item_id
	          JOIN agent_jobs job ON job.id = mapping.job_id
	         WHERE item.state IN ('accepted', 'rejected', 'stopped', 'abandoned')
	           AND job.status IN ('pending', 'running')
	         ORDER BY mapping.cancel_attempts, mapping.job_id
	         LIMIT $1
	    ), claimed AS (
	        UPDATE lifecycle_delegate_job m
	           SET cancel_attempts = m.cancel_attempts + 1, updated_at = now()
	          FROM claimable c
	         WHERE m.execution_key = c.execution_key AND m.job_id = c.job_id
     RETURNING m.execution_key, m.job_id, m.cancel_attempts
    )
    SELECT execution_key, job_id FROM claimed ORDER BY cancel_attempts, job_id`

	// One terminal event per run being ended, carrying the stage and hash it
	// was at when it ended. Written from the row itself so those values are the
	// run's own rather than the caller's idea of them.
	terminalEventFromSetSQL = `INSERT INTO lifecycle_event
	        (work_item_id, stage, kind, actor, detail, content_hash)
	    SELECT item.work_item_id, item.current_stage, 'terminal', 'go-wfe', $2, item.content_hash
	      FROM lifecycle_work_item item
	     WHERE item.work_item_id IN (SELECT id FROM %s) AND item.state = 'active'`

	wfeParkBudgetChargeSQL = `UPDATE lifecycle_work_item
	                             SET cum_cost_usd = cum_cost_usd + $2, reserved_cost_usd = 0,
	                                 reservation_state = '', reservation_owner = '',
	                                 reservation_lease_until = NULL
	                           WHERE work_item_id = $1`

	// Two exclusions matter:
	//
	//   already parked   keeps the reason it has, which is the more specific
	//                    explanation of why that run stopped.
	//   holding a live   is mid-invocation. Parking it would strand money it has
	//   reservation      authorised but not reconciled, and it would come back
	//                    holding a reservation it can no longer spend. The
	//                    completed item is the exception: its invocation is over,
	//                    which is why we are here.
	wfeParkBudgetTreeSQL = treeCTE + `UPDATE lifecycle_work_item
	                                     SET pause_reason = 'budget_cap',
	                                         paused_state = current_stage, updated_at = now()
	                                   WHERE state = 'active' AND pause_reason = ''
	                                     AND work_item_id IN (SELECT id FROM tree)
	                                     AND (reservation_state = '' OR work_item_id = $2)`
)

// countSince answers one of the two "since progress" counts.
func countSince(ctx context.Context, q store.Queryer, workItemID, stage string,
	capacity bool) (uint32, []string, error) {
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, wfeSinceProgressSQL, workItemID, stage, capacity)
}

func wfeChildrenList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, wfeChildrenListSQL, f[0], max)
}

func wfeActiveRootCount(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	return scalar(ctx, q, wfeActiveRootCountSQL)
}

func wfeIDByGitProposal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	return lookupID(ctx, q, wfeIDByGitProposalSQL, f[0], f[1], f[2])
}

func wfeExecutedTurnCount(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, wfeExecutedTurnCountSQL, f[0])
}

func wfeStageLoopCount(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, wfeStageLoopCountSQL, f[0], f[1])
}

func wfeRunnerFailuresSince(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return countSince(ctx, q, f[0], f[1], false)
}

func wfeCapacityWaitsSince(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return countSince(ctx, q, f[0], f[1], true)
}

func wfeDescendantIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if f[0] == "" || !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, wfeDescendantIDsSQL, f[0], max)
}

// collectStrings runs a query returning one text column and flattens it.
func collectStrings(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	rows, err := q.Query(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	var cells []string
	for rows.Next() {
		var value string
		if err := rows.Scan(&value); err != nil {
			return 0, nil, err
		}
		cells = append(cells, value)
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// affected runs a sweep and answers with how many rows it moved.
func affected(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	tag, err := q.Exec(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func wfeResumeTransient(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	secs, ok := store.Atoi(f[1])
	if f[0] == "" || !ok || secs < 0 {
		return store.StatusInvalid, nil, nil
	}
	return affected(ctx, q, wfeResumeTransientSQL, f[0], secs)
}

func wfeResumeWallCaps(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	maxResumes, ok := store.Atoi(f[0])
	if !ok || maxResumes < 0 {
		return store.StatusInvalid, nil, nil
	}
	return affected(ctx, q, wfeResumeWallCapsSQL, maxResumes)
}

func wfeAbandonExhaustedWallCaps(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	maxResumes, okMax := store.Atoi(f[0])
	grace, okGrace := store.Atoi(f[1])
	if !okMax || maxResumes < 0 || !okGrace || grace < 0 {
		return store.StatusInvalid, nil, nil
	}
	return affected(ctx, q, wfeAbandonExhaustedSQL, maxResumes, grace)
}

func wfeResumeReadyParents(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	return affected(ctx, q, wfeResumeReadyParentsSQL)
}

func wfeDelegateJobSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[2])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return setField(ctx, q, wfeDelegateJobSaveSQL, f[0], jobID, f[1], f[3])
}

// wfeDelegateJobsTerminalClaim is op 46: the delegate jobs still running under
// a work item that has already finished, claimed as this sweep's to cancel.
func wfeDelegateJobsTerminalClaim(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, wfeTerminalClaimSQL, max)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	var cells []string
	for rows.Next() {
		var key string
		var jobID int64
		if err := rows.Scan(&key, &jobID); err != nil {
			return 0, nil, err
		}
		cells = append(cells, key, store.I64toa(jobID))
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// endActiveSet writes a terminal event for every active member of a set and
// stops them, answering with the ids it ended.
//
// The event is written BEFORE the state changes, while the stage and hash are
// still the run's own.
func endActiveSet(ctx context.Context, tx store.Tx, cte, cteName, detail string,
	args ...any) ([]string, error) {
	rows, err := tx.Query(ctx, cte+`SELECT item.work_item_id FROM lifecycle_work_item item
	                                 WHERE item.work_item_id IN (SELECT id FROM `+cteName+`)
	                                   AND item.state = 'active'
	                                 ORDER BY item.work_item_id`, args...)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(ids) == 0 {
		return nil, nil
	}

	if _, err := tx.Exec(ctx, `INSERT INTO lifecycle_event
	        (work_item_id, stage, kind, actor, detail, content_hash)
	    SELECT work_item_id, current_stage, 'terminal', 'go-wfe', $2, content_hash
	      FROM lifecycle_work_item
	     WHERE work_item_id = ANY($1) AND state = 'active'`, ids, detail); err != nil {
		return nil, err
	}
	if _, err := tx.Exec(ctx, `UPDATE lifecycle_work_item
	                              SET state = 'stopped', pause_reason = '', paused_state = '',
	                                  updated_at = now()
	                            WHERE work_item_id = ANY($1) AND state = 'active'`, ids); err != nil {
		return nil, err
	}
	if _, err := tx.Exec(ctx,
		`DELETE FROM wfe_frozen_create WHERE work_item_id = ANY($1)`, ids); err != nil {
		return nil, err
	}
	return ids, nil
}

// wfeStopTree is op 57: stop a run and everything under it.
func wfeStopTree(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if f[0] == "" || !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	ids, err := endActiveSet(ctx, tx, treeCTE, "tree", "operator_stop", f[0])
	if err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	if len(ids) > max {
		ids = ids[:max]
	}
	return store.StatusOK, ids, nil
}

// wfeReconcileOrphans is op 58: stop active runs whose parent already finished.
func wfeReconcileOrphans(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > lifecycleListMax {
		return store.StatusInvalid, nil, nil
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	ids, err := endActiveSet(ctx, tx, orphanCTE, "orphan", "ancestor_terminal")
	if err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	if len(ids) > max {
		ids = ids[:max]
	}
	return store.StatusOK, ids, nil
}

// wfeParkBudgetTree is op 59: the tree ran out of money.
func wfeParkBudgetTree(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	rootID, completedID := f[0], f[1]
	addedCost, okCost := store.Atof(f[2])
	if rootID == "" || !okCost {
		return store.StatusInvalid, nil, nil
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	// Charging is skipped when there is nothing to charge, so a park with no
	// completed invocation does not clear a reservation it never consumed.
	if addedCost > 0 && completedID != "" {
		if _, err := tx.Exec(ctx, wfeParkBudgetChargeSQL, completedID, addedCost); err != nil {
			return 0, nil, err
		}
	}
	if _, err := tx.Exec(ctx, wfeParkBudgetTreeSQL, rootID, completedID); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeDeleteTree is op 60: remove a run and everything under it, from every table.
func wfeDeleteTree(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	// Children before parents, so a partial failure never leaves history
	// without its item or an item without its history.
	for _, table := range []string{
		"wfe_frozen_create", "wfe_convergence", "lifecycle_stage_attempt", "lifecycle_event",
	} {
		if _, err := q.Exec(ctx,
			treeCTE+`DELETE FROM `+table+` WHERE work_item_id IN (SELECT id FROM tree)`,
			f[0]); err != nil {
			return 0, nil, err
		}
	}
	if _, err := q.Exec(ctx,
		treeCTE+`DELETE FROM lifecycle_work_item WHERE work_item_id IN (SELECT id FROM tree)`,
		f[0]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeLatestStageRetryDetail is op 68: why a stage last had to retry, phrased for
// a human reading the run.
//
// The answer is the most recent explanation SINCE the stage last made progress,
// and progress has two boundaries rather than one: the last advance out of the
// stage, and any operator resume after it. Three sources, in order of how
// directly they explain the CURRENT attempt:
//
//  1. the newest loop after the boundary -- the most direct explanation;
//  2. failing that, and only when a resume moved the boundary, the pause that
//     preceded that resume, which is what the operator was looking at;
//  3. failing that, the newest non-manual pause after the boundary.
//
// "manual" is excluded throughout: an operator pausing a run is not the run
// explaining itself. Empty is a legitimate answer -- a stage with no attempts
// and no resume has nothing to explain.
func wfeLatestStageRetryDetail(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	workItemID, stage := f[0], f[1]
	if workItemID == "" {
		return store.StatusInvalid, nil, nil
	}

	var attempts int64
	switch err := q.QueryRow(ctx, stageAttemptGetSQL, workItemID, stage).Scan(&attempts); {
	case store.IsNoRows(err):
		attempts = 0
	case err != nil:
		return 0, nil, err
	}

	var boundary int64
	if err := q.QueryRow(ctx, `SELECT coalesce(max(id), 0) FROM lifecycle_event
	                            WHERE work_item_id = $1 AND stage = $2 AND kind = 'advance'`,
		workItemID, stage).Scan(&boundary); err != nil {
		return 0, nil, err
	}

	var reset int64
	if err := q.QueryRow(ctx, `SELECT coalesce(max(id), 0) FROM lifecycle_event
	                            WHERE work_item_id = $1 AND stage = $2 AND kind = 'resume'
	                              AND detail = 'retry_limit' AND id > $3`,
		workItemID, stage, boundary).Scan(&reset); err != nil {
		return 0, nil, err
	}

	if attempts == 0 && reset <= boundary {
		return store.StatusOK, []string{""}, nil
	}

	after := boundary
	if reset > boundary {
		after = reset
	}

	var detail string
	err := q.QueryRow(ctx, `SELECT detail FROM lifecycle_event
	                         WHERE work_item_id = $1 AND stage = $2 AND kind = 'loop' AND id > $3
	                         ORDER BY id DESC LIMIT 1`, workItemID, stage, after).Scan(&detail)
	switch {
	case err == nil:
		return store.StatusOK, []string{detail}, nil
	case !store.IsNoRows(err):
		return 0, nil, err
	}

	if reset > boundary {
		err = q.QueryRow(ctx, `SELECT detail FROM lifecycle_event
		                        WHERE work_item_id = $1 AND stage = $2 AND kind = 'pause'
		                          AND detail <> 'manual' AND id > $3 AND id < $4
		                        ORDER BY id DESC LIMIT 1`,
			workItemID, stage, boundary, reset).Scan(&detail)
		switch {
		case err == nil:
			return store.StatusOK, []string{detail}, nil
		case !store.IsNoRows(err):
			return 0, nil, err
		}
	}

	err = q.QueryRow(ctx, `SELECT detail FROM lifecycle_event
	                        WHERE work_item_id = $1 AND stage = $2 AND kind = 'pause'
	                          AND detail <> 'manual' AND id > $3
	                        ORDER BY id DESC LIMIT 1`, workItemID, stage, after).Scan(&detail)
	switch {
	case store.IsNoRows(err):
		return store.StatusOK, []string{""}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{detail}, nil
}
