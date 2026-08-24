package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Tool events and the chains that summarise runs of them: a session records
// each tool call, and once a run of calls is worth collapsing it becomes a
// chain with a short stub standing in for the raw results.

// chain_id is stored as NULL when an event belongs to no chain and rendered as
// the wire's 0. The mapping lives here, in the column list every read shares, so
// no individual read has to remember it.
const convEventColumns = `id, session_id, tool_name, tool_input, tool_result,
                          result_bytes, COALESCE(chain_id, 0),
                          to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const convChainColumns = `id, session_id, event_id_first, event_id_last, tools, stub,
                          raw_bytes, stub_bytes, state,
                          to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	convRecordEventSQL = `INSERT INTO conv_tool_events
	                          (session_id, tool_name, tool_input, tool_result, result_bytes)
	                      VALUES ($1, $2, $3, $4, $5) RETURNING id`

	// The C matched an id range and chain_id=0 with NO session filter, so a
	// chain could claim events belonging to another session whenever the id
	// range overlapped -- ids are global, and nothing in the statement tied the
	// range to the chain's own session.
	//
	// The wire carries no session, but it does not need to: the chain knows
	// which session it belongs to, so the scope is recoverable from the chain
	// id already being passed.
	convSetChainIDSQL = `UPDATE conv_tool_events
	                        SET chain_id = $1
	                      WHERE id BETWEEN $2 AND $3
	                        AND chain_id IS NULL
	                        AND session_id = (SELECT session_id
	                                            FROM conv_tool_chains WHERE id = $1)`

	convInsertChainSQL = `INSERT INTO conv_tool_chains
	                          (session_id, event_id_first, event_id_last,
	                           tools, stub, raw_bytes, stub_bytes)
	                      VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING id`

	convPendingEventsSQL = `SELECT ` + convEventColumns + `
	                          FROM conv_tool_events
	                         WHERE session_id = $1 AND chain_id IS NULL
	                         ORDER BY id LIMIT $2`

	convListChainsSQL = `SELECT ` + convChainColumns + `
	                       FROM conv_tool_chains
	                      WHERE session_id = $1
	                      ORDER BY id DESC LIMIT $2`

	convChainEventsSQL = `SELECT ` + convEventColumns + `
	                        FROM conv_tool_events
	                       WHERE chain_id = $1
	                       ORDER BY id LIMIT $2`

	// A literal substring search, case-insensitively.
	//
	// SQLite's instr(lower(stub), lower(?)) > 0 is a plain substring test with
	// no pattern language, so strpos is the exact equivalent and -- unlike the
	// LIKE searches elsewhere in this family -- there is nothing to escape. A
	// caller searching for "50%" here means the three characters, and both
	// forms agree on that.
	convSearchChainsSQL = `SELECT ` + convChainColumns + `
	                         FROM conv_tool_chains
	                        WHERE session_id = $1
	                          AND strpos(lower(stub), lower($2)) > 0
	                        ORDER BY id DESC LIMIT $3`

	convStateGetSQL = `SELECT last_event_id, chain_count, event_count
	                     FROM conv_context_state WHERE session_id = $1`

	convStateUpdateSQL = `INSERT INTO conv_context_state
	                          (session_id, last_event_id, chain_count, event_count, updated_at)
	                      VALUES ($1, $2, $3, $4, now())
	                      ON CONFLICT (session_id) DO UPDATE SET
	                          last_event_id = EXCLUDED.last_event_id,
	                          chain_count   = EXCLUDED.chain_count,
	                          event_count   = EXCLUDED.event_count,
	                          updated_at    = now()`
)

func convEventRow(scan func(...any) error) ([]string, error) {
	var (
		id, resultBytes, chainID                          int64
		sessionID, toolName, toolInput, toolResult, stamp string
	)
	if err := scan(&id, &sessionID, &toolName, &toolInput, &toolResult,
		&resultBytes, &chainID, &stamp); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), sessionID, toolName, toolInput, toolResult,
		store.I64toa(resultBytes), store.I64toa(chainID), stamp,
	}, nil
}

func convChainRow(scan func(...any) error) ([]string, error) {
	var (
		id, first, last, rawBytes, stubBytes int64
		sessionID, tools, stub, state, stamp string
	)
	if err := scan(&id, &sessionID, &first, &last, &tools, &stub,
		&rawBytes, &stubBytes, &state, &stamp); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), sessionID,
		store.I64toa(first), store.I64toa(last),
		tools, stub,
		store.I64toa(rawBytes), store.I64toa(stubBytes),
		state, stamp,
	}, nil
}

func convRecordEvent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	resultBytes, ok := store.Atoi64(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, convRecordEventSQL,
		f[0], f[1], f[2], f[3], resultBytes).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

// convSetChainID claims a run of a session's unassigned events for a chain.
func convSetChainID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	chainID, ok := store.Atoi64(f[0])
	if !ok || chainID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	first, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	last, ok := store.Atoi64(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// An inverted range claims nothing rather than quietly matching nothing,
	// because a caller that computed it backwards has a bug either way and the
	// silent version is the one that is hard to find.
	if last < first {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, convSetChainIDSQL, chainID, first, last)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func convInsertChain(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	nums := make([]int64, 0, 4)
	for _, at := range []int{1, 2, 5, 6} {
		n, ok := store.Atoi64(f[at])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	var id int64
	if err := q.QueryRow(ctx, convInsertChainSQL,
		f[0], nums[0], nums[1], f[3], f[4], nums[2], nums[3]).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func convPendingEvents(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, convPendingEventsSQL, 8, convEventRow, f[0], max)
}

func convListChains(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, convListChainsSQL, 10, convChainRow, f[0], max)
}

func convChainEvents(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	chainID, ok := store.Atoi64(f[0])
	if !ok || chainID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, convChainEventsSQL, 8, convEventRow, chainID, max)
}

func convSearchChains(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[2])
	if f[0] == "" || f[1] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, convSearchChainsSQL, 10, convChainRow, f[0], f[1], max)
}

func convStateGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var lastEventID, chainCount, eventCount int64
	err := q.QueryRow(ctx, convStateGetSQL, f[0]).Scan(&lastEventID, &chainCount, &eventCount)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(lastEventID),
		store.I64toa(chainCount),
		store.I64toa(eventCount),
	}, nil
}

func convStateUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	nums := make([]int64, 0, 3)
	for _, at := range []int{1, 2, 3} {
		n, ok := store.Atoi64(f[at])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	if _, err := q.Exec(ctx, convStateUpdateSQL, f[0], nums[0], nums[1], nums[2]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
