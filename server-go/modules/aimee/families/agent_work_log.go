package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The cognify queue, the agent log and its aggregates, and trigger runs.

// Reply widths, from the catalog.
const (
	cognifyClaimCells    = 9
	agentLogRowCells     = 13
	agentLogFailureCells = 3
	agentLogPatternCells = 8
	agentLogSeedCells    = 4
	agentLogMetricCells  = 8
	agentLogStatsCells   = 9
	agentLogPromCells    = 8
	roleCountCells       = 2
)

// --- the cognify queue ---------------------------------------------------------

const (
	// Enqueueing the same memory twice is one job, not two.
	cognifyEnqueueSQL = `INSERT INTO memory_cognify_jobs (kind, memory_id)
	                     VALUES ('cognify_unit', $1)
	                     ON CONFLICT (kind, memory_id) DO NOTHING`

	cognifyStatusSQL = `SELECT COUNT(*) FILTER (WHERE status = 'pending'),
	                           COUNT(*) FILTER (WHERE status = 'running'),
	                           COUNT(*) FILTER (WHERE status = 'done'),
	                           COUNT(*) FILTER (WHERE status = 'failed'),
	                           COUNT(*)
	                      FROM memory_cognify_jobs`

	// Claiming the next job, as one statement.
	//
	// The C took SQLite's whole-database write lock (BEGIN IMMEDIATE), read the
	// oldest pending job, and updated it -- correct, but it serialised every
	// other writer in the store behind a queue poll.
	//
	// FOR UPDATE SKIP LOCKED is the shape this actually wants: two claimants
	// take two DIFFERENT jobs instead of one waiting for the other, and nothing
	// outside this table is held up at all.
	cognifyClaimNextSQL = `UPDATE memory_cognify_jobs
	                          SET status = 'running',
	                              attempts = attempts + 1,
	                              claimed_by = $1,
	                              claimed_at = now(),
	                              updated_at = now()
	                        WHERE id = (SELECT id FROM memory_cognify_jobs
	                                     WHERE status = 'pending'
	                                     ORDER BY id
	                                     LIMIT 1
	                                     FOR UPDATE SKIP LOCKED)
	                    RETURNING id, memory_id, attempts, max_attempts, kind, status,
	                              claimed_by,
	                              COALESCE(to_char(claimed_at AT TIME ZONE 'utc',
	                                       'YYYY-MM-DD HH24:MI:SS'), ''),
	                              last_error`

	// Marking a job done or failed releases it: a finished job that still names
	// a claimant reads as held forever.
	cognifyMarkSQL = `UPDATE memory_cognify_jobs
	                     SET status = $2, last_error = $3,
	                         claimed_by = '', claimed_at = NULL, updated_at = now()
	                   WHERE id = $1`
)

func cognifyEnqueue(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	memoryID, ok := store.Atoi64(f[0])
	if !ok || memoryID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	// DO NOTHING makes a repeat a no-op, so this succeeds either way: the
	// caller asked for the memory to be queued and it is.
	if _, err := q.Exec(ctx, cognifyEnqueueSQL, memoryID); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func cognifyStatus(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var pending, running, done, failed, total int64
	if err := q.QueryRow(ctx, cognifyStatusSQL).Scan(
		&pending, &running, &done, &failed, &total); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(pending), store.I64toa(running), store.I64toa(done),
		store.I64toa(failed), store.I64toa(total),
	}, nil
}

// cognifyClaimNext takes the oldest pending job. MISSING means the queue is
// empty, which is an answer rather than a failure.
func cognifyClaimNext(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var (
		id, memoryID, attempts, maxAttempts int64
		kind, status, claimedBy, claimedAt  string
		lastError                           string
	)
	err := q.QueryRow(ctx, cognifyClaimNextSQL, "cognify-worker").Scan(
		&id, &memoryID, &attempts, &maxAttempts, &kind, &status,
		&claimedBy, &claimedAt, &lastError)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(id), store.I64toa(memoryID),
		store.I64toa(attempts), store.I64toa(maxAttempts),
		kind, status, claimedBy, claimedAt, lastError,
	}, nil
}

func cognifyMark(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, cognifyMarkSQL, id, f[1], f[2])
}

// --- the agent log --------------------------------------------------------------

const agentLogRowColumns = `id, agent_name, role, prompt_tokens, completion_tokens,
                            latency_ms, success, turns, tool_calls, confidence, session_id,
                            to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                            error`

