package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Token accounting: one row per model call, and the aggregates read off it.

// realizedFilter is what counts as spend that actually happened.
//
// An empty usage_kind is treated as realized because rows written before the
// column existed carry none, and excluding them would silently shrink every
// historical total.
const realizedFilter = `(usage_kind = 'realized' OR usage_kind = '')`

// sinceFilter bounds an aggregate to the last N hours, where N <= 0 means "all
// of it".
//
// The C wrote two whole statements per aggregate and picked between them, and
// built the interval by formatting "-%d hours" into a string for
// datetime('now', ?). One statement says the same thing, and make_interval
// takes the number as a number.
const sinceFilter = `($1 <= 0 OR created_at >= now() - make_interval(hours => $1::int))`

// Reply widths, from the catalog. Each is used both by the operation and by the
// contract test, so a row that grows without the wire agreeing is a test
// failure rather than a caller reading a cell that moved.
const (
	tokenAuditGroupedCells   = 5
	tokenAuditDashboardCells = 9
)

const (
	tokenAuditInsertSQL = `INSERT INTO token_audit
	        (session_id, delegation_id, project_name, tool_name, role, model, source,
	         requested_model, stop_reason, usage_kind, agent_log_id,
	         request_id, idempotency_key, attempt, principal, served_model, duration_ms,
	         metadata, prompt_tokens, completion_tokens, cache_write_tokens,
	         cache_read_tokens, estimated_cost_usd)
	    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16,
	            $17, $18, $19, $20, $21, $22, $23)
	    ON CONFLICT DO NOTHING`

	tokenAuditCostForDelegationSQL = `SELECT COALESCE(SUM(estimated_cost_usd), 0)
	                                    FROM token_audit
	                                   WHERE delegation_id = $1 AND ` + realizedFilter

	// The "_ex" form counts every kind, not only realized spend: it answers
	// "what did this delegation cost including what was estimated or avoided",
	// which is a different question from what is billable.
	tokenAuditCostForDelegationExSQL = `SELECT COALESCE(SUM(estimated_cost_usd), 0)
	                                      FROM token_audit WHERE delegation_id = $1`

	// The supervisor/worker split, as ONE row of twelve.
	//
	// The C ran a GROUP BY and pivoted the two rows into a struct in C, filling
	// whichever half it saw. FILTER does the pivot in the statement, which also
	// fixes what the C got by accident: a session with only supervisor turns
	// still answers, with zeros for the worker half, because the aggregate
	// always produces a row.
	tokenAuditSessionSplitSQL = `SELECT
	        COUNT(*)                          FILTER (WHERE delegation_id = ''),
	        COALESCE(SUM(prompt_tokens)::bigint       FILTER (WHERE delegation_id = ''), 0),
	        COALESCE(SUM(completion_tokens)::bigint   FILTER (WHERE delegation_id = ''), 0),
	        COALESCE(SUM(cache_write_tokens)::bigint  FILTER (WHERE delegation_id = ''), 0),
	        COALESCE(SUM(cache_read_tokens)::bigint   FILTER (WHERE delegation_id = ''), 0),
	        COALESCE(SUM(estimated_cost_usd)  FILTER (WHERE delegation_id = ''), 0),
	        COUNT(*)                          FILTER (WHERE delegation_id <> ''),
	        COALESCE(SUM(prompt_tokens)::bigint       FILTER (WHERE delegation_id <> ''), 0),
	        COALESCE(SUM(completion_tokens)::bigint   FILTER (WHERE delegation_id <> ''), 0),
	        COALESCE(SUM(cache_write_tokens)::bigint  FILTER (WHERE delegation_id <> ''), 0),
	        COALESCE(SUM(cache_read_tokens)::bigint   FILTER (WHERE delegation_id <> ''), 0),
	        COALESCE(SUM(estimated_cost_usd)  FILTER (WHERE delegation_id <> ''), 0)
	      FROM token_audit
	     WHERE session_id = $1 AND ` + realizedFilter

	tokenAuditTotalsSQL = `SELECT COUNT(*),
	                              COALESCE(SUM(prompt_tokens)::bigint, 0),
	                              COALESCE(SUM(completion_tokens)::bigint, 0),
	                              COALESCE(SUM(cache_write_tokens)::bigint, 0),
	                              COALESCE(SUM(cache_read_tokens)::bigint, 0),
	                              COALESCE(SUM(estimated_cost_usd), 0)
	                         FROM token_audit
	                        WHERE ` + realizedFilter + ` AND ` + sinceFilter

	// Spend by kind, as one row of five.
	//
	// Realized deliberately absorbs the empty kind AND any kind nobody has
	// defined: an unrecognised usage_kind is counted as money spent, because
	// the alternative is a charge that appears in no column at all. The C made
	// the same choice in an if-else chain.
	//
	// spend is realized: estimated, avoided and partial are reported beside it
	// and never folded in, so "what do we owe" cannot be inflated by a
	// projection.
	tokenAuditSpendBreakdownSQL = `SELECT
	        COALESCE(SUM(estimated_cost_usd) FILTER (
	            WHERE usage_kind NOT IN ('estimated', 'avoided', 'partial')), 0) AS realized,
	        COALESCE(SUM(estimated_cost_usd) FILTER (WHERE usage_kind = 'estimated'), 0),
	        COALESCE(SUM(estimated_cost_usd) FILTER (WHERE usage_kind = 'avoided'), 0),
	        COALESCE(SUM(estimated_cost_usd) FILTER (WHERE usage_kind = 'partial'), 0)
	      FROM token_audit
	     WHERE ` + sinceFilter

	// The dashboard is an AGGREGATE by tool and role, not a list of calls.
	tokenAuditListDashboardSQL = `SELECT tool_name, role,
	                                     COALESCE(SUM(prompt_tokens)::bigint, 0),
	                                     COALESCE(SUM(completion_tokens)::bigint, 0),
	                                     COALESCE(SUM(cache_write_tokens)::bigint, 0),
	                                     COALESCE(SUM(cache_read_tokens)::bigint, 0),
	                                     COALESCE(SUM(estimated_cost_usd), 0),
	                                     COUNT(*),
	                                     COALESCE(to_char(MAX(created_at) AT TIME ZONE 'utc',
	                                              'YYYY-MM-DD HH24:MI:SS'), '')
	                                FROM token_audit
	                               WHERE ` + realizedFilter + `
	                               GROUP BY tool_name, role
	                               ORDER BY COALESCE(SUM(estimated_cost_usd), 0) DESC,
	                                        MAX(created_at) DESC
	                               LIMIT $1`
)

