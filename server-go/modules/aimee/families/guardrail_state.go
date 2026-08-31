package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Guardrail session state: one parent row per session plus six child collections.
const (
	EventGuardrailState uint32 = 11785
	StageGuardrailState uint32 = 9

	opSessionStateLoad        uint32 = 1
	opSessionStateSave        uint32 = 2
	opSessionStateDelete      uint32 = 3
	opSessionStateExists      uint32 = 4
	opSessionStateList        uint32 = 5
	opSessionStateGetSummary  uint32 = 6
	opSessionStateListExpired uint32 = 7
)

// The collection capacities, from src/modules/guardrails/guardrails.h.
//
// They are part of the WIRE, not just of the C's storage: every slot is a field
// whether or not it is used, so the frame is a fixed 387 fields and these
// numbers are what make that arithmetic come out.
const (
	maxSeenPaths  = 64
	maxWorktrees  = 16
	maxTDDWrites  = 8
	maxReadPaths  = 64
	maxFileHashes = 64
	maxAPHits     = 32
)

// Request field offsets. The save frame is sid followed by every collection and
// scalar in catalog order; the load REPLY is the same list without sid, so a
// reply offset is a request offset minus one.
const (
	offSid           = 0
	offSeenPaths     = 1
	offSeenCount     = offSeenPaths + maxSeenPaths // 65
	offSessionMode   = offSeenCount + 1            // 66
	offGuardrailMode = offSessionMode + 1          // 67
	offActiveTaskID  = offGuardrailMode + 1        // 68
	offHookCalls     = offActiveTaskID + 1         // 69
	offDirty         = offHookCalls + 1            // 70
	offWorktrees     = offDirty + 1                // 71
	offWorktreeCnt   = offWorktrees + maxWorktrees*2
	offIsDelegate    = offWorktreeCnt + 1
	offOrchEdits     = offIsDelegate + 1
	offOrchNudge     = offOrchEdits + 1
	offSkillSymbols  = offOrchNudge + 1
	offSkillWaiting  = offSkillSymbols + 1
	offSkillTDD      = offSkillWaiting + 1
	offTDDMode       = offSkillTDD + 1
	offTDDWrites     = offTDDMode + 1
	offTDDWriteCnt   = offTDDWrites + maxTDDWrites*2
	offReadPaths     = offTDDWriteCnt + 1
	offReadPathCnt   = offReadPaths + maxReadPaths
	offFileHashes    = offReadPathCnt + 1
	offFileHashCnt   = offFileHashes + maxFileHashes*2
	offAPHits        = offFileHashCnt + 1
	offAPHitCnt      = offAPHits + maxAPHits*2

	saveFields = offAPHitCnt + 1 // 387
	loadFields = saveFields - 1  // 386: the reply carries no sid
)

// sessionStateListMax bounds the two list operations.
const sessionStateListMax = 256

// Defaults the C applied when a field arrived empty. They are here rather than
// as column defaults because the module writes every column on every save, so a
// column default would never fire.
const (
	defaultSessionMode   = "implement"
	defaultGuardrailMode = "approve"
	defaultTDDMode       = "off"
)