// sinceSecs bounds a window given in seconds, where <= 0 means "all of it".
const sinceSecsFilter = `($1 <= 0 OR created_at > now() - make_interval(secs => $1::bigint))`

const (
	agentLogInsertSQL = `INSERT INTO agent_log
	                         (agent_name, role, prompt_tokens, completion_tokens, latency_ms,
	                          success, error, turns, tool_calls, confidence, session_id)
	                     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)
	                     RETURNING id`

	agentLogListRecentSQL = `SELECT ` + agentLogRowColumns + `
	                           FROM agent_log ORDER BY id DESC LIMIT $1`

	agentLogListBySessionSQL = `SELECT ` + agentLogRowColumns + `
	                              FROM agent_log WHERE session_id = $1
	                              ORDER BY id DESC LIMIT $2`

	// The sessions whose role matches a pattern.
	//
	// The C wrote "SELECT DISTINCT session_id ... ORDER BY id DESC", which
	// orders by a column that is not in the DISTINCT list. SQLite picks a row
	// arbitrarily; PostgreSQL rejects it outright, which forces the question
	// the C never answered: which of a session's rows decides its position.
	// DISTINCT ON answers it -- the most recent.
	//
	// The pattern is the CALLER's, wildcards and all: the wire field is named
	// "pattern" and the C bound it unwrapped, so composing it is the caller's
	// job. This is the opposite of the searches that take raw text.
	agentLogSearchSessionsSQL = `SELECT session_id
	                               FROM (SELECT DISTINCT ON (session_id) session_id, id
	                                       FROM agent_log
	                                      WHERE session_id <> '' AND role LIKE $1
	                                      ORDER BY session_id, id DESC) recent
	                              ORDER BY id DESC
	                              LIMIT $2`

	agentLogCountPerRoleSQL = `SELECT role, COUNT(*) FROM agent_log
	                            WHERE ` + sinceSecsFilter + `
	                            GROUP BY role ORDER BY COUNT(*) DESC, role LIMIT $2`

	agentLogFailuresSinceSQL = `SELECT role, error,
	                                   to_char(created_at AT TIME ZONE 'utc',
	                                           'YYYY-MM-DD HH24:MI:SS')
	                              FROM agent_log
	                             WHERE NOT success AND ` + sinceSecsFilter + `
	                             ORDER BY id DESC LIMIT $2`

	agentLogRecentErrorsSQL = `SELECT error FROM agent_log
	                            WHERE NOT success AND error <> ''
	                              AND created_at > now() - make_interval(days => $1::int)
	                            ORDER BY id DESC LIMIT $2`

	// Which role/agent pairings actually work out.
	agentLogDelegationPatternsSQL = `SELECT role, agent_name,
	                                        COUNT(*) FILTER (WHERE success),
	                                        COUNT(*) FILTER (WHERE NOT success),
	                                        COUNT(*),
	                                        COALESCE(AVG(turns), 0),
	                                        COALESCE(AVG(tool_calls), 0),
	                                        COALESCE(MAX(error) FILTER (WHERE NOT success), '')
	                                   FROM agent_log
	                                  WHERE created_at > now() - make_interval(days => $1::int)
	                                  GROUP BY role, agent_name
	                                 HAVING COUNT(*) >= $2
	                                  ORDER BY COUNT(*) DESC, role, agent_name
	                                  LIMIT $3`

	// Where failures cluster, and what they said.
	agentLogFailureSeedsSQL = `SELECT role, agent_name, COUNT(*),
	                                  string_agg(DISTINCT error, ' | ')
	                             FROM agent_log
	                            WHERE NOT success AND error <> ''
	                              AND created_at > now() - make_interval(days => $1::int)
	                            GROUP BY role, agent_name
	                           HAVING COUNT(*) >= $2
	                            ORDER BY COUNT(*) DESC, role, agent_name
	                            LIMIT $3`

	// These two join token_audit, which belongs to the telemetry family. Both
	// tables live in the same store, so the join is ordinary SQL -- what does
	// NOT cross the boundary is a foreign key, because each family's schema
	// loads on its own.
	agentLogMetricsByRoleSQL = `SELECT al.role, COUNT(*),
	                                   COUNT(*) FILTER (WHERE al.success),
	                                   COALESCE(AVG(al.latency_ms), 0),
	                                   COALESCE(SUM(al.prompt_tokens + al.completion_tokens), 0),
	                                   COALESCE(SUM(ta.cache_write_tokens), 0),
	                                   COALESCE(SUM(ta.cache_read_tokens), 0),
	                                   COALESCE(SUM(ta.estimated_cost_usd), 0)
	                              FROM agent_log al
	                              LEFT JOIN token_audit ta ON ta.agent_log_id = al.id
	                             GROUP BY al.role
	                             ORDER BY COUNT(*) DESC, al.role
	                             LIMIT $1`

	agentLogAgentStatsSQL = `SELECT al.agent_name, COUNT(*),
	                                COALESCE(SUM(al.prompt_tokens), 0),
	                                COALESCE(SUM(al.completion_tokens), 0),
	                                COALESCE(AVG(al.latency_ms), 0),
	                                COALESCE(AVG(CASE WHEN al.success THEN 1.0 ELSE 0.0 END), 0),
	                                COALESCE(SUM(ta.cache_write_tokens), 0),
	                                COALESCE(SUM(ta.cache_read_tokens), 0),
	                                COALESCE(SUM(ta.estimated_cost_usd), 0)
	                           FROM agent_log al
	                           LEFT JOIN token_audit ta ON ta.agent_log_id = al.id
	                          WHERE ($1 = '' OR al.agent_name = $1)
	                          GROUP BY al.agent_name
	                          ORDER BY COUNT(*) DESC, al.agent_name
	                          LIMIT $2`

	agentLogHUDSummarySQL = `SELECT COUNT(*),
	                                COUNT(*) FILTER (WHERE success),
	                                COUNT(*) FILTER (WHERE NOT success),
	                                COALESCE(SUM(prompt_tokens)::bigint, 0),
	                                COALESCE(SUM(completion_tokens)::bigint, 0),
	                                COALESCE(SUM(turns)::bigint, 0),
	                                COALESCE(SUM(tool_calls)::bigint, 0),
	                                COALESCE(AVG(latency_ms), 0),
	                                COUNT(*) FILTER (
	                                    WHERE created_at > now()
	                                                     - make_interval(secs => $1::bigint)),
	                                COUNT(*) FILTER (
	                                    WHERE success AND created_at > now()
	                                                     - make_interval(secs => $1::bigint))
	                           FROM agent_log`

	agentLogSessionOutcomeSQL = `SELECT COUNT(*) FILTER (WHERE success), COUNT(*)
	                               FROM agent_log WHERE session_id = $1`

	agentLogPrometheusSQL = `SELECT agent_name, role, COUNT(*),
	                                COUNT(*) FILTER (WHERE success),
	                                COALESCE(SUM(prompt_tokens)::bigint, 0),
	                                COALESCE(SUM(completion_tokens)::bigint, 0),
	                                COALESCE(AVG(latency_ms), 0),
	                                COALESCE(SUM(tool_calls)::bigint, 0)
	                           FROM agent_log
	                          GROUP BY agent_name, role
	                          ORDER BY COUNT(*) DESC, agent_name, role
	                          LIMIT $1`

	agentLogStatsSQL = `SELECT COUNT(*),
	                           COALESCE(SUM(turns)::bigint, 0),
	                           COALESCE(SUM(tool_calls)::bigint, 0),
	                           COALESCE(SUM(prompt_tokens)::bigint, 0),
	                           COALESCE(SUM(completion_tokens)::bigint, 0),
	                           COUNT(*) FILTER (WHERE success)
	                      FROM agent_log WHERE ` + sinceSecsFilter
)