// groupedSpendSQL is the shape of by_role, by_tool, by_model and by_source:
// the same aggregates over a different grouping column.
//
// The column is chosen from a fixed set by the operation rather than taken from
// the wire, so this composes a statement without composing anything a caller
// supplied.
func groupedSpendSQL(column string) string {
	return `SELECT ` + column + `, COUNT(*),
	               COALESCE(SUM(prompt_tokens)::bigint, 0),
	               COALESCE(SUM(completion_tokens)::bigint, 0),
	               COALESCE(SUM(estimated_cost_usd), 0)
	          FROM token_audit
	         WHERE ` + realizedFilter + ` AND ` + sinceFilter + `
	         GROUP BY 1
	         ORDER BY COALESCE(SUM(prompt_tokens)::bigint, 0) +
	                  COALESCE(SUM(completion_tokens)::bigint, 0) DESC, 1
	         LIMIT $2`
}

var (
	tokenAuditByRoleSQL   = groupedSpendSQL("role")
	tokenAuditByToolSQL   = groupedSpendSQL("tool_name")
	tokenAuditByModelSQL  = groupedSpendSQL("model")
	tokenAuditBySourceSQL = groupedSpendSQL("source")
)

// tokenAuditInsert records one model call, and answers with nothing.
//
// The idempotency key makes a repeat a no-op rather than a second charge. The C
// created the unique index that enforces that from a SEPARATE operation the
// caller had to remember to call, so a store nobody had asked accepted the
// duplicate silently. The index is part of the schema here, so the guarantee
// does not depend on anyone remembering -- and the window before the call,
// which is exactly when a retry storm arrives, no longer exists.
func tokenAuditInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	nums := make([]int64, 0, 7)
	for _, at := range []int{10, 13, 16, 18, 19, 20, 21} {
		n, ok := store.Atoi64(f[at])
		if !ok || n < 0 {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	cost, ok := store.Atof(f[22])
	if !ok || cost < 0 {
		return store.StatusInvalid, nil, nil
	}
	usageKind := f[9]
	if usageKind == "" {
		usageKind = "realized"
	}
	if _, err := q.Exec(ctx, tokenAuditInsertSQL,
		f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], usageKind,
		nums[0], f[11], f[12], nums[1], f[14], f[15], nums[2], f[17],
		nums[3], nums[4], nums[5], nums[6], cost); err != nil {
		return store.StatusFailed, nil, err
	}
	// A conflict wrote nothing, and that is success: this exact charge is
	// already in the ledger, which is what the caller wanted.
	return store.StatusOK, nil, nil
}