const (
	sessionStateUpsertSQL = `INSERT INTO session_state (
	        session_id, session_mode, guardrail_mode, tdd_mode, active_task_id,
	        hook_call_count, orch_direct_edits, orch_nudge_sent,
	        skill_find_symbols_advisory_sent, skill_condition_waiting_advisory_sent,
	        skill_tdd_advisory_sent, updated_at)
	    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, now())
	    ON CONFLICT (session_id) DO UPDATE SET
	        session_mode = EXCLUDED.session_mode,
	        guardrail_mode = EXCLUDED.guardrail_mode,
	        tdd_mode = EXCLUDED.tdd_mode,
	        active_task_id = EXCLUDED.active_task_id,
	        hook_call_count = EXCLUDED.hook_call_count,
	        orch_direct_edits = EXCLUDED.orch_direct_edits,
	        orch_nudge_sent = EXCLUDED.orch_nudge_sent,
	        skill_find_symbols_advisory_sent = EXCLUDED.skill_find_symbols_advisory_sent,
	        skill_condition_waiting_advisory_sent = EXCLUDED.skill_condition_waiting_advisory_sent,
	        skill_tdd_advisory_sent = EXCLUDED.skill_tdd_advisory_sent,
	        updated_at = now()`

	sessionStateScalarSQL = `SELECT session_mode, guardrail_mode, tdd_mode, active_task_id,
	                                hook_call_count, orch_direct_edits, orch_nudge_sent,
	                                skill_find_symbols_advisory_sent,
	                                skill_condition_waiting_advisory_sent,
	                                skill_tdd_advisory_sent
	                           FROM session_state WHERE session_id = $1`

	sessionStateDeleteSQL = `DELETE FROM session_state WHERE session_id = $1`

	sessionStateExistsSQL = `SELECT 1 FROM session_state WHERE session_id = $1`

	sessionSummarySQL = `SELECT session_id,
	                            to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	                            hook_call_count
	                       FROM session_state WHERE session_id = $1`

	sessionListSQL = `SELECT session_id,
	                         to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	                         hook_call_count
	                    FROM session_state
	                   ORDER BY updated_at DESC, session_id
	                   LIMIT $1`

	// Expiry is measured against the DATABASE's clock, as it was before. The
	// caller supplies an age, not an instant, so a caller whose clock has
	// drifted cannot expire sessions early.
	sessionListExpiredSQL = `SELECT session_id FROM session_state
	                          WHERE updated_at < now() - make_interval(secs => $1)
	                          ORDER BY updated_at, session_id
	                          LIMIT $2`
)

// childTable describes one collection: how to clear it and how to refill it.
type childTable struct {
	clear  string
	insert string
}

var sessionChildren = []childTable{
	{`DELETE FROM session_state_seen_paths WHERE session_id = $1`,
		`INSERT INTO session_state_seen_paths (session_id, seq, path) VALUES ($1, $2, $3)`},
	{`DELETE FROM session_state_read_paths WHERE session_id = $1`,
		`INSERT INTO session_state_read_paths (session_id, seq, path) VALUES ($1, $2, $3)`},
	{`DELETE FROM session_state_worktrees WHERE session_id = $1`,
		`INSERT INTO session_state_worktrees (session_id, seq, git_root, worktree_path) VALUES ($1, $2, $3, $4)`},
	{`DELETE FROM session_state_tdd_writes WHERE session_id = $1`,
		`INSERT INTO session_state_tdd_writes (session_id, seq, stem, is_test) VALUES ($1, $2, $3, $4)`},
	{`DELETE FROM session_state_ap_hits WHERE session_id = $1`,
		`INSERT INTO session_state_ap_hits (session_id, pattern_id, hits) VALUES ($1, $2, $3)`},
	{`DELETE FROM session_state_file_hashes WHERE session_id = $1`,
		`INSERT INTO session_state_file_hashes (session_id, path, content_hash) VALUES ($1, $2, $3)`},
}

const (
	childSeenPaths = iota
	childReadPaths
	childWorktrees
	childTDDWrites
	childAPHits
	childFileHashes
)

// hashToStored and hashFromStored move a uint64 through a signed BIGINT.
//
// The wire carries content_hash unsigned (the C prints %llu and parses with
// strtoull), so values above 2^63-1 occur. BIGINT cannot hold that magnitude,
// but it holds the BITS: the conversion is exact in both directions, and the
// value is an FNV-1a hash that is only ever compared for equality.
func hashToStored(v uint64) int64   { return int64(v) }
func hashFromStored(v int64) uint64 { return uint64(v) }

// countIn reads a collection's count field and clamps it to the wire's capacity.
// A count larger than the slots that exist would otherwise read past them.
func countIn(fields []string, at, capacity int) int {
	n, ok := store.Atoi(fields[at])
	if !ok || n < 0 {
		return 0
	}
	if n > capacity {
		return capacity
	}
	return n
}

func orDefault(value, fallback string) string {
	if value == "" {
		return fallback
	}
	return value
}

