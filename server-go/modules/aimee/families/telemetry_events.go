package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Cost folding, interaction events, guardrail events and evaluation results.

// --- cost folding ------------------------------------------------------------

const (
	// A child's cost folds into its parent exactly once. The UNIQUE constraint
	// is what makes a repeated fold an update rather than a second charge, and
	// the C relied on the same constraint -- so this is the same rule said in
	// the schema instead of hoped for.
	costFoldRecordSQL = `INSERT INTO cost_fold_log
	                         (parent_session_id, child_session_id, cost_usd, cost_source)
	                     VALUES ($1, $2, $3, $4)
	                     ON CONFLICT (parent_session_id, child_session_id) DO UPDATE
	                        SET cost_usd = EXCLUDED.cost_usd,
	                            cost_source = EXCLUDED.cost_source`

	costFoldTotalSQL = `SELECT COALESCE(SUM(cost_usd), 0)
	                      FROM cost_fold_log WHERE parent_session_id = $1`
)

func costFoldRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	parent, child := f[0], f[1]
	if parent == "" || child == "" {
		return store.StatusInvalid, nil, nil
	}
	// A session folding into itself would double every total that reads it.
	// The schema refuses it too; refusing here names the mistake rather than
	// surfacing a constraint violation.
	if parent == child {
		return store.StatusInvalid, nil, nil
	}
	cost, ok := store.Atof(f[2])
	if !ok || cost < 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, costFoldRecordSQL, parent, child, cost, f[3])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	// One cell: whether the fold landed. A repeat updates the same row, so this
	// is 1 either way -- which is the point of the uniqueness constraint.
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func costFoldTotal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var total float64
	if err := q.QueryRow(ctx, costFoldTotalSQL, f[0]).Scan(&total); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Ftoa(total)}, nil
}

// --- interaction events -------------------------------------------------------

// The reply carries seven cells. reflected_at and promoted_at are not among
// them: they are how the feeds SELECT, not something a reader of an event is
// told. The order here is the wire's, which is not the order the C's SELECT
// used -- it went through a struct on the way out.
const interactionColumns = `id,
                            to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
                            session_id, event_type, actor, payload, outcome`

const (
	interactionRecordSQL = `INSERT INTO interaction_events
	                            (session_id, event_type, actor, payload, outcome)
	                        VALUES ($1, $2, $3, $4, $5) RETURNING id`

	// Scoped to a session: the caller reflects on one conversation, not on
	// everything the store has ever seen.
	interactionUnreflectedSQL = `SELECT ` + interactionColumns + `
	                               FROM interaction_events
	                              WHERE session_id = $1 AND reflected_at IS NULL
	                              ORDER BY created_at ASC, id ASC
	                              LIMIT $2`

	interactionForSessionSQL = `SELECT ` + interactionColumns + `
	                              FROM interaction_events
	                             WHERE session_id = $1
	                             ORDER BY id DESC LIMIT $2`

	interactionPromotionFeedSQL = `SELECT ` + interactionColumns + `
	                                 FROM interaction_events
	                                WHERE reflected_at IS NOT NULL AND promoted_at IS NULL
	                                ORDER BY id LIMIT $1`

	// One statement for the whole batch.
	//
	// The C prepared an UPDATE and stepped it once per id, so marking 500
	// events was 500 round trips -- and it stopped at the first failure with
	// the earlier ones already written and no way to say which.
	interactionMarkReflectedSQL = `UPDATE interaction_events
	                                  SET reflected_at = now()
	                                WHERE id = ANY ($1::bigint[])
	                                  AND reflected_at IS NULL`

	// Promotion follows reflection: an event is only promoted out of a feed it
	// was reflected into. The C would happily stamp promoted_at on an event
	// that had never been reflected, which the schema now refuses outright.
	interactionMarkPromotedSQL = `UPDATE interaction_events
	                                 SET promoted_at = now()
	                               WHERE id = ANY ($1::bigint[])
	                                 AND reflected_at IS NOT NULL
	                                 AND promoted_at IS NULL`

	// Keep the newest `max` events and drop the rest.
	interactionEvictSQL = `DELETE FROM interaction_events
	                        WHERE id NOT IN (SELECT id FROM interaction_events
	                                          ORDER BY id DESC LIMIT $1)`
)

