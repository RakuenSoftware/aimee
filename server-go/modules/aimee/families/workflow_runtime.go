package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Execution traces, pipelines, roadmap dispatches, and the workflow-engine
// session binding.

// Reply widths, from the catalog.
const (
	traceRecentCells     = 5
	traceGetCells        = 10
	traceToolCallCells   = 5
	traceAfterIDCells    = 7
	pipelineCells        = 12
	roadmapDispatchCells = 10
	roadmapUnitCells     = 17
)

// --- execution traces ---------------------------------------------------------

const (
	// plan_id 0 on the wire means "not part of a plan", stored as NULL.
	traceInsertSQL = `INSERT INTO execution_trace
	                      (plan_id, session_id, turn, direction, content,
	                       tool_name, tool_args, tool_result, context_hash)
	                  VALUES (NULLIF($1::bigint, 0), $2, $3, $4, $5, $6, $7, $8, $9)`

	traceCountForSessionSQL = `SELECT COUNT(*) FROM execution_trace WHERE session_id = $1`

	traceListRecentSQL = `SELECT id, turn, direction, tool_name,
	                             to_char(created_at AT TIME ZONE 'utc',
	                                     'YYYY-MM-DD HH24:MI:SS')
	                        FROM execution_trace ORDER BY id DESC LIMIT $1`

	traceGetSQL = `SELECT id, COALESCE(plan_id, 0), turn, direction, content,
	                      tool_name, tool_args, tool_result, context_hash,
	                      to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')
	                 FROM execution_trace WHERE id = $1`

	traceListToolCallsSQL = `SELECT turn, direction, tool_name, tool_args, tool_result
	                           FROM execution_trace ORDER BY id DESC LIMIT $1`

	traceListAfterIDSQL = `SELECT id, COALESCE(plan_id, 0), turn, direction,
	                              tool_name, tool_args, tool_result
	                         FROM execution_trace
	                        WHERE id > $1 AND tool_name <> ''
	                        ORDER BY plan_id, turn, id
	                        LIMIT $2`
)

func traceInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	planID, ok := store.Atoi64(f[0])
	if !ok || planID < 0 {
		return store.StatusInvalid, nil, nil
	}
	turn, ok := store.Atoi64(f[2])
	if !ok || turn < 0 {
		return store.StatusInvalid, nil, nil
	}
	direction := f[3]
	if direction == "" {
		direction = "call"
	}
	if _, err := q.Exec(ctx, traceInsertSQL,
		planID, f[1], turn, direction, f[4], f[5], f[6], f[7], f[8]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func traceCountForSession(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var n int64
	if err := q.QueryRow(ctx, traceCountForSessionSQL, f[0]).Scan(&n); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(n)}, nil
}

func traceListRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, traceListRecentSQL, traceRecentCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				id, turn                 int64
				direction, tool, created string
			)
			if err := scan(&id, &turn, &direction, &tool, &created); err != nil {
				return nil, err
			}
			return []string{
				store.I64toa(id), store.I64toa(turn), direction, tool, created,
			}, nil
		}, max)
}

func traceGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var (
		traceID, planID, turn                        int64
		direction, content, tool, args, result, hash string
		created                                      string
	)
	err := q.QueryRow(ctx, traceGetSQL, id).Scan(&traceID, &planID, &turn, &direction,
		&content, &tool, &args, &result, &hash, &created)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(traceID), store.I64toa(planID), store.I64toa(turn),
		direction, content, tool, args, result, hash, created,
	}, nil
}

func traceListToolCalls(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, traceListToolCallsSQL, traceToolCallCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				turn                          int64
				direction, tool, args, result string
			)
			if err := scan(&turn, &direction, &tool, &args, &result); err != nil {
				return nil, err
			}
			return []string{
				store.I64toa(turn), direction, tool, args, result,
			}, nil
		}, max)
}