// tokenAuditEnsureIdemIndex is a no-op. See the note on the insert above: the
// index it used to create is part of the schema now.
func tokenAuditEnsureIdemIndex(_ context.Context, _ store.Queryer, _ []string) (uint32, []string, error) {
	return store.StatusOK, nil, nil
}

// oneCost answers with a single summed cost.
func oneCost(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		if f[0] == "" {
			return store.StatusInvalid, nil, nil
		}
		var cost float64
		if err := q.QueryRow(ctx, sql, f[0]).Scan(&cost); err != nil {
			return store.StatusFailed, nil, err
		}
		return store.StatusOK, []string{store.Ftoa(cost)}, nil
	}
}

// tokenAuditSessionSplit answers with twelve cells: the supervisor's six, then
// the workers'.
func tokenAuditSessionSplit(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var (
		sCalls, sPrompt, sCompletion, sWrite, sRead int64
		wCalls, wPrompt, wCompletion, wWrite, wRead int64
		sCost, wCost                                float64
	)
	if err := q.QueryRow(ctx, tokenAuditSessionSplitSQL, f[0]).Scan(
		&sCalls, &sPrompt, &sCompletion, &sWrite, &sRead, &sCost,
		&wCalls, &wPrompt, &wCompletion, &wWrite, &wRead, &wCost); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(sCalls), store.I64toa(sPrompt), store.I64toa(sCompletion),
		store.I64toa(sWrite), store.I64toa(sRead), store.Ftoa(sCost),
		store.I64toa(wCalls), store.I64toa(wPrompt), store.I64toa(wCompletion),
		store.I64toa(wWrite), store.I64toa(wRead), store.Ftoa(wCost),
	}, nil
}

func tokenAuditTotals(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, ok := store.Atoi(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var (
		calls, prompt, completion, cw, cr int64
		cost                              float64
	)
	if err := q.QueryRow(ctx, tokenAuditTotalsSQL, since).Scan(
		&calls, &prompt, &completion, &cw, &cr, &cost); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(calls), store.I64toa(prompt), store.I64toa(completion),
		store.I64toa(cw), store.I64toa(cr), store.Ftoa(cost),
	}, nil
}

// tokenAuditSpendBreakdown answers with five cells: the four kinds, then what
// is actually owed.
func tokenAuditSpendBreakdown(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, ok := store.Atoi(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var realized, estimated, avoided, partial float64
	if err := q.QueryRow(ctx, tokenAuditSpendBreakdownSQL, since).Scan(
		&realized, &estimated, &avoided, &partial); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.Ftoa(realized), store.Ftoa(estimated),
		store.Ftoa(avoided), store.Ftoa(partial),
		// Billable spend is realized only.
		store.Ftoa(realized),
	}, nil
}

// groupedSpend is the body all four by-column aggregates share.
func groupedSpend(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		since, ok := store.Atoi(f[0])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		max, ok := boundedMax(f[1])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		return collect(ctx, q, sql, tokenAuditGroupedCells,
			func(scan func(...any) error) ([]string, error) {
				var (
					key                       string
					calls, prompt, completion int64
					cost                      float64
				)
				if err := scan(&key, &calls, &prompt, &completion, &cost); err != nil {
					return nil, err
				}
				return []string{
					key, store.I64toa(calls),
					store.I64toa(prompt), store.I64toa(completion),
					store.Ftoa(cost),
				}, nil
			}, since, max)
	}
}

func tokenAuditListDashboard(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, tokenAuditListDashboardSQL, tokenAuditDashboardCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				tool, role, lastSeen          string
				prompt, completion, cw, cr, n int64
				cost                          float64
			)
			if err := scan(&tool, &role, &prompt, &completion, &cw, &cr,
				&cost, &n, &lastSeen); err != nil {
				return nil, err
			}
			return []string{
				tool, role,
				store.I64toa(prompt), store.I64toa(completion),
				store.I64toa(cw), store.I64toa(cr),
				store.Ftoa(cost), store.I64toa(n), lastSeen,
			}, nil
		}, max)
}