// interactionTableCap is how many events the table keeps. Recording one trims
// to this, which is why recording answers with a count of what it evicted.
const interactionTableCap = 50000

func interactionRow(scan func(...any) error) ([]string, error) {
	var (
		id                                              int64
		createdAt, sessionID, eventType, actor, payload string
		outcome                                         string
	)
	if err := scan(&id, &createdAt, &sessionID, &eventType, &actor, &payload,
		&outcome); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), createdAt, sessionID, eventType, actor, payload, outcome,
	}, nil
}

func interactionEventRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	actor := f[2]
	if actor == "" {
		actor = "agent"
	}
	payload := f[3]
	if payload == "" {
		payload = "{}"
	}
	outcome := f[4]
	if outcome == "" {
		outcome = "ok"
	}
	var id int64
	if err := q.QueryRow(ctx, interactionRecordSQL,
		f[0], f[1], actor, payload, outcome).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	// Recording trims the table, and the reply is how many events that dropped.
	// The two are one transaction here; in the C the insert committed and the
	// trim was a separate statement that could fail on its own, leaving the
	// caller told nothing was evicted while the table stayed over its cap.
	tag, err := q.Exec(ctx, interactionEvictSQL, interactionTableCap)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func interactionEventListUnreflected(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, interactionUnreflectedSQL, 7, interactionRow, f[0], max)
}

func interactionEventListForSession(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, interactionForSessionSQL, 7, interactionRow, f[0], max)
}

func interactionEventListPromotionFeed(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, interactionPromotionFeedSQL, 7, interactionRow, max)
}

// interactionIDs reads a variadic id list off the wire.
//
// The wire carries up to 512 ids and no leading field. An id that is not a
// number is a malformed request rather than one to skip: skipping it would
// mark a different set than the caller asked for and report success.
func interactionIDs(f []string) ([]int64, bool) {
	if len(f) > 512 {
		return nil, false
	}
	ids := make([]int64, 0, len(f))
	for _, cell := range f {
		id, ok := store.Atoi64(cell)
		if !ok || id <= 0 {
			return nil, false
		}
		ids = append(ids, id)
	}
	return ids, true
}

// markInteraction is the body both marking operations share.
func markInteraction(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		ids, ok := interactionIDs(f)
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		if len(ids) == 0 {
			// Marking nothing is not a failure; it is what was asked.
			return store.StatusOK, []string{"0"}, nil
		}
		tag, err := q.Exec(ctx, sql, ids)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
	}
}

func interactionEventEvictIfNeeded(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	keep, ok := store.Atoi64(f[0])
	if !ok || keep <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, interactionEvictSQL, keep); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- guardrail events ----------------------------------------------------------

// Nine cells: the risks a reader acts on, not every score the guardrail
// computed. The detail columns stay in the table for anyone querying it
// directly.
const guardrailEventColumns = `id,
                               to_char(recorded_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                               session_id, tool_name, overall_risk, labels,
                               final_action, explanation, dry_run`

const guardrailEventCells = 9