// sessionStateSave is op 2: replace this session's whole state.
//
// The child collections are a SNAPSHOT, not a log, so each is cleared and
// refilled rather than merged. That is what the C did, and it is why the whole
// operation is one transaction: a session observed between the clear and the
// refill would look like it had forgotten everything.
func sessionStateSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sid := f[offSid]
	if sid == "" {
		return store.StatusInvalid, nil, nil
	}
	activeTaskID, okTask := store.Atoi64(f[offActiveTaskID])
	hookCalls, okHooks := store.Atoi64(f[offHookCalls])
	orchEdits, okEdits := store.Atoi64(f[offOrchEdits])
	orchNudge, okNudge := store.Atoi64(f[offOrchNudge])
	skillSymbols, okSymbols := store.Atoi64(f[offSkillSymbols])
	skillWaiting, okWaiting := store.Atoi64(f[offSkillWaiting])
	skillTDD, okTDD := store.Atoi64(f[offSkillTDD])
	if !okTask || !okHooks || !okEdits || !okNudge || !okSymbols || !okWaiting || !okTDD {
		return store.StatusInvalid, nil, nil
	}

	if _, err := q.Exec(ctx, sessionStateUpsertSQL, sid,
		orDefault(f[offSessionMode], defaultSessionMode),
		orDefault(f[offGuardrailMode], defaultGuardrailMode),
		orDefault(f[offTDDMode], defaultTDDMode),
		activeTaskID, hookCalls, orchEdits, orchNudge,
		skillSymbols, skillWaiting, skillTDD); err != nil {
		return 0, nil, err
	}

	for _, child := range sessionChildren {
		if _, err := q.Exec(ctx, child.clear, sid); err != nil {
			return 0, nil, err
		}
	}

	for i := 0; i < countIn(f, offSeenCount, maxSeenPaths); i++ {
		if _, err := q.Exec(ctx, sessionChildren[childSeenPaths].insert,
			sid, i, f[offSeenPaths+i]); err != nil {
			return 0, nil, err
		}
	}
	for i := 0; i < countIn(f, offReadPathCnt, maxReadPaths); i++ {
		if _, err := q.Exec(ctx, sessionChildren[childReadPaths].insert,
			sid, i, f[offReadPaths+i]); err != nil {
			return 0, nil, err
		}
	}
	for i := 0; i < countIn(f, offWorktreeCnt, maxWorktrees); i++ {
		if _, err := q.Exec(ctx, sessionChildren[childWorktrees].insert,
			sid, i, f[offWorktrees+i*2], f[offWorktrees+i*2+1]); err != nil {
			return 0, nil, err
		}
	}
	for i := 0; i < countIn(f, offTDDWriteCnt, maxTDDWrites); i++ {
		isTest, ok := store.Atoi(f[offTDDWrites+i*2+1])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		if _, err := q.Exec(ctx, sessionChildren[childTDDWrites].insert,
			sid, i, f[offTDDWrites+i*2], isTest != 0); err != nil {
			return 0, nil, err
		}
	}
	for i := 0; i < countIn(f, offAPHitCnt, maxAPHits); i++ {
		patternID, okID := store.Atoi64(f[offAPHits+i*2])
		hits, okHits := store.Atoi64(f[offAPHits+i*2+1])
		if !okID || !okHits {
			return store.StatusInvalid, nil, nil
		}
		if _, err := q.Exec(ctx, sessionChildren[childAPHits].insert,
			sid, patternID, hits); err != nil {
			return 0, nil, err
		}
	}
	for i := 0; i < countIn(f, offFileHashCnt, maxFileHashes); i++ {
		hash, ok := store.Atou64(f[offFileHashes+i*2+1])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		if _, err := q.Exec(ctx, sessionChildren[childFileHashes].insert,
			sid, f[offFileHashes+i*2], hashToStored(hash)); err != nil {
			return 0, nil, err
		}
	}
	return store.StatusOK, nil, nil
}

