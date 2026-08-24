package families

import (
	"context"
	"encoding/json"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Execution plans: a task broken into ordered steps, and the evidence each step
// leaves behind.

// A plan travels as four scalars, a fixed 32 steps, and a step count. Each step
// is nine scalars plus eight dependency slots.
const (
	planMaxSteps      = 32
	planMaxDeps       = 8
	planStepCells     = 9 + planMaxDeps                    // 17
	planCells         = 4 + planMaxSteps*planStepCells + 1 // 549
	planSummaryCells  = 7
	stepEvidenceCells = 4
)

const (
	planCreateSQL = `INSERT INTO execution_plans (agent_name, task, status)
	                 VALUES ($1, $2, 'pending') RETURNING id`

	planAddStepSQL = `INSERT INTO plan_steps
	                      (plan_id, seq, action, precondition, success_predicate,
	                       rollback, deps, status)
	                  VALUES ($1, $2, $3, $4, $5, $6, $7, 'pending')`

	planGetSQL = `SELECT id, agent_name, task, status FROM execution_plans WHERE id = $1`

	planStepsSQL = `SELECT id, seq, action, precondition, success_predicate, rollback,
	                       status, output, deps
	                  FROM plan_steps WHERE plan_id = $1 ORDER BY seq LIMIT $2`

	planListIDsSQL = `SELECT id FROM execution_plans ORDER BY id DESC LIMIT $1`

	planExistsSQL = `SELECT 1 FROM execution_plans WHERE id = $1`

	planCountStepsSQL = `SELECT COUNT(*) FROM plan_steps WHERE plan_id = $1`

	planListRunningSQL = `SELECT id FROM execution_plans WHERE status = 'running'
	                       ORDER BY id LIMIT $1`

	// One statement rather than three: the C ran a correlated subquery per
	// count, twice per row.
	planRecentSummariesSQL = `SELECT p.id, p.agent_name, p.task, p.status,
	                                 to_char(p.created_at AT TIME ZONE 'utc',
	                                         'YYYY-MM-DD HH24:MI:SS'),
	                                 COUNT(s.id),
	                                 COUNT(s.id) FILTER (WHERE s.status = 'done')
	                            FROM execution_plans p
	                            LEFT JOIN plan_steps s ON s.plan_id = p.id
	                           GROUP BY p.id
	                           ORDER BY p.id DESC
	                           LIMIT $1`

	planSetStatusSQL = `UPDATE execution_plans SET status = $2 WHERE id = $1`

	// Cancelling only touches a plan that is still going. The state is in the
	// WHERE clause, so a plan that finished between the read and the write is
	// not resurrected as cancelled.
	planCancelSQL = `UPDATE execution_plans
	                    SET status = 'cancelled', cancelled_at = now(), cancel_reason = $2
	                  WHERE id = $1 AND status IN ('pending', 'running')`

	// The threshold is a parameter.
	//
	// The C formatted it into the statement with snprintf -- "-%d seconds" --
	// and re-prepared the statement for every distinct threshold, which is a
	// plan-cache miss per value and a statement built by string concatenation
	// for no reason: the number was never anything but a number.
	planCancelStaleSQL = `UPDATE execution_plans
	                         SET status = 'cancelled', cancelled_at = now(),
	                             cancel_reason = $2
	                       WHERE status = 'running'
	                         AND created_at < now() - make_interval(secs => $1::bigint)`

	stepSetStatusSQL = `UPDATE plan_steps SET status = $2 WHERE id = $1`

	stepSetStatusOutputSQL = `UPDATE plan_steps SET status = $2, output = $3 WHERE id = $1`

	// Failing a plan's live steps. The C also matched status '0' and '1',
	// because an older writer had put numbers in the column; the schema admits
	// only the names now, so there is one spelling to match.
	stepCancelActiveSQL = `UPDATE plan_steps
	                          SET status = 'failed', finished_at = now()
	                        WHERE plan_id = $1 AND status IN ('pending', 'running')`

	// A step is orphaned when it is still running but its plan is not.
	stepCancelOrphansSQL = `UPDATE plan_steps s
	                           SET status = 'failed', finished_at = now()
	                          FROM execution_plans p
	                         WHERE p.id = s.plan_id
	                           AND s.status = 'running'
	                           AND p.status <> 'running'`

	stepEvidenceInsertSQL = `INSERT INTO step_evidence
	                             (plan_id, step_id, kind, content, passed, strength)
	                         VALUES ($1, $2, $3, $4, $5, $6)`

	stepEvidenceLatestSQL = `SELECT strength, passed, kind,
	                                to_char(created_at AT TIME ZONE 'utc',
	                                        'YYYY-MM-DD HH24:MI:SS')
	                           FROM step_evidence WHERE step_id = $1
	                           ORDER BY id DESC LIMIT 1`
)

// planStep is one step as the caller sends it.
//
// "after" is the caller's name for what the column calls deps, kept because it
// is the wire's name and renaming it here would silently drop every dependency
// a caller declared.
type planStep struct {
	Action           string   `json:"action"`
	Precondition     string   `json:"precondition"`
	SuccessPredicate string   `json:"success_predicate"`
	Rollback         string   `json:"rollback"`
	After            []string `json:"after"`
}

// planCreate records a plan and its steps together.
//
// One transaction, and a step that will not insert fails the whole create.
//
// The C ignored every step insert's result -- the call was cast to void -- so a
// plan whose steps failed to write came back with an id and looked fine. The
// caller then had a plan that could never run and no indication of why. A plan
// without its steps is not a plan.
func planCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	agentName, task, stepsJSON := f[0], f[1], f[2]
	if task == "" {
		return store.StatusInvalid, nil, nil
	}

	var steps []planStep
	if strings.TrimSpace(stepsJSON) != "" {
		if err := json.Unmarshal([]byte(stepsJSON), &steps); err != nil {
			return store.StatusInvalid, nil, nil
		}
	}
	// More steps than the reply can carry would produce a plan that cannot be
	// read back in full, which is worse than refusing to make it.
	if len(steps) > planMaxSteps {
		return store.StatusInvalid, nil, nil
	}

	var planID int64
	if err := q.QueryRow(ctx, planCreateSQL, agentName, task).Scan(&planID); err != nil {
		return store.StatusFailed, nil, err
	}
	for i, step := range steps {
		deps := step.After
		if deps == nil {
			deps = []string{}
		}
		encoded, err := json.Marshal(deps)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		if _, err := q.Exec(ctx, planAddStepSQL, planID, i,
			step.Action, step.Precondition, step.SuccessPredicate,
			step.Rollback, string(encoded)); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	return store.StatusOK, []string{store.I64toa(planID)}, nil
}

// planGet answers with the plan and its steps, padded to the fixed width the
// wire carries.
func planGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var (
		planID                  int64
		agentName, task, status string
	)
	err := q.QueryRow(ctx, planGetSQL, id).Scan(&planID, &agentName, &task, &status)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}

	out := make([]string, planCells)
	out[0], out[1], out[2], out[3] = store.I64toa(planID), agentName, task, status
	// Every unused slot is an explicit empty or zero rather than left nil, so
	// the frame is the same width whatever the plan holds.
	for i := 4; i < planCells; i++ {
		out[i] = ""
	}

	rows, err := q.Query(ctx, planStepsSQL, id, planMaxSteps)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	defer rows.Close()

	count := 0
	for rows.Next() {
		var (
			stepID, seq                                   int64
			action, precondition, predicate, rollbackText string
			stepStatus, output, deps                      string
		)
		if err := rows.Scan(&stepID, &seq, &action, &precondition, &predicate,
			&rollbackText, &stepStatus, &output, &deps); err != nil {
			return store.StatusFailed, nil, err
		}
		// The dependencies are stored as a JSON array and travel as eight
		// fixed slots plus a count, which is what the wire declares. A step
		// with more dependencies than slots reports the count it actually has
		// and fills what fits: truncating the COUNT as well would tell the
		// caller the step depends on fewer things than it does.
		var depends []string
		if err := json.Unmarshal([]byte(deps), &depends); err != nil {
			depends = nil
		}

		at := 4 + count*planStepCells
		out[at] = store.I64toa(stepID)
		out[at+1] = action
		out[at+2] = precondition
		out[at+3] = predicate
		out[at+4] = rollbackText
		for d := 0; d < planMaxDeps; d++ {
			if d < len(depends) {
				out[at+5+d] = depends[d]
				continue
			}
			out[at+5+d] = ""
		}
		out[at+5+planMaxDeps] = store.Itoa(len(depends))
		out[at+6+planMaxDeps] = stepStatus
		out[at+7+planMaxDeps] = output
		out[at+8+planMaxDeps] = store.I64toa(seq) // wave
		count++
	}
	if err := rows.Err(); err != nil {
		return store.StatusFailed, nil, err
	}
	out[planCells-1] = store.Itoa(count)
	return store.StatusOK, out, nil
}

func planListIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, planListIDsSQL, max)
}

// planExists answers with nothing; the status carries the answer.
func planExists(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var one int
	err := q.QueryRow(ctx, planExistsSQL, id).Scan(&one)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func planCountSteps(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var n int64
	if err := q.QueryRow(ctx, planCountStepsSQL, id).Scan(&n); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(n)}, nil
}

func planListRunningIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, planListRunningSQL, max)
}

func planListRecentSummaries(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, planRecentSummariesSQL, planSummaryCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				id, total, done             int64
				agentName, task, status, at string
			)
			if err := scan(&id, &agentName, &task, &status, &at, &total, &done); err != nil {
				return nil, err
			}
			return []string{
				store.I64toa(id), agentName, task, status, at,
				store.I64toa(total), store.I64toa(done),
			}, nil
		}, max)
}

// changed answers with how many rows an update touched.
func changed(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	tag, err := q.Exec(ctx, sql, args...)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func planSetStatus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return changed(ctx, q, planSetStatusSQL, id, f[1])
}

func planCancelByID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "cancelled"
	}
	return changed(ctx, q, planCancelSQL, id, reason)
}

func planCancelStale(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	threshold, ok := store.Atoi64(f[0])
	if !ok || threshold <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "orphan cleanup"
	}
	return changed(ctx, q, planCancelStaleSQL, threshold, reason)
}