const (
	guardrailEventInsertSQL = `INSERT INTO guardrail_events
	        (session_id, tool_name, overall_risk, action_risk, diff_risk, drift_risk,
	         antipattern_similarity, recommendation, labels, final_action, explanation, dry_run)
	    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)`

	// Four named counts, not a grouping: how many the guardrail only observed,
	// then how many it actually warned, prompted or blocked on. An event that
	// was a dry run is counted ONLY in the first cell however it was labelled,
	// because a guardrail that did not act did not warn anyone.
	guardrailEventCounts7dSQL = `SELECT
	        COUNT(*) FILTER (WHERE dry_run),
	        COUNT(*) FILTER (WHERE NOT dry_run AND final_action = 'warn'),
	        COUNT(*) FILTER (WHERE NOT dry_run AND final_action = 'prompt'),
	        COUNT(*) FILTER (WHERE NOT dry_run AND final_action = 'block')
	      FROM guardrail_events
	     WHERE recorded_at >= now() - interval '7 days'`

	// An advisory is an event the guardrail did NOT act on. dry_run is what
	// says so, which is why it is a boolean rather than the C's integer: a
	// count that reads "not 0" would include any value someone wrote by
	// mistake.
	guardrailEventSessionAdvisorySQL = `SELECT COUNT(*) FROM guardrail_events
	                                     WHERE session_id = $1 AND dry_run`

	// only_advisory keeps the events the guardrail had something to say about.
	// The C wrote two statements and chose between them; one says it once.
	guardrailEventListSQL = `SELECT ` + guardrailEventColumns + `
	                           FROM guardrail_events
	                          WHERE (NOT $2 OR final_action IN ('warn', 'prompt', 'block'))
	                          ORDER BY recorded_at DESC, id DESC
	                          LIMIT $1`
)

func guardrailEventRow(scan func(...any) error) ([]string, error) {
	var (
		id                               int64
		recordedAt, sessionID, toolName  string
		overall                          float64
		labels, finalAction, explanation string
		dryRun                           bool
	)
	if err := scan(&id, &recordedAt, &sessionID, &toolName, &overall,
		&labels, &finalAction, &explanation, &dryRun); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), recordedAt, sessionID, toolName,
		store.Ftoa(overall), labels, finalAction, explanation,
		store.Btoa(dryRun),
	}, nil
}

func guardrailEventInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	risks := make([]float64, 0, 5)
	for _, at := range []int{2, 3, 4, 5, 6} {
		v, ok := store.Atof(f[at])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		risks = append(risks, v)
	}
	dryRun, ok := store.Atob(f[11])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, guardrailEventInsertSQL,
		f[0], f[1], risks[0], risks[1], risks[2], risks[3], risks[4],
		f[7], f[8], f[9], f[10], dryRun); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func guardrailEventCounts7d(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var dryRun, warn, prompt, block int64
	if err := q.QueryRow(ctx, guardrailEventCounts7dSQL).Scan(
		&dryRun, &warn, &prompt, &block); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(dryRun), store.I64toa(warn),
		store.I64toa(prompt), store.I64toa(block),
	}, nil
}

func guardrailEventSessionAdvisoryCount(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var count int64
	if err := q.QueryRow(ctx, guardrailEventSessionAdvisorySQL, f[0]).Scan(&count); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(count)}, nil
}

func guardrailEventList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	onlyAdvisory, ok := store.Atob(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, guardrailEventListSQL, guardrailEventCells,
		guardrailEventRow, max, onlyAdvisory)
}

// --- evaluation results ---------------------------------------------------------

