package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Roadmap dispatches and the workflow-engine session binding.

const roadmapDispatchColumns = `id, roadmap_id, status, phase, token_profile,
                                require_slice_discussion, budget_ceiling_tokens, exit_reason,
                                to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                                to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const roadmapUnitColumns = `id, roadmap_id, unit_id, level, state, tool_policy_mode,
                            claimed_by,
                            COALESCE(to_char(claimed_at AT TIME ZONE 'utc',
                                     'YYYY-MM-DD HH24:MI:SS'), ''),
                            COALESCE(to_char(heartbeat_at AT TIME ZONE 'utc',
                                     'YYYY-MM-DD HH24:MI:SS'), ''),
                            verify_attempts, dispatch_attempts, worktree_path,
                            coord_job_id, result, error,
                            to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                            to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	// Starting a dispatch restarts it: status and phase go back to the
	// beginning, which is what "dispatch this roadmap" means.
	//
	// created_at does NOT go back. The C used INSERT OR REPLACE, which deletes
	// the row and inserts a new one, so re-dispatching a roadmap silently reset
	// when it had first been started -- and anything reasoning about how long a
	// roadmap had been running read the time of the last restart.
	roadmapDispatchUpsertSQL = `INSERT INTO roadmap_dispatch
	                                (roadmap_id, status, phase, token_profile,
	                                 require_slice_discussion, budget_ceiling_tokens,
	                                 exit_reason)
	                            VALUES ($1, 'running', 'plan', $2, $3, $4, '')
	                            ON CONFLICT (roadmap_id) DO UPDATE SET
	                                status                   = 'running',
	                                phase                    = 'plan',
	                                token_profile            = EXCLUDED.token_profile,
	                                require_slice_discussion = EXCLUDED.require_slice_discussion,
	                                budget_ceiling_tokens    = EXCLUDED.budget_ceiling_tokens,
	                                exit_reason              = '',
	                                updated_at               = now()`

	roadmapDispatchGetSQL = `SELECT ` + roadmapDispatchColumns + `
	                           FROM roadmap_dispatch WHERE roadmap_id = $1`

	roadmapDispatchSetStatusSQL = `UPDATE roadmap_dispatch
	                                  SET status = $2, exit_reason = $3, updated_at = now()
	                                WHERE roadmap_id = $1`

	roadmapDispatchSetPhaseSQL = `UPDATE roadmap_dispatch
	                                 SET phase = $2, updated_at = now()
	                               WHERE roadmap_id = $1`

	// Ensuring a unit leaves an existing one alone: a unit already being worked
	// on must not be reset to pending by a coordinator re-walking the roadmap.
	roadmapUnitEnsureSQL = `INSERT INTO roadmap_unit_dispatch
	                            (roadmap_id, unit_id, level, state, tool_policy_mode)
	                        VALUES ($1, $2, $3, 'pending', $4)
	                        ON CONFLICT (roadmap_id, unit_id) DO NOTHING`

	roadmapUnitGetSQL = `SELECT ` + roadmapUnitColumns + `
	                       FROM roadmap_unit_dispatch
	                      WHERE roadmap_id = $1 AND unit_id = $2`

	roadmapUnitSetStateSQL = `UPDATE roadmap_unit_dispatch
	                             SET state = $3, updated_at = now()
	                           WHERE roadmap_id = $1 AND unit_id = $2`

	// The claim is what decides between two workers.
	//
	// The C had NO guard here: it set claimed_by and state='active' on whatever
	// row matched, so two coordinators that had both selected the same unit
	// both "succeeded" and the second silently took ownership from the first.
	// Both then worked the same unit.
	//
	// claimed_by = '' is the same condition select_next uses to decide a unit is
	// available, so putting it in the WHERE makes the claim itself the decision
	// rather than a write that trusts a read taken earlier. Exactly one claimant
	// changes a row; the loser is told so.
	roadmapUnitClaimSQL = `UPDATE roadmap_unit_dispatch
	                          SET claimed_by = $3,
	                              claimed_at = now(),
	                              heartbeat_at = now(),
	                              state = 'active',
	                              dispatch_attempts = dispatch_attempts + 1,
	                              worktree_path = $4,
	                              updated_at = now()
	                        WHERE roadmap_id = $1 AND unit_id = $2
	                          AND claimed_by = ''`

	roadmapUnitHeartbeatSQL = `UPDATE roadmap_unit_dispatch
	                              SET heartbeat_at = now(), updated_at = now()
	                            WHERE roadmap_id = $1 AND unit_id = $2`

	// Finishing releases the unit as well as recording the outcome: a finished
	// unit that still names an owner reads as claimed forever, and the schema's
	// claim constraint says a unit with no claim time has no owner.
	roadmapUnitFinishSQL = `UPDATE roadmap_unit_dispatch
	                           SET state = $3, result = $4, error = $5,
	                               claimed_by = '', claimed_at = NULL,
	                               updated_at = now()
	                         WHERE roadmap_id = $1 AND unit_id = $2`

	roadmapUnitSetCoordJobSQL = `UPDATE roadmap_unit_dispatch
	                                SET coord_job_id = $3, updated_at = now()
	                              WHERE roadmap_id = $1 AND unit_id = $2`

	roadmapUnitIncrementVerifySQL = `UPDATE roadmap_unit_dispatch
	                                    SET verify_attempts = verify_attempts + 1,
	                                        updated_at = now()
	                                  WHERE roadmap_id = $1 AND unit_id = $2`

	// The next unit worth handing out: the lowest-numbered task that is pending
	// and unclaimed. This is a HINT -- the claim is what actually decides.
	roadmapUnitSelectNextSQL = `SELECT unit_id FROM roadmap_unit_dispatch
	                             WHERE roadmap_id = $1
	                               AND level = 'task'
	                               AND state = 'pending'
	                               AND claimed_by = ''
	                             ORDER BY id
	                             LIMIT 1`
)