// readCollection fills reply slots from a child table.
//
// It returns how many rows it placed so the caller can write the count field.
// Rows beyond the wire's capacity are DROPPED rather than overflowing: the
// frame has a fixed number of slots, and a store that somehow held more than
// fits is a store to report truncated, not one to write past the end of.
func readCollection(ctx context.Context, q store.Queryer, sql, sid string,
	capacity int, place func(i int, scan func(...any) error) error) (int, error) {
	rows, err := q.Query(ctx, sql, sid)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	n := 0
	for rows.Next() {
		if n >= capacity {
			break
		}
		if err := place(n, rows.Scan); err != nil {
			return 0, err
		}
		n++
	}
	if err := rows.Err(); err != nil {
		return 0, err
	}
	return n, nil
}

// sessionStateLoad is op 1: the whole state, or MISSING when the session has none.
func sessionStateLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sid := f[0]
	if sid == "" {
		return store.StatusInvalid, nil, nil
	}

	reply := make([]string, loadFields)
	for i := range reply {
		reply[i] = ""
	}
	// Every count starts at zero, and every unused slot stays empty: the frame
	// is fixed-width, so an unfilled slot is not absent, it is blank.
	zero := []int{offSeenCount, offWorktreeCnt, offTDDWriteCnt, offReadPathCnt,
		offFileHashCnt, offAPHitCnt, offDirty, offIsDelegate}
	for _, at := range zero {
		reply[at-1] = "0"
	}

	var (
		sessionMode, guardrailMode, tddMode  string
		activeTaskID, hookCalls              int64
		orchEdits, orchNudge                 int64
		skillSymbols, skillWaiting, skillTDD int64
	)
	err := q.QueryRow(ctx, sessionStateScalarSQL, sid).Scan(
		&sessionMode, &guardrailMode, &tddMode, &activeTaskID, &hookCalls,
		&orchEdits, &orchNudge, &skillSymbols, &skillWaiting, &skillTDD)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}

	reply[offSessionMode-1] = sessionMode
	reply[offGuardrailMode-1] = guardrailMode
	reply[offTDDMode-1] = tddMode
	reply[offActiveTaskID-1] = store.I64toa(activeTaskID)
	reply[offHookCalls-1] = store.I64toa(hookCalls)
	reply[offOrchEdits-1] = store.I64toa(orchEdits)
	reply[offOrchNudge-1] = store.I64toa(orchNudge)
	reply[offSkillSymbols-1] = store.I64toa(skillSymbols)
	reply[offSkillWaiting-1] = store.I64toa(skillWaiting)
	reply[offSkillTDD-1] = store.I64toa(skillTDD)

	n, err := readCollection(ctx, q,
		`SELECT path FROM session_state_seen_paths WHERE session_id = $1 ORDER BY seq`,
		sid, maxSeenPaths, func(i int, scan func(...any) error) error {
			return scan(&reply[offSeenPaths-1+i])
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offSeenCount-1] = store.Itoa(n)

	n, err = readCollection(ctx, q,
		`SELECT path FROM session_state_read_paths WHERE session_id = $1 ORDER BY seq`,
		sid, maxReadPaths, func(i int, scan func(...any) error) error {
			return scan(&reply[offReadPaths-1+i])
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offReadPathCnt-1] = store.Itoa(n)

	n, err = readCollection(ctx, q,
		`SELECT git_root, worktree_path FROM session_state_worktrees WHERE session_id = $1 ORDER BY seq`,
		sid, maxWorktrees, func(i int, scan func(...any) error) error {
			return scan(&reply[offWorktrees-1+i*2], &reply[offWorktrees-1+i*2+1])
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offWorktreeCnt-1] = store.Itoa(n)

	n, err = readCollection(ctx, q,
		`SELECT stem, is_test FROM session_state_tdd_writes WHERE session_id = $1 ORDER BY seq`,
		sid, maxTDDWrites, func(i int, scan func(...any) error) error {
			var isTest bool
			if err := scan(&reply[offTDDWrites-1+i*2], &isTest); err != nil {
				return err
			}
			reply[offTDDWrites-1+i*2+1] = store.Btoa(isTest)
			return nil
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offTDDWriteCnt-1] = store.Itoa(n)

	n, err = readCollection(ctx, q,
		`SELECT pattern_id, hits FROM session_state_ap_hits WHERE session_id = $1 ORDER BY pattern_id`,
		sid, maxAPHits, func(i int, scan func(...any) error) error {
			var patternID, hits int64
			if err := scan(&patternID, &hits); err != nil {
				return err
			}
			reply[offAPHits-1+i*2] = store.I64toa(patternID)
			reply[offAPHits-1+i*2+1] = store.I64toa(hits)
			return nil
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offAPHitCnt-1] = store.Itoa(n)

	n, err = readCollection(ctx, q,
		`SELECT path, content_hash FROM session_state_file_hashes WHERE session_id = $1 ORDER BY path`,
		sid, maxFileHashes, func(i int, scan func(...any) error) error {
			var stored int64
			if err := scan(&reply[offFileHashes-1+i*2], &stored); err != nil {
				return err
			}
			reply[offFileHashes-1+i*2+1] = store.U64toa(hashFromStored(stored))
			return nil
		})
	if err != nil {
		return 0, nil, err
	}
	reply[offFileHashCnt-1] = store.Itoa(n)

	return store.StatusOK, reply, nil
}

// sessionStateDelete is op 3. The child rows go with it: every child table
// carries ON DELETE CASCADE, so one statement is the whole operation.
func sessionStateDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, sessionStateDeleteSQL, f[0]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// sessionStateExists is op 4: present or not, as a 0/1 cell. Absence is not
// MISSING here -- "is there one" was answered, and the answer was no.
func sessionStateExists(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var one int
	switch err := q.QueryRow(ctx, sessionStateExistsSQL, f[0]).Scan(&one); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{"1"}, nil
}

// sessionStateGetSummary is op 6.
func sessionStateGetSummary(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var sid, updatedAt string
	var hookCalls int64
	switch err := q.QueryRow(ctx, sessionSummarySQL, f[0]).Scan(&sid, &updatedAt, &hookCalls); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{sid, updatedAt, store.I64toa(hookCalls)}, nil
}

// sessionStateList is op 5: recent sessions, three cells each.
func sessionStateList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > sessionStateListMax {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, sessionListSQL, max)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	cells := make([]string, 0, max*3)
	for rows.Next() {
		var sid, updatedAt string
		var hookCalls int64
		if err := rows.Scan(&sid, &updatedAt, &hookCalls); err != nil {
			return 0, nil, err
		}
		cells = append(cells, sid, updatedAt, store.I64toa(hookCalls))
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// sessionStateListExpired is op 7: sessions idle for longer than a threshold.
func sessionStateListExpired(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	threshold, okThreshold := store.Atoi(f[0])
	max, okMax := store.Atoi(f[1])
	if !okThreshold || threshold < 0 || !okMax || max <= 0 || max > sessionStateListMax {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, sessionListExpiredSQL, threshold, max)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	cells := make([]string, 0, max)
	for rows.Next() {
		var sid string
		if err := rows.Scan(&sid); err != nil {
			return 0, nil, err
		}
		cells = append(cells, sid)
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// GuardrailState is the family, ready to be bound to kind 11785.
var GuardrailState = store.Family{
	Name:  "guardrail_state",
	Event: EventGuardrailState,
	Stage: StageGuardrailState,
	Ops: map[uint32]store.Op{
		opSessionStateLoad:        {Name: "session_state_load", Cells: 386, Args: 1, Run: sessionStateLoad},
		opSessionStateSave:        {Name: "session_state_save", Args: saveFields, Tx: true, Run: sessionStateSave},
		opSessionStateDelete:      {Name: "session_state_delete", Args: 1, Tx: true, Run: sessionStateDelete},
		opSessionStateExists:      {Name: "session_state_exists", Args: 1, Run: sessionStateExists},
		opSessionStateList:        {Name: "session_state_list", Cells: 3, Args: 1, Run: sessionStateList},
		opSessionStateGetSummary:  {Name: "session_state_get_summary", Cells: 3, Args: 1, Run: sessionStateGetSummary},
		opSessionStateListExpired: {Name: "session_state_list_expired", Cells: 1, Args: 2, Run: sessionStateListExpired},
	},
}