func agentLogRow(scan func(...any) error) ([]string, error) {
	var (
		id, prompt, completion, latency, turns, tools, confidence int64
		agentName, role, sessionID, createdAt, errText            string
		success                                                   bool
	)
	if err := scan(&id, &agentName, &role, &prompt, &completion, &latency,
		&success, &turns, &tools, &confidence, &sessionID, &createdAt, &errText); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), agentName, role,
		store.I64toa(prompt), store.I64toa(completion), store.I64toa(latency),
		store.Btoa(success), store.I64toa(turns), store.I64toa(tools),
		store.I64toa(confidence), sessionID, createdAt, errText,
	}, nil
}

func agentLogInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	nums := make([]int64, 0, 5)
	for _, at := range []int{2, 3, 4, 7, 8} {
		n, ok := store.Atoi64(f[at])
		if !ok || n < 0 {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	success, ok := store.Atob(f[5])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	confidence, ok := store.Atoi64(f[9])
	if !ok || confidence < -1 || confidence > 100 {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, agentLogInsertSQL,
		f[0], f[1], nums[0], nums[1], nums[2], success, f[6],
		nums[3], nums[4], confidence, f[10]).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func agentLogListRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogListRecentSQL, agentLogRowCells, agentLogRow, max)
}

func agentLogListBySession(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogListBySessionSQL, agentLogRowCells, agentLogRow, f[0], max)
}