func roadmapDispatchRow(scan func(...any) error) ([]string, error) {
	var (
		id, ceiling                       int64
		roadmapID, status, phase, profile string
		exitReason, createdAt, updatedAt  string
		requireDiscussion                 bool
	)
	if err := scan(&id, &roadmapID, &status, &phase, &profile,
		&requireDiscussion, &ceiling, &exitReason, &createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), roadmapID, status, phase, profile,
		store.Btoa(requireDiscussion), store.I64toa(ceiling), exitReason,
		createdAt, updatedAt,
	}, nil
}

func roadmapUnitRow(scan func(...any) error) ([]string, error) {
	var (
		id, verify, dispatch, coordJob          int64
		roadmapID, unitID, level, state, policy string
		claimedBy, claimedAt, heartbeatAt       string
		worktree, result, errText               string
		createdAt, updatedAt                    string
	)
	if err := scan(&id, &roadmapID, &unitID, &level, &state, &policy,
		&claimedBy, &claimedAt, &heartbeatAt, &verify, &dispatch,
		&worktree, &coordJob, &result, &errText, &createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), roadmapID, unitID, level, state, policy,
		claimedBy, claimedAt, heartbeatAt,
		store.I64toa(verify), store.I64toa(dispatch),
		worktree, store.I64toa(coordJob), result, errText,
		createdAt, updatedAt,
	}, nil
}

func roadmapDispatchUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	profile := f[1]
	if profile == "" {
		profile = "balanced"
	}
	requireDiscussion, ok := store.Atob(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	ceiling, ok := store.Atoi64(f[3])
	if !ok || ceiling < 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, roadmapDispatchUpsertSQL,
		f[0], profile, requireDiscussion, ceiling); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func roadmapDispatchGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := roadmapDispatchRow(func(dest ...any) error {
		return q.QueryRow(ctx, roadmapDispatchGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func roadmapDispatchSetStatus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapDispatchSetStatusSQL, f[0], f[1], f[2])
}

func roadmapDispatchSetPhase(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapDispatchSetPhaseSQL, f[0], f[1])
}

// touchedOrMissing reports MISSING when an update matched nothing, which is
// what "the thing you named is not there" means for a write.
func touchedOrMissing(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	tag, err := q.Exec(ctx, sql, args...)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

func roadmapUnitEnsure(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	level := f[2]
	if level == "" {
		level = "task"
	}
	policy := f[3]
	if policy == "" {
		policy = "execution"
	}
	// DO NOTHING makes a repeat a no-op, so this succeeds whether or not it
	// wrote anything: the caller asked for the unit to exist and it does.
	if _, err := q.Exec(ctx, roadmapUnitEnsureSQL, f[0], f[1], level, policy); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func roadmapUnitGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := roadmapUnitRow(func(dest ...any) error {
		return q.QueryRow(ctx, roadmapUnitGetSQL, f[0], f[1]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func roadmapUnitSetState(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapUnitSetStateSQL, f[0], f[1], f[2])
}

// roadmapUnitClaim takes a unit for one worker.
//
// MISSING means the claim did not land: either the unit does not exist or
// somebody else holds it. Both mean "do not start work on this", which is the
// only thing the caller needs to know and the thing the C never told it.
func roadmapUnitClaim(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapUnitClaimSQL, f[0], f[1], f[2], f[3])
}

func roadmapUnitHeartbeat(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapUnitHeartbeatSQL, f[0], f[1])
}

func roadmapUnitFinish(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	state := f[2]
	if state == "" {
		state = "done"
	}
	return touchedOrMissing(ctx, q, roadmapUnitFinishSQL, f[0], f[1], state, f[3], f[4])
}

func roadmapUnitSetCoordJob(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[2])
	if f[0] == "" || f[1] == "" || !ok || jobID < 0 {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapUnitSetCoordJobSQL, f[0], f[1], jobID)
}

func roadmapUnitIncrementVerifyAttempts(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, roadmapUnitIncrementVerifySQL, f[0], f[1])
}