func stepSetStatus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return changed(ctx, q, stepSetStatusSQL, id, f[1])
}

func stepSetStatusOutput(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return changed(ctx, q, stepSetStatusOutputSQL, id, f[1], f[2])
}

func stepCancelActiveForPlan(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return changed(ctx, q, stepCancelActiveSQL, id)
}

// stepCancelOrphans fails steps still running under a plan that is not.
//
// The C expressed "its plan is not running" as a NOT IN over a subquery of
// running plan ids, which also caught steps whose plan had been deleted -- and
// then could not fail them, because the row was gone. A join says the same
// thing about the plans that exist, and the cascade handles the rest.
func stepCancelOrphans(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	return changed(ctx, q, stepCancelOrphansSQL)
}

func stepEvidenceInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	planID, ok := store.Atoi64(f[0])
	if !ok || planID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	stepID, ok := store.Atoi64(f[1])
	if !ok || stepID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if f[2] == "" || f[3] == "" {
		return store.StatusInvalid, nil, nil
	}
	passed, ok := store.Atob(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	strength := f[5]
	if strength == "" {
		strength = "weak"
	}
	if _, err := q.Exec(ctx, stepEvidenceInsertSQL,
		planID, stepID, f[2], f[3], passed, strength); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func stepEvidenceGetLatest(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	stepID, ok := store.Atoi64(f[0])
	if !ok || stepID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var (
		strength, kind, at string
		passed             bool
	)
	err := q.QueryRow(ctx, stepEvidenceLatestSQL, stepID).Scan(&strength, &passed, &kind, &at)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{strength, store.Btoa(passed), kind, at}, nil
}