func agentLogSearchSessionsByRole(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, agentLogSearchSessionsSQL, f[0], max)
}

func agentLogCountPerRole(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogCountPerRoleSQL, roleCountCells,
		func(scan func(...any) error) ([]string, error) {
			var role string
			var n int64
			if err := scan(&role, &n); err != nil {
				return nil, err
			}
			return []string{role, store.I64toa(n)}, nil
		}, since, max)
}

func agentLogFailuresSince(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	since, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogFailuresSinceSQL, agentLogFailureCells,
		func(scan func(...any) error) ([]string, error) {
			var role, errText, at string
			if err := scan(&role, &errText, &at); err != nil {
				return nil, err
			}
			return []string{role, errText, at}, nil
		}, since, max)
}

func agentLogListRecentErrors(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	days, ok := store.Atoi(f[0])
	if !ok || days < 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, agentLogRecentErrorsSQL, days, max)
}

// windowMinMax reads the three fields the two clustering aggregates share.
func windowMinMax(f []string) (days int, min int64, max int, ok bool) {
	days, ok = store.Atoi(f[0])
	if !ok || days < 0 {
		return 0, 0, 0, false
	}
	min, ok = store.Atoi64(f[1])
	if !ok || min < 0 {
		return 0, 0, 0, false
	}
	max, ok = boundedMax(f[2])
	return days, min, max, ok
}

func agentLogDelegationPatterns(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	days, min, max, ok := windowMinMax(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogDelegationPatternsSQL, agentLogPatternCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				role, agentName, recentError string
				wins, fails, total           int64
				avgTurns, avgTools           float64
			)
			if err := scan(&role, &agentName, &wins, &fails, &total,
				&avgTurns, &avgTools, &recentError); err != nil {
				return nil, err
			}
			return []string{
				role, agentName,
				store.I64toa(wins), store.I64toa(fails), store.I64toa(total),
				store.Ftoa(avgTurns), store.Ftoa(avgTools), recentError,
			}, nil
		}, days, min, max)
}

func agentLogFailureSeeds(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	days, min, max, ok := windowMinMax(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogFailureSeedsSQL, agentLogSeedCells,
		func(scan func(...any) error) ([]string, error) {
			var role, agentName, errors string
			var fails int64
			if err := scan(&role, &agentName, &fails, &errors); err != nil {
				return nil, err
			}
			return []string{role, agentName, store.I64toa(fails), errors}, nil
		}, days, min, max)
}

func agentLogMetricsByRole(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogMetricsByRoleSQL, agentLogMetricCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				role                          string
				total, successes              int64
				tokens, cacheWrite, cacheRead int64
				avgLatency, cost              float64
			)
			if err := scan(&role, &total, &successes, &avgLatency, &tokens,
				&cacheWrite, &cacheRead, &cost); err != nil {
				return nil, err
			}
			return []string{
				role, store.I64toa(total), store.I64toa(successes),
				store.Ftoa(avgLatency), store.I64toa(tokens),
				store.I64toa(cacheWrite), store.I64toa(cacheRead),
				store.Ftoa(cost),
			}, nil
		}, max)
}

func agentLogAgentStats(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogAgentStatsSQL, agentLogStatsCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				agentName                     string
				calls, prompt, completion     int64
				cacheWrite, cacheRead         int64
				avgLatency, successRate, cost float64
			)
			if err := scan(&agentName, &calls, &prompt, &completion, &avgLatency,
				&successRate, &cacheWrite, &cacheRead, &cost); err != nil {
				return nil, err
			}
			return []string{
				agentName, store.I64toa(calls),
				store.I64toa(prompt), store.I64toa(completion),
				store.Ftoa(avgLatency), store.Ftoa(successRate),
				store.I64toa(cacheWrite), store.I64toa(cacheRead),
				store.Ftoa(cost),
			}, nil
		}, f[0], max)
}

func agentLogHUDSummary(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	recent, ok := store.Atoi64(f[0])
	if !ok || recent < 0 {
		return store.StatusInvalid, nil, nil
	}
	var (
		total, successes, failures, prompt, completion int64
		turns, tools, recentCalls, recentSuccesses     int64
		avgLatency                                     float64
	)
	if err := q.QueryRow(ctx, agentLogHUDSummarySQL, recent).Scan(
		&total, &successes, &failures, &prompt, &completion,
		&turns, &tools, &avgLatency, &recentCalls, &recentSuccesses); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(total), store.I64toa(successes), store.I64toa(failures),
		store.I64toa(prompt), store.I64toa(completion),
		store.I64toa(turns), store.I64toa(tools),
		store.Ftoa(avgLatency),
		store.I64toa(recentCalls), store.I64toa(recentSuccesses),
	}, nil
}