func roadmapUnitSelectNext(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var unitID string
	err := q.QueryRow(ctx, roadmapUnitSelectNextSQL, f[0]).Scan(&unitID)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{unitID, "0"}, nil
}

// --- the workflow-engine session binding ------------------------------------

const (
	wfeBindSQL = `INSERT INTO workflow_binding (aimee_session_id, work_item_id, enforce_stage)
	              VALUES ($1, $2, $3)
	              ON CONFLICT (aimee_session_id) DO UPDATE SET
	                  work_item_id  = EXCLUDED.work_item_id,
	                  enforce_stage = EXCLUDED.enforce_stage,
	                  updated_at    = now()
	              RETURNING (xmax = 0)`

	wfeBindingGetSQL = `SELECT work_item_id, enforce_stage
	                      FROM workflow_binding WHERE aimee_session_id = $1`

	wfeUnbindSQL = `DELETE FROM workflow_binding WHERE aimee_session_id = $1`

	// A ttl of 0 clears the lease. A negative ttl sets one in the past, which
	// is how a caller forces a binding stale on purpose.
	//
	// The C built the interval by formatting "%+d seconds" into a string and
	// binding THAT to datetime(). The number was never anything but a number.
	wfeLeaseRenewSQL = `UPDATE workflow_binding
	                       SET lease_expiry = CASE WHEN $2::bigint = 0 THEN NULL
	                                               ELSE now() + make_interval(secs => $2::bigint)
	                                          END,
	                           updated_at = now()
	                     WHERE aimee_session_id = $1`

	wfeLeaseExpiryGetSQL = `SELECT COALESCE(to_char(lease_expiry AT TIME ZONE 'utc',
	                                        'YYYY-MM-DD HH24:MI:SS'), '')
	                          FROM workflow_binding WHERE aimee_session_id = $1`

	// A lease that has run out. NULL is no lease at all, which is not stale --
	// the C spelled "no lease" as an empty string and had to exclude it by
	// hand, because '' sorts before every timestamp and would otherwise have
	// read as expired.
	wfeLeaseStaleSQL = `SELECT work_item_id FROM workflow_binding
	                     WHERE lease_expiry IS NOT NULL AND lease_expiry < now()
	                     ORDER BY lease_expiry ASC
	                     LIMIT $1`

	// Reclaiming clears the expired leases and says how many it cleared.
	//
	// The C read up to 64 stale rows into a C array and then updated them one
	// at a time, so a binding that expired between the read and the write was
	// missed, and one reclaimed by another process in the meantime was counted
	// anyway. One statement reclaims exactly what it changed.
	wfeLeaseReclaimSQL = `UPDATE workflow_binding
	                         SET lease_expiry = NULL, updated_at = now()
	                       WHERE lease_expiry IS NOT NULL AND lease_expiry < now()`
)

// wfeBind binds a session to a work item, and says whether it was new.
func wfeBind(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	stage := f[2]
	if stage == "" {
		stage = "off"
	}
	var inserted bool
	if err := q.QueryRow(ctx, wfeBindSQL, f[0], f[1], stage).Scan(&inserted); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(inserted)}, nil
}

func wfeBindingGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var workItemID, stage string
	err := q.QueryRow(ctx, wfeBindingGetSQL, f[0]).Scan(&workItemID, &stage)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{workItemID, stage}, nil
}

func wfeUnbind(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, wfeUnbindSQL, f[0])
}

func wfeLeaseRenew(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	ttl, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, wfeLeaseRenewSQL, f[0], ttl)
}

func wfeLeaseExpiryGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var expiry string
	err := q.QueryRow(ctx, wfeLeaseExpiryGetSQL, f[0]).Scan(&expiry)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{expiry}, nil
}

func wfeLeaseStaleWorkItems(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, wfeLeaseStaleSQL, max)
}

func wfeLeaseReclaimStale(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	tag, err := q.Exec(ctx, wfeLeaseReclaimSQL)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}