func traceListAfterID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	after, ok := store.Atoi64(f[0])
	if !ok || after < 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, traceListAfterIDSQL, traceAfterIDCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				id, planID, turn              int64
				direction, tool, args, result string
			)
			if err := scan(&id, &planID, &turn, &direction, &tool, &args, &result); err != nil {
				return nil, err
			}
			return []string{
				store.I64toa(id), store.I64toa(planID), store.I64toa(turn),
				direction, tool, args, result,
			}, nil
		}, after, max)
}

// --- pipelines -----------------------------------------------------------------

const pipelineColumns = `id, task, status, current_phase, request_classification, plan_depth,
                         phase_attempts, COALESCE(plan_id, 0), job_id, clarify_session_id,
                         to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                         to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	pipelineCreateSQL = `INSERT INTO pipelines
	                         (task, status, current_phase, request_classification, plan_depth)
	                     VALUES ($1, 'active', 'classify', $2, $3) RETURNING id`

	pipelineGetSQL = `SELECT ` + pipelineColumns + ` FROM pipelines WHERE id = $1`

	pipelineUpdateSQL = `UPDATE pipelines
	                        SET status = $2, current_phase = $3,
	                            request_classification = $4, plan_depth = $5,
	                            phase_attempts = $6, plan_id = NULLIF($7::bigint, 0),
	                            job_id = $8, clarify_session_id = $9,
	                            updated_at = now()
	                      WHERE id = $1`

	pipelineLinkPlanSQL = `UPDATE pipelines
	                          SET plan_id = NULLIF($2::bigint, 0), updated_at = now()
	                        WHERE id = $1`

	pipelineLinkJobSQL = `UPDATE pipelines SET job_id = $2, updated_at = now() WHERE id = $1`

	// Cancelling only touches a pipeline that is still going.
	pipelineCancelSQL = `UPDATE pipelines
	                        SET status = 'cancelled', updated_at = now()
	                      WHERE id = $1 AND status IN ('active', 'paused')`

	pipelineListActiveSQL = `SELECT ` + pipelineColumns + `
	                           FROM pipelines WHERE status IN ('active', 'paused')
	                           ORDER BY updated_at DESC, id DESC LIMIT $1`
)

func pipelineRow(scan func(...any) error) ([]string, error) {
	var (
		id, attempts, planID, jobID, clarifyID     int64
		task, status, phase, classification, depth string
		createdAt, updatedAt                       string
	)
	if err := scan(&id, &task, &status, &phase, &classification, &depth,
		&attempts, &planID, &jobID, &clarifyID, &createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), task, status, phase, classification, depth,
		store.I64toa(attempts), store.I64toa(planID),
		store.I64toa(jobID), store.I64toa(clarifyID),
		createdAt, updatedAt,
	}, nil
}

func pipelineCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	classification := f[1]
	if classification == "" {
		classification = "simple"
	}
	depth := f[2]
	if depth == "" {
		depth = "simple"
	}
	var id int64
	if err := q.QueryRow(ctx, pipelineCreateSQL, f[0], classification, depth).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func pipelineGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reply, err := pipelineRow(func(dest ...any) error {
		return q.QueryRow(ctx, pipelineGetSQL, id).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func pipelineUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	nums := make([]int64, 0, 4)
	for _, at := range []int{3, 4, 5, 8} {
		n, ok := store.Atoi64(f[at])
		if !ok || n < 0 {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	if _, err := q.Exec(ctx, pipelineUpdateSQL, id,
		f[1], f[2], f[6], f[7], nums[0], nums[1], nums[2], nums[3]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// linkPipeline is the shape both link operations share.
func linkPipeline(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		id, ok := store.Atoi64(f[0])
		if !ok || id <= 0 {
			return store.StatusInvalid, nil, nil
		}
		linked, ok := store.Atoi64(f[1])
		if !ok || linked < 0 {
			return store.StatusInvalid, nil, nil
		}
		tag, err := q.Exec(ctx, sql, id, linked)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		if tag.RowsAffected() == 0 {
			return store.StatusMissing, nil, nil
		}
		return store.StatusOK, nil, nil
	}
}

func pipelineCancel(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, pipelineCancelSQL, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

func pipelineListActive(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, pipelineListActiveSQL, pipelineCells, pipelineRow, max)
}
