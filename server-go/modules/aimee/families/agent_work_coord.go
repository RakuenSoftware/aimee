package families

import (
	"context"
	"encoding/json"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Coordination jobs and the tasks that make them up.

// Reply widths, from the catalog.
const (
	coordTaskCells     = 11
	coordJobCells      = 10
	coordDispatchCells = 5
)

const coordTaskColumns = `id, job_id, COALESCE(step_id, 0), status, claimed_by,
                          COALESCE(to_char(claimed_at AT TIME ZONE 'utc',
                                   'YYYY-MM-DD HH24:MI:SS'), ''),
                          files::text, result, error, preempt_requeues,
                          to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

// A job carries its own task tallies, counted where the tasks are.
const coordJobColumns = `j.id, j.plan_id, j.status, j.max_concurrent,
                         to_char(j.created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                         to_char(j.updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                         COUNT(t.id),
                         COUNT(t.id) FILTER (WHERE t.status = 'done'),
                         COUNT(t.id) FILTER (WHERE t.status = 'failed'),
                         COUNT(t.id) FILTER (WHERE t.status IN ('claimed', 'running'))`

const (
	coordJobCreateSQL = `INSERT INTO coord_jobs (plan_id, max_concurrent)
	                     VALUES ($1, $2) RETURNING id`

	coordTaskAddSQL = `INSERT INTO coord_job_tasks
	                       (job_id, step_id, files, role, prompt, cwd, persona)
	                   VALUES ($1, NULLIF($2::bigint, 0), $3::jsonb, $4, $5, $6, $7)
	                   RETURNING id`

	// Claiming the next task, as ONE statement.
	//
	// The C took SQLite's whole-database write lock, read every held task's
	// file list into a fixed array of 64, walked the pending tasks in C, and
	// compared JSON documents pairwise. Three things follow from that which do
	// not follow from this:
	//
	//   A job with more than 64 tasks held at once had the rest unchecked, so a
	//   task overlapping the 65th was handed out anyway and two delegates edited
	//   the same file.
	//
	//   A task whose files would not parse compared as conflicting with
	//   nothing, so malformed input was handed out alongside anything. The
	//   column is JSONB with an array CHECK now, so it cannot be stored at all.
	//
	//   Every other writer in the store waited behind a queue poll.
	//
	// The overlap is `?|`: does this task's file list share any element with a
	// held task's. SKIP LOCKED means two claimants take two different tasks.
	coordTaskClaimNextSQL = `UPDATE coord_job_tasks
	                            SET status = 'claimed',
	                                claimed_by = $2,
	                                claimed_at = now()
	                          WHERE id = (
	                              SELECT t.id FROM coord_job_tasks t
	                               WHERE t.job_id = $1 AND t.status = 'pending'
	                                 AND NOT EXISTS (
	                                     SELECT 1 FROM coord_job_tasks held
	                                      WHERE held.job_id = t.job_id
	                                        AND held.status IN ('claimed', 'running')
	                                        AND held.files ?| ARRAY(
	                                            SELECT jsonb_array_elements_text(t.files)))
	                               ORDER BY t.id
	                               LIMIT 1
	                               FOR UPDATE SKIP LOCKED)
	                      RETURNING ` + coordTaskColumns

	// Finishing a task releases it. The "_owned" forms additionally require the
	// caller to be the holder, so a delegate cannot finish a task that was
	// taken away from it.
	coordTaskCompleteSQL = `UPDATE coord_job_tasks
	                           SET status = 'done', result = $2,
	                               claimed_by = '', claimed_at = NULL
	                         WHERE id = $1 AND status IN ('claimed', 'running')`

	coordTaskCompleteOwnedSQL = `UPDATE coord_job_tasks
	                                SET status = 'done', result = $3,
	                                    claimed_by = '', claimed_at = NULL
	                              WHERE id = $1 AND claimed_by = $2
	                                AND status IN ('claimed', 'running')`

	coordTaskFailSQL = `UPDATE coord_job_tasks
	                       SET status = 'failed', error = $2,
	                           claimed_by = '', claimed_at = NULL
	                     WHERE id = $1 AND status IN ('claimed', 'running')`

	coordTaskFailOwnedSQL = `UPDATE coord_job_tasks
	                            SET status = 'failed', error = $3,
	                                claimed_by = '', claimed_at = NULL
	                          WHERE id = $1 AND claimed_by = $2
	                            AND status IN ('claimed', 'running')`

	// Releasing puts a task back in the queue.
	coordTaskReleaseSQL = `UPDATE coord_job_tasks
	                          SET status = 'pending', claimed_by = '', claimed_at = NULL,
	                              preempt_requeues = preempt_requeues + 1
	                        WHERE id = $1 AND status IN ('claimed', 'running')`

	// The bounded form gives up rather than requeueing forever: a task that has
	// been preempted too many times is failed, because putting it back a
	// hundredth time is not going to work either.
	coordTaskReleaseBoundedSQL = `UPDATE coord_job_tasks
	                                 SET status = CASE WHEN preempt_requeues + 1 > $2
	                                                   THEN 'failed' ELSE 'pending' END,
	                                     error = CASE WHEN preempt_requeues + 1 > $2
	                                                  THEN 'preempted too many times'
	                                                  ELSE error END,
	                                     claimed_by = '', claimed_at = NULL,
	                                     preempt_requeues = preempt_requeues + 1
	                               WHERE id = $1 AND status IN ('claimed', 'running')`

	coordTaskReleaseBoundedOwnedSQL = `UPDATE coord_job_tasks
	                                      SET status = CASE WHEN preempt_requeues + 1 > $3
	                                                        THEN 'failed' ELSE 'pending' END,
	                                          error = CASE WHEN preempt_requeues + 1 > $3
	                                                       THEN 'preempted too many times'
	                                                       ELSE error END,
	                                          claimed_by = '', claimed_at = NULL,
	                                          preempt_requeues = preempt_requeues + 1
	                                    WHERE id = $1 AND claimed_by = $2
	                                      AND status IN ('claimed', 'running')`

	// Recovering everything one owner held, in one statement, reporting how
	// much went back to the queue and how much was given up on.
	coordOwnerRecoverSQL = `WITH recovered AS (
	        UPDATE coord_job_tasks
	           SET status = CASE WHEN preempt_requeues + 1 > $2
	                             THEN 'failed' ELSE 'pending' END,
	               error = CASE WHEN preempt_requeues + 1 > $2
	                            THEN 'owner lost, preempted too many times' ELSE error END,
	               claimed_by = '', claimed_at = NULL,
	               preempt_requeues = preempt_requeues + 1
	         WHERE claimed_by = $1 AND status IN ('claimed', 'running')
	     RETURNING status)
	    SELECT COUNT(*) FILTER (WHERE status = 'pending'),
	           COUNT(*) FILTER (WHERE status = 'failed')
	      FROM recovered`

	coordJobGetSQL = `SELECT ` + coordJobColumns + `
	                    FROM coord_jobs j
	                    LEFT JOIN coord_job_tasks t ON t.job_id = j.id
	                   WHERE j.id = $1
	                   GROUP BY j.id`

	coordTaskListSQL = `SELECT ` + coordTaskColumns + `
	                      FROM coord_job_tasks WHERE job_id = $1
	                      ORDER BY id LIMIT $2`

	coordJobCancelSQL = `UPDATE coord_jobs
	                        SET status = 'cancelled', updated_at = now()
	                      WHERE id = $1 AND status IN ('pending', 'running')`

	// A job's status follows its tasks: done when they all finished, failed
	// when any did, running while any is held, pending otherwise.
	coordJobRefreshStatusSQL = `UPDATE coord_jobs j
	                               SET status = s.derived, updated_at = now()
	                              FROM (SELECT
	                                        CASE
	                                            WHEN COUNT(*) = 0 THEN 'pending'
	                                            WHEN COUNT(*) FILTER (
	                                                WHERE status = 'failed') > 0 THEN 'failed'
	                                            WHEN COUNT(*) FILTER (
	                                                WHERE status <> 'done') = 0 THEN 'done'
	                                            WHEN COUNT(*) FILTER (
	                                                WHERE status IN ('claimed', 'running')) > 0
	                                                THEN 'running'
	                                            ELSE 'pending'
	                                        END AS derived
	                                      FROM coord_job_tasks WHERE job_id = $1) s
	                             WHERE j.id = $1 AND j.status <> 'cancelled'`

	// Whether a file list clashes with anything the job is already holding.
	coordJobFileConflictSQL = `SELECT EXISTS (
	                               SELECT 1 FROM coord_job_tasks
	                                WHERE job_id = $1
	                                  AND status IN ('claimed', 'running')
	                                  AND files ?| ARRAY(
	                                      SELECT jsonb_array_elements_text($2::jsonb)))`

	coordJobListRecentSQL = `SELECT ` + coordJobColumns + `
	                           FROM coord_jobs j
	                           LEFT JOIN coord_job_tasks t ON t.job_id = j.id
	                          GROUP BY j.id
	                          ORDER BY j.id DESC LIMIT $1`

	coordJobListActiveSQL = `SELECT id FROM coord_jobs
	                          WHERE status IN ('pending', 'running')
	                          ORDER BY id LIMIT $1`

	coordTaskGetDispatchSQL = `SELECT role, prompt, files::text, cwd, persona
	                             FROM coord_job_tasks WHERE id = $1`
)

func coordTaskRow(scan func(...any) error) ([]string, error) {
	var (
		id, jobID, stepID, requeues       int64
		status, claimedBy, claimedAt      string
		files, result, errText, createdAt string
	)
	if err := scan(&id, &jobID, &stepID, &status, &claimedBy, &claimedAt,
		&files, &result, &errText, &requeues, &createdAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), store.I64toa(jobID), store.I64toa(stepID),
		status, claimedBy, claimedAt, files, result, errText,
		store.I64toa(requeues), createdAt,
	}, nil
}

func coordJobRow(scan func(...any) error) ([]string, error) {
	var (
		id, planID, maxConcurrent    int64
		total, done, failed, running int64
		status, createdAt, updatedAt string
	)
	if err := scan(&id, &planID, &status, &maxConcurrent, &createdAt, &updatedAt,
		&total, &done, &failed, &running); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), store.I64toa(planID), status,
		store.I64toa(maxConcurrent), createdAt, updatedAt,
		store.I64toa(total), store.I64toa(done),
		store.I64toa(failed), store.I64toa(running),
	}, nil
}

// filesArray reads a caller's file list, refusing anything that is not an array
// of paths.
//
// The C parsed this with cJSON and treated a parse failure as "conflicts with
// nothing", so a malformed list was handed out alongside every other task. A
// list the store cannot understand is a list it cannot check, and a task it
// cannot check is one it must not dispatch.
func filesArray(raw string) (string, bool) {
	if strings.TrimSpace(raw) == "" {
		return "[]", true
	}
	var paths []string
	if err := json.Unmarshal([]byte(raw), &paths); err != nil {
		return "", false
	}
	encoded, err := json.Marshal(paths)
	if err != nil {
		return "", false
	}
	return string(encoded), true
}

func coordJobCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	planID, ok := store.Atoi64(f[0])
	if !ok || planID < 0 {
		return store.StatusInvalid, nil, nil
	}
	maxConcurrent, ok := store.Atoi64(f[1])
	if !ok || maxConcurrent <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, coordJobCreateSQL, planID, maxConcurrent).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func coordTaskAdd(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[0])
	if !ok || jobID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	stepID, ok := store.Atoi64(f[1])
	if !ok || stepID < 0 {
		return store.StatusInvalid, nil, nil
	}
	files, ok := filesArray(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	role := f[3]
	if role == "" {
		role = "execute"
	}
	persona := f[6]
	if persona == "" {
		persona = "engineer"
	}
	var id int64
	if err := q.QueryRow(ctx, coordTaskAddSQL,
		jobID, stepID, files, role, f[4], f[5], persona).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

// coordTaskClaimNext takes the next task nobody's held files clash with.
//
// MISSING means there is nothing to take: either the job has no pending tasks
// or every one of them overlaps something in flight. Both mean "wait", which is
// the only thing the caller does about it.
func coordTaskClaimNext(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[0])
	if !ok || jobID <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := coordTaskRow(func(dest ...any) error {
		return q.QueryRow(ctx, coordTaskClaimNextSQL, jobID, f[1]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

// finishTask is the shape the four complete/fail operations share.
func finishTask(sql string, owned bool) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		id, ok := store.Atoi64(f[0])
		if !ok || id <= 0 {
			return store.StatusInvalid, nil, nil
		}
		if owned {
			if f[1] == "" {
				return store.StatusInvalid, nil, nil
			}
			return touchedOrMissing(ctx, q, sql, id, f[1], f[2])
		}
		return touchedOrMissing(ctx, q, sql, id, f[1])
	}
}

func coordTaskRelease(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, coordTaskReleaseSQL, id)
}

func coordTaskReleaseBounded(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := store.Atoi64(f[1])
	if !ok || max < 0 {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, coordTaskReleaseBoundedSQL, id, max)
}

func coordTaskReleaseBoundedOwned(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	max, ok := store.Atoi64(f[2])
	if !ok || max < 0 {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, coordTaskReleaseBoundedOwnedSQL, id, f[1], max)
}

// coordOwnerRecover takes back everything one owner held.
//
// Recovering nothing is success, not a miss: the owner held nothing, which is
// what the caller wanted to be true.
func coordOwnerRecover(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	max, ok := store.Atoi64(f[1])
	if !ok || max < 0 {
		return store.StatusInvalid, nil, nil
	}
	var requeued, failed int64
	if err := q.QueryRow(ctx, coordOwnerRecoverSQL, f[0], max).Scan(&requeued, &failed); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(requeued), store.I64toa(failed),
	}, nil
}

func coordJobGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reply, err := coordJobRow(func(dest ...any) error {
		return q.QueryRow(ctx, coordJobGetSQL, id).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func coordTaskList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, coordTaskListSQL, coordTaskCells, coordTaskRow, id, max)
}

func coordJobCancel(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, coordJobCancelSQL, id)
}

// coordJobRefreshStatus derives a job's status from its tasks.
//
// A cancelled job stays cancelled: cancellation is a decision somebody made,
// and no count of tasks overrides it. The C recomputed unconditionally, so a
// cancelled job whose last task finished came back as done.
func coordJobRefreshStatus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, coordJobRefreshStatusSQL, id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// coordJobFileConflict answers whether a file list clashes with work in flight.
// The status carries the answer: OK means it does, MISSING means it does not.
func coordJobFileConflict(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	files, ok := filesArray(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var conflict bool
	if err := q.QueryRow(ctx, coordJobFileConflictSQL, id, files).Scan(&conflict); err != nil {
		return store.StatusFailed, nil, err
	}
	if !conflict {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

func coordJobListRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, coordJobListRecentSQL, coordJobCells, coordJobRow, max)
}

func coordJobListActiveIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, coordJobListActiveSQL, max)
}

func coordTaskGetDispatch(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var role, prompt, files, cwd, persona string
	err := q.QueryRow(ctx, coordTaskGetDispatchSQL, id).Scan(
		&role, &prompt, &files, &cwd, &persona)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{role, prompt, files, cwd, persona}, nil
}