func agentLogSessionOutcome(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var successes, total int64
	if err := q.QueryRow(ctx, agentLogSessionOutcomeSQL, f[0]).Scan(&successes, &total); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(successes), store.I64toa(total),
	}, nil
}

func agentLogPrometheus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, agentLogPrometheusSQL, agentLogPromCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				agentName, role                      string
				total, successes, prompt, completion int64
				tools                                int64
				avgLatency                           float64
			)
			if err := scan(&agentName, &role, &total, &successes, &prompt,
				&completion, &avgLatency, &tools); err != nil {
				return nil, err
			}
			return []string{
				agentName, role, store.I64toa(total), store.I64toa(successes),
				store.I64toa(prompt), store.I64toa(completion),
				store.Ftoa(avgLatency), store.I64toa(tools),
			}, nil
		}, max)
}

func agentLogStats(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var total, turns, tools, prompt, completion, successes int64
	if err := q.QueryRow(ctx, agentLogStatsSQL, since).Scan(
		&total, &turns, &tools, &prompt, &completion, &successes); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(total), store.I64toa(turns), store.I64toa(tools),
		store.I64toa(prompt), store.I64toa(completion), store.I64toa(successes),
	}, nil
}

// --- trigger runs -----------------------------------------------------------------

const (
	triggerInsertSQL = `INSERT INTO trigger_runs (id, source, event, task, workspace, metadata)
	                    VALUES ($1, $2, $3, $4, $5, $6)`

	// The timestamps follow the status rather than being set separately: a run
	// that reads as running started, and one that reads as finished stopped.
	// The C set status from one statement and left the two timestamps to the
	// caller, so a run could read as finished having never started.
	triggerStatusSetSQL = `UPDATE trigger_runs
	                          SET status = $2,
	                              pipeline_id = $3,
	                              error = $4,
	                              started_at = CASE WHEN $2 = 'running' THEN now()
	                                                ELSE started_at END,
	                              finished_at = CASE
	                                  WHEN $2 IN ('done', 'failed', 'cancelled')
	                                  THEN now() ELSE finished_at END
	                        WHERE id = $1`

	triggerGetSQL = `SELECT id, source, event, task, workspace, metadata, pipeline_id, status,
	                        to_char(queued_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
	                        COALESCE(to_char(started_at AT TIME ZONE 'utc',
	                                 'YYYY-MM-DD HH24:MI:SS'), ''),
	                        COALESCE(to_char(finished_at AT TIME ZONE 'utc',
	                                 'YYYY-MM-DD HH24:MI:SS'), ''),
	                        error
	                   FROM trigger_runs WHERE id = $1`

	// The list is rendered as one JSON document, which is what the wire's
	// single "runs" cell carries. Building it here rather than in the module
	// means the rows never leave the database as rows.
	triggerListJSONSQL = `SELECT COALESCE(json_agg(row_to_json(r) ORDER BY r.queued_at DESC),
	                                      '[]')::text
	                        FROM (SELECT id, source, event, task, workspace, pipeline_id,
	                                     status, queued_at, error
	                                FROM trigger_runs
	                               WHERE ($1 = '' OR status = $1)) r`
)

func triggerInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[3] == "" {
		return store.StatusInvalid, nil, nil
	}
	metadata := f[5]
	if metadata == "" {
		metadata = "{}"
	}
	if _, err := q.Exec(ctx, triggerInsertSQL,
		f[0], f[1], f[2], f[3], f[4], metadata); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func triggerStatusSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, triggerStatusSetSQL, f[0], f[1], f[2], f[3])
}

func triggerGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var (
		id, source, event, task, workspace, metadata           string
		pipelineID, status, queued, started, finished, errText string
	)
	err := q.QueryRow(ctx, triggerGetSQL, f[0]).Scan(&id, &source, &event, &task,
		&workspace, &metadata, &pipelineID, &status, &queued, &started, &finished, &errText)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		id, source, event, task, workspace, metadata, pipelineID, status,
		queued, started, finished, errText,
	}, nil
}

func triggerListJSON(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	var document string
	if err := q.QueryRow(ctx, triggerListJSONSQL, f[0]).Scan(&document); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{document}, nil
}