// Eleven cells: what a run did, not the whole row. The response text, the
// hashes and the hardware profile stay in the table for anyone querying it
// directly -- a list of results is read to see outcomes, and carrying every
// response body would make it enormous.
const evalColumns = `suite, task_name, agent_name, ablation, success, turns,
                     tool_calls, tool_call_failures, rescue_recoveries, latency_ms,
                     to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const evalCells = 11

const (
	evalInsertSQL = `INSERT INTO eval_results
	        (suite, task_name, agent_name, ablation, success, turns, tool_calls,
	         tool_call_failures, rescue_recoveries, prompt_tokens, completion_tokens,
	         latency_ms, response, error, dataset_hash, target_hash, harness_version,
	         hardware_profile, seed)
	    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16,
	            $17, $18, $19)`

	// The tasks that had a failing run in the last week, and what the failure
	// said.
	//
	// The C wrote "SELECT DISTINCT task_name, error ... ORDER BY created_at
	// DESC", which orders by a column that is not in the DISTINCT list. SQLite
	// accepts that and picks a row arbitrarily, so "most recent first" was
	// whatever the query plan happened to produce. PostgreSQL rejects it
	// outright, which forces the question the C never answered: WHICH failure's
	// timestamp orders a task that failed more than once.
	//
	// DISTINCT ON answers it -- the most recent occurrence of each distinct
	// (task, error) -- and the outer ORDER BY then means what it says.
	evalFailedTasksSQL = `SELECT task_name, error
	                        FROM (SELECT DISTINCT ON (task_name, error)
	                                     task_name, error, created_at
	                                FROM eval_results
	                               WHERE NOT success
	                                 AND created_at > now() - interval '7 days'
	                               ORDER BY task_name, error, created_at DESC) recent
	                       ORDER BY created_at DESC, task_name
	                       LIMIT $1`

	// The tasks that had a passing run in the last week. Same correction as
	// above; this one carries no error to distinguish on.
	evalPassedTasksSQL = `SELECT task_name
	                        FROM (SELECT DISTINCT ON (task_name) task_name, created_at
	                                FROM eval_results
	                               WHERE success
	                                 AND created_at > now() - interval '7 days'
	                               ORDER BY task_name, created_at DESC) recent
	                       ORDER BY created_at DESC, task_name
	                       LIMIT $1`

	evalListSQL = `SELECT ` + evalColumns + `
	                 FROM eval_results
	                WHERE ($1 = '' OR suite = $1)
	                ORDER BY id DESC
	                LIMIT $2`
)

func evalRow(scan func(...any) error) ([]string, error) {
	var (
		turns, toolCalls, failures, rescues, latency int64
		suite, task, agent, ablation, stamp          string
		success                                      bool
	)
	if err := scan(&suite, &task, &agent, &ablation, &success, &turns,
		&toolCalls, &failures, &rescues, &latency, &stamp); err != nil {
		return nil, err
	}
	return []string{
		suite, task, agent, ablation, store.Btoa(success),
		store.I64toa(turns), store.I64toa(toolCalls),
		store.I64toa(failures), store.I64toa(rescues),
		store.I64toa(latency), stamp,
	}, nil
}

func evalResultInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	success, ok := store.Atob(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	nums := make([]int64, 0, 8)
	for _, at := range []int{5, 6, 7, 8, 9, 10, 11, 18} {
		n, ok := store.Atoi64(f[at])
		if !ok || n < 0 {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	// A run cannot have failed more tool calls than it made. The schema
	// refuses it; refusing here names it rather than surfacing a constraint.
	if nums[2] > nums[1] {
		return store.StatusInvalid, nil, nil
	}
	harness := f[16]
	if harness == "" {
		harness = "1"
	}
	ablation := f[3]
	if ablation == "" {
		ablation = "full"
	}

	if _, err := q.Exec(ctx, evalInsertSQL,
		f[0], f[1], f[2], ablation, success,
		nums[0], nums[1], nums[2], nums[3], nums[4], nums[5], nums[6],
		f[12], f[13], f[14], f[15], harness, f[17], nums[7]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// evalFailedTasksRecent answers with each recently-failing task and its error.
func evalFailedTasksRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, evalFailedTasksSQL, 2, func(scan func(...any) error) ([]string, error) {
		var task, errText string
		if err := scan(&task, &errText); err != nil {
			return nil, err
		}
		return []string{task, errText}, nil
	}, max)
}

// evalPassedTasksRecent answers with each recently-passing task.
//
// A task can appear in both lists: a task that failed on Monday and passed on
// Tuesday had both a failing and a passing run this week, and both are true.
// That is the C's meaning and it is the useful one -- "what has been failing
// lately" is a different question from "what is failing now".
func evalPassedTasksRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, evalPassedTasksSQL, 1, func(scan func(...any) error) ([]string, error) {
		var task string
		if err := scan(&task); err != nil {
			return nil, err
		}
		return []string{task}, nil
	}, max)
}

func evalResultsList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, evalListSQL, evalCells, evalRow, f[0], max)
}
