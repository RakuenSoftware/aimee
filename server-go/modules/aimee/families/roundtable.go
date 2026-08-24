package families

import (
	"context"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The roundtable pipeline: a run has passes, a pass has attempts, a run has
// gates.
const (
	EventRoundtable uint32 = 11788
	StageRoundtable uint32 = 12

	opRTRunCreate           uint32 = 1
	opRTRunGet              uint32 = 2
	opRTRunUpdate           uint32 = 3
	opRTRunSetState         uint32 = 4
	opRTRunCASState         uint32 = 5
	opRTRunList             uint32 = 6
	opRTRunCountActive      uint32 = 7
	opRTRunBranchOwner      uint32 = 8
	opRTPassCreate          uint32 = 9
	opRTPassGet             uint32 = 10
	opRTPassUpdate          uint32 = 11
	opRTPassLatest          uint32 = 12
	opRTPassMaxNo           uint32 = 13
	opRTPassMaxGroup        uint32 = 14
	opRTPassGroupAgg        uint32 = 15
	opRTAttemptCreate       uint32 = 16
	opRTAttemptGetByRun     uint32 = 17
	opRTAttemptCurrent      uint32 = 18
	opRTAttemptUpdate       uint32 = 19
	opRTAttemptMaxNo        uint32 = 20
	opRTAttemptSupersede    uint32 = 21
	opRTGateCreate          uint32 = 22
	opRTGateGet             uint32 = 23
	opRTGateUpdate          uint32 = 24
	opRTGateAgeExceedsHours uint32 = 25
)

const roundtableListMax = 512

// terminalStates are the run states that end a pipeline. The list, the active
// count and the branch-owner check all exclude them, so they are named once.
var terminalStates = []string{"done", "failed", "abandoned"}

// --- a column spec, because these rows are wide --------------------------------
//
// The four entities carry 36, 33, 22 and 15 columns. Writing each one out by
// hand at every read and write is 106 chances to transpose two of them, so the
// shape is declared once and the SELECT list, the row reader and the UPDATE are
// all derived from it.

type colKind uint8

const (
	kText colKind = iota
	kInt
	kFloat
	kStamp     // NOT NULL timestamptz, rendered to the wire's spelling
	kNullStamp // nullable timestamptz; NULL is '' on the wire
	kBoolInt   // boolean column carried as 0/1, which is what the wire has
)

type col struct {
	name string
	kind colKind
	// set marks a column the entity's update writes. id and the creation
	// columns are not written, which is why this is a property of the column
	// rather than a slice of the list.
	set bool
}

func text(name string, set bool) col  { return col{name, kText, set} }
func num(name string, set bool) col   { return col{name, kInt, set} }
func real_(name string, set bool) col { return col{name, kFloat, set} }

// boolint is a BOOLEAN column that the wire spells 0/1. The cast belongs in
// SQL rather than the scan so the column keeps its real type in the schema --
// `WHERE NOT success` is what the owning family writes, and that only works on
// a boolean.
func boolint(name string, set bool) col { return col{name, kBoolInt, set} }

// selectList renders the columns for a read: timestamps become the wire's text
// spelling in SQL, everything else is scanned in its own type.
func selectList(cols []col) string {
	parts := make([]string, len(cols))
	for i, c := range cols {
		switch c.kind {
		case kBoolInt:
			parts[i] = c.name + `::int`
		case kStamp:
			parts[i] = `to_char(` + c.name + ` AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')`
		case kNullStamp:
			// A NULL timestamp is '' on the wire: that is what "not yet" has
			// always looked like to a caller.
			parts[i] = `coalesce(to_char(` + c.name +
				` AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), '')`
		default:
			parts[i] = c.name
		}
	}
	return strings.Join(parts, ", ")
}

// scanRow reads one row into the cells the wire carries.
func scanRow(cols []col, scan func(...any) error) ([]string, error) {
	texts := make([]string, len(cols))
	ints := make([]int64, len(cols))
	floats := make([]float64, len(cols))
	dest := make([]any, len(cols))
	for i, c := range cols {
		switch c.kind {
		case kInt:
			dest[i] = &ints[i]
		case kFloat:
			dest[i] = &floats[i]
		default:
			dest[i] = &texts[i]
		}
	}
	if err := scan(dest...); err != nil {
		return nil, err
	}
	cells := make([]string, len(cols))
	for i, c := range cols {
		switch c.kind {
		case kInt:
			cells[i] = store.I64toa(ints[i])
		case kFloat:
			cells[i] = store.Ftoa(floats[i])
		default:
			cells[i] = texts[i]
		}
	}
	return cells, nil
}

// updateSQL builds the UPDATE for the columns the entity writes, with the row's
// id as the last parameter.
func updateSQL(table string, cols []col, touchUpdatedAt bool) string {
	var sets []string
	n := 0
	for _, c := range cols {
		if !c.set {
			continue
		}
		n++
		sets = append(sets, c.name+" = $"+store.Itoa(n))
	}
	if touchUpdatedAt {
		sets = append(sets, "updated_at = now()")
	}
	return "UPDATE " + table + " SET " + strings.Join(sets, ", ") +
		" WHERE id = $" + store.Itoa(n+1)
}

// updateArgs converts the request fields for the columns the update writes.
//
// The fields are the WHOLE row in column order, including the ones the update
// does not write, so the caller sends what it read back.
func updateArgs(cols []col, fields []string) ([]any, bool) {
	var args []any
	for i, c := range cols {
		if !c.set {
			continue
		}
		switch c.kind {
		case kInt:
			v, ok := store.Atoi64(fields[i])
			if !ok {
				return nil, false
			}
			args = append(args, v)
		case kFloat:
			v, ok := store.Atof(fields[i])
			if !ok {
				return nil, false
			}
			args = append(args, v)
		case kNullStamp:
			// '' means "not yet", which is NULL in the column.
			if fields[i] == "" {
				args = append(args, nil)
			} else {
				args = append(args, fields[i])
			}
		default:
			args = append(args, fields[i])
		}
	}
	return args, true
}

// --- the four entities ----------------------------------------------------------

var runCols = []col{
	num("id", false), text("idea", true), text("state", true), text("phase", true),
	text("admission_class", true), num("schema_version", true), text("done_bar", true),
	text("brief", true), text("gate_digest", true), text("proposal_ref", true),
	text("proposal_origin_hash", true), text("diff_ref", true), text("diff_origin_hash", true),
	text("chunk_index_ref", true), text("repo_root", true), text("remote", true),
	text("base_branch", true), text("head_branch", true), text("workspace_id", true),
	text("workspace_provider", true), text("worktree_path", true), text("head_sha", true),
	text("base_sha", true), num("proposal_pr_number", true), text("proposal_pr_url", true),
	num("impl_pr_number", true), text("impl_pr_url", true), text("cost_scope", true),
	text("cost_source", true), num("cost_version", true),
	real_("proposal_phase_cost_usd", true), real_("impl_phase_cost_usd", true),
	real_("total_cost_usd", true), num("accepted_question_count", true),
	{name: "created_at", kind: kStamp}, {name: "updated_at", kind: kStamp},
}

var passCols = []col{
	num("id", false), num("pipeline_id", false), text("phase", false), text("mode", false),
	num("pass_no", false), text("status", true), text("artifact_hash", true),
	num("converged", true), num("envelope_valid", true), num("blocking_count", true),
	num("suggestion_count", true), num("nit_count", true), num("open_questions", true),
	num("coverage_gaps", true), num("items_round", true), num("artifact_round", true),
	num("best_round", true), num("rounds_run", true), real_("cost_usd", true),
	text("result_hash", true), num("is_chunked", true), num("chunk_total", true),
	num("chunk_done", true), num("synthesis_done", true), num("chunk_group", true),
	num("chunk_index", true), num("answered_count", true), num("chunk_offset", true),
	num("chunk_len", true), num("chunk_omitted", true), num("chunk_over_budget", true),
	{name: "created_at", kind: kStamp}, {name: "updated_at", kind: kStamp},
}

var attemptCols = []col{
	num("id", false), num("pass_id", false), num("attempt_no", false), text("run_id", false),
	num("is_current", true), text("capture_status", true), text("terminal_status", true),
	text("parse_status", true), num("envelope_valid", true), num("items_truncated", true),
	num("truncated", true), num("degraded", true), num("cost_capped", true),
	num("deadline_hit", true), num("cancelled", true), num("lost_result", true),
	text("result_hash", true), text("result_snapshot", true), real_("cost_usd", true),
	num("cost_known", true),
	{name: "submitted_at", kind: kStamp},
	{name: "terminal_at", kind: kNullStamp, set: true},
}

var gateCols = []col{
	num("id", false), num("pipeline_id", false), num("gate_no", false),
	text("verdict", true), text("reason", true), text("actor", true),
	num("pr_number", true), text("expected_head_sha", true), text("merge_sha", true),
	text("merge_executor", true), text("merge_command", true), text("merge_output", true),
	num("merge_exit_code", true),
	{name: "resolved_at", kind: kNullStamp, set: true},
	{name: "created_at", kind: kStamp},
}

// --- reads ------------------------------------------------------------------------

// readOne is the shape of every single-row read here.
func readOne(ctx context.Context, q store.Queryer, sql string, cols []col,
	args ...any) (uint32, []string, error) {
	cells, err := scanRow(cols, q.QueryRow(ctx, sql, args...).Scan)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

func readMany(ctx context.Context, q store.Queryer, sql string, cols []col,
	args ...any) (uint32, []string, error) {
	rows, err := q.Query(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	var cells []string
	for rows.Next() {
		row, err := scanRow(cols, rows.Scan)
		if err != nil {
			return 0, nil, err
		}
		cells = append(cells, row...)
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

func scalar(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	var n int64
	if err := q.QueryRow(ctx, sql, args...).Scan(&n); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(n)}, nil
}

// --- runs ---------------------------------------------------------------------------

func rtRunCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, `INSERT INTO roundtable_pipeline_runs
	        (idea, state, phase, admission_class, done_bar)
	    VALUES ($1, 'drafting', 'proposal', $2, $3)
	    RETURNING id`, f[0], f[2], f[3]).Scan(&id)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func rtRunGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(runCols)+` FROM roundtable_pipeline_runs WHERE id = $1`,
		runCols, id)
}

func rtRunUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	args, ok := updateArgs(runCols, f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// schema_version 0 means "unset" and becomes 1, as it did in the C: a row
	// written with no version is version 1, not version 0.
	const schemaVersionArg = 4
	if v, _ := args[schemaVersionArg].(int64); v == 0 {
		args[schemaVersionArg] = int64(1)
	}
	if _, err := q.Exec(ctx, updateSQL("roundtable_pipeline_runs", runCols, true),
		append(args, id)...); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func rtRunSetState(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	// The phase is optional: an empty one leaves it alone rather than blanking
	// it, which is why this is two statements and not a coalesce.
	var err error
	if f[2] == "" {
		_, err = q.Exec(ctx, `UPDATE roundtable_pipeline_runs
		                         SET state = $2, updated_at = now() WHERE id = $1`, id, f[1])
	} else {
		_, err = q.Exec(ctx, `UPDATE roundtable_pipeline_runs
		                         SET state = $2, phase = $3, updated_at = now()
		                       WHERE id = $1`, id, f[1], f[2])
	}
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// rtRunCASState is op 5: a compare-and-swap on the run's state.
//
// The expected state is in the WHERE clause, which is what makes the transition
// atomic: of two callers racing, exactly one matches and changes a row. The
// loser changes zero and is told so. A read followed by a write would let both
// pass the read.
func rtRunCASState(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, `UPDATE roundtable_pipeline_runs
	                            SET state = $2, updated_at = now()
	                          WHERE id = $1 AND state = $3`, id, f[2], f[1])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() == 0 {
		// Not an error: the run was not in the state the caller expected.
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

func rtRunList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if !ok || max <= 0 || max > roundtableListMax {
		return store.StatusInvalid, nil, nil
	}
	list := selectList(runCols)
	if f[0] != "" {
		return readMany(ctx, q,
			`SELECT `+list+` FROM roundtable_pipeline_runs
			  WHERE state = $1 ORDER BY updated_at DESC, id DESC LIMIT $2`,
			runCols, f[0], max)
	}
	// No filter means "everything still running".
	return readMany(ctx, q,
		`SELECT `+list+` FROM roundtable_pipeline_runs
		  WHERE state <> ALL($1) ORDER BY updated_at DESC, id DESC LIMIT $2`,
		runCols, terminalStates, max)
}

func rtRunCountActive(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	return scalar(ctx, q, `SELECT count(*) FROM roundtable_pipeline_runs
	                        WHERE admission_class = 'active' AND state <> ALL($1)`,
		terminalStates)
}

// rtRunBranchOwner is op 8: which other live run already holds this head branch.
//
// 0 means nobody, which is why it answers OK with a zero rather than MISSING:
// the caller asks this as a guard before claiming a branch.
func rtRunBranchOwner(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	exclude, ok := store.Atoi64(f[2])
	if !ok || f[1] == "" {
		return store.StatusOK, []string{"0"}, nil
	}
	var owner int64
	// The repo_root comparison is skipped when either side is blank: a run
	// recorded before repo_root existed still owns its branch.
	err := q.QueryRow(ctx, `SELECT id FROM roundtable_pipeline_runs
	                         WHERE head_branch = $1 AND id <> $2
	                           AND state <> ALL($3)
	                           AND ($4 = '' OR repo_root = '' OR repo_root = $4)
	                         ORDER BY id DESC LIMIT 1`,
		f[1], exclude, terminalStates, f[0]).Scan(&owner)
	switch {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(owner)}, nil
}

// --- passes ----------------------------------------------------------------------

func rtPassCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, okID := store.Atoi64(f[0])
	passNo, okNo := store.Atoi64(f[3])
	if !okID || pipelineID <= 0 || f[1] == "" || !okNo {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, `INSERT INTO roundtable_pipeline_passes
	        (pipeline_id, phase, mode, pass_no, status)
	    VALUES ($1, $2, $3, $4, $5)
	    RETURNING id`, pipelineID, f[1], f[2], passNo, f[4]).Scan(&id)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func rtPassGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(passCols)+` FROM roundtable_pipeline_passes WHERE id = $1`,
		passCols, id)
}

func rtPassUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	args, ok := updateArgs(passCols, f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, updateSQL("roundtable_pipeline_passes", passCols, true),
		append(args, id)...); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func rtPassLatest(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, ok := store.Atoi64(f[0])
	if !ok || pipelineID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(passCols)+` FROM roundtable_pipeline_passes
		  WHERE pipeline_id = $1 AND phase = $2
		  ORDER BY pass_no DESC, id DESC LIMIT 1`,
		passCols, pipelineID, f[1])
}

func rtPassMaxNo(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, ok := store.Atoi64(f[0])
	if !ok || pipelineID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	// coalesce so a pipeline with no passes answers 0 rather than nothing: the
	// caller adds one to get the next number.
	return scalar(ctx, q, `SELECT coalesce(max(pass_no), 0) FROM roundtable_pipeline_passes
	                        WHERE pipeline_id = $1 AND phase = $2`, pipelineID, f[1])
}

func rtPassMaxGroup(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, ok := store.Atoi64(f[0])
	if !ok || pipelineID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, `SELECT coalesce(max(chunk_group), 0) FROM roundtable_pipeline_passes
	                        WHERE pipeline_id = $1 AND phase = $2`, pipelineID, f[1])
}

// rtPassGroupAgg is op 15: whether a chunk group is complete.
//
// The counting rules are the interesting part and they are the C's, kept:
//
//   - A member with chunk_index < 0 is the SYNTHESIS unit -- a whole-artifact
//     check rather than a chunk -- so it is not counted in the total.
//   - A synthesis unit that omitted required spans or overflowed its budget is
//     not a complete check, so it blocks the aggregate whatever its verdict.
//   - Invalid chunks are COUNTED, not flagged: the caller reports how many.
func rtPassGroupAgg(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, okID := store.Atoi64(f[0])
	chunkGroup, okGroup := store.Atoi64(f[2])
	if !okID || pipelineID <= 0 || f[1] == "" || !okGroup || chunkGroup <= 0 {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, `SELECT chunk_index, status, envelope_valid, blocking_count,
	                                  suggestion_count, chunk_omitted, chunk_over_budget
	                             FROM roundtable_pipeline_passes
	                            WHERE pipeline_id = $1 AND phase = $2 AND chunk_group = $3`,
		pipelineID, f[1], chunkGroup)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()

	var total, done, invalid, blocking, suggestions int64
	var synthesisPresent, synthesisDone int64
	for rows.Next() {
		var chunkIndex, valid, blockingCount, suggestionCount, omitted, overBudget int64
		var status string
		if err := rows.Scan(&chunkIndex, &status, &valid, &blockingCount,
			&suggestionCount, &omitted, &overBudget); err != nil {
			return 0, nil, err
		}
		captured := status == "captured" || status == "done"
		blocking += blockingCount
		suggestions += suggestionCount

		if chunkIndex < 0 {
			synthesisPresent = 1
			switch {
			case omitted > 0 || overBudget != 0:
				invalid++
			case captured && valid != 0:
				synthesisDone = 1
			case captured:
				invalid++
			}
			continue
		}
		total++
		switch {
		case captured && valid != 0:
			done++
		case captured:
			invalid++
		}
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{
		store.I64toa(total), store.I64toa(done), store.I64toa(invalid),
		store.I64toa(blocking), store.I64toa(suggestions),
		store.I64toa(synthesisPresent), store.I64toa(synthesisDone),
	}, nil
}

// --- attempts -----------------------------------------------------------------------

func rtAttemptCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	passID, okPass := store.Atoi64(f[0])
	attemptNo, okNo := store.Atoi64(f[1])
	if !okPass || passID <= 0 || !okNo {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, `INSERT INTO roundtable_pipeline_attempts
	        (pass_id, attempt_no, run_id, is_current, capture_status)
	    VALUES ($1, $2, $3, 1, 'pending')
	    RETURNING id`, passID, attemptNo, f[2]).Scan(&id)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func rtAttemptGetByRun(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(attemptCols)+` FROM roundtable_pipeline_attempts
		  WHERE run_id = $1 ORDER BY id DESC LIMIT 1`,
		attemptCols, f[0])
}

func rtAttemptCurrent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	passID, ok := store.Atoi64(f[0])
	if !ok || passID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(attemptCols)+` FROM roundtable_pipeline_attempts
		  WHERE pass_id = $1 AND is_current = 1 ORDER BY id DESC LIMIT 1`,
		attemptCols, passID)
}

func rtAttemptUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	args, ok := updateArgs(attemptCols, f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// Attempts have no updated_at: submitted_at and terminal_at are the record.
	if _, err := q.Exec(ctx, updateSQL("roundtable_pipeline_attempts", attemptCols, false),
		append(args, id)...); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func rtAttemptMaxNo(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	passID, ok := store.Atoi64(f[0])
	if !ok || passID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, `SELECT coalesce(max(attempt_no), 0) FROM roundtable_pipeline_attempts
	                        WHERE pass_id = $1`, passID)
}

// rtAttemptSupersede is op 21: make one attempt the only current one.
func rtAttemptSupersede(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	passID, okPass := store.Atoi64(f[0])
	keepID, okKeep := store.Atoi64(f[1])
	if !okPass || passID <= 0 || !okKeep {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, `UPDATE roundtable_pipeline_attempts SET is_current = 0
	                           WHERE pass_id = $1 AND id <> $2`, passID, keepID); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- gates ---------------------------------------------------------------------------

func rtGateCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, okID := store.Atoi64(f[0])
	gateNo, okNo := store.Atoi64(f[1])
	prNumber, okPR := store.Atoi64(f[2])
	if !okID || pipelineID <= 0 || !okNo || !okPR {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, `INSERT INTO roundtable_pipeline_gates
	        (pipeline_id, gate_no, pr_number, expected_head_sha)
	    VALUES ($1, $2, $3, $4)
	    RETURNING id`, pipelineID, gateNo, prNumber, f[3]).Scan(&id)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

// rtGateGet is op 23: the newest gate for a (pipeline, gate_no).
func rtGateGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, okID := store.Atoi64(f[0])
	gateNo, okNo := store.Atoi64(f[1])
	if !okID || pipelineID <= 0 || !okNo {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q,
		`SELECT `+selectList(gateCols)+` FROM roundtable_pipeline_gates
		  WHERE pipeline_id = $1 AND gate_no = $2 ORDER BY id DESC LIMIT 1`,
		gateCols, pipelineID, gateNo)
}

func rtGateUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	args, ok := updateArgs(gateCols, f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// Gates have no updated_at: resolved_at is the record.
	if _, err := q.Exec(ctx, updateSQL("roundtable_pipeline_gates", gateCols, false),
		append(args, id)...); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// rtGateAgeExceedsHours is op 25: has this gate been waiting too long.
//
// The age is measured against the DATABASE's clock. The C computed it with
// strftime arithmetic on a TEXT column; this is an interval comparison against
// a timestamp, which is the same question asked in the type system.
func rtGateAgeExceedsHours(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	pipelineID, okID := store.Atoi64(f[0])
	gateNo, okNo := store.Atoi64(f[1])
	hours, okHours := store.Atoi(f[2])
	if !okID || pipelineID <= 0 || !okNo || !okHours || hours <= 0 {
		return store.StatusOK, []string{"0"}, nil
	}
	var over bool
	err := q.QueryRow(ctx, `SELECT created_at < now() - make_interval(hours => $3)
	                          FROM roundtable_pipeline_gates
	                         WHERE pipeline_id = $1 AND gate_no = $2
	                         ORDER BY id DESC LIMIT 1`,
		pipelineID, gateNo, hours).Scan(&over)
	switch {
	case store.IsNoRows(err):
		// No gate is not an overdue gate.
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(over)}, nil
}

// Roundtable is the family, ready to be bound to kind 11788.
var Roundtable = store.Family{
	Name:  "roundtable",
	Event: EventRoundtable,
	Stage: StageRoundtable,
	Ops: map[uint32]store.Op{
		opRTRunCreate:           {Name: "roundtable_run_create", Args: 4, Tx: true, Run: rtRunCreate},
		opRTRunGet:              {Name: "roundtable_run_get", Cells: 36, Args: 1, Run: rtRunGet},
		opRTRunUpdate:           {Name: "roundtable_run_update", Args: len(runCols), Tx: true, Run: rtRunUpdate},
		opRTRunSetState:         {Name: "roundtable_run_set_state", Args: 3, Tx: true, Run: rtRunSetState},
		opRTRunCASState:         {Name: "roundtable_run_cas_state", Args: 3, Tx: true, Run: rtRunCASState},
		opRTRunList:             {Name: "roundtable_run_list", Cells: len(runCols), Args: 2, Run: rtRunList},
		opRTRunCountActive:      {Name: "roundtable_run_count_active", Args: 0, Run: rtRunCountActive},
		opRTRunBranchOwner:      {Name: "roundtable_run_branch_owner", Args: 3, Run: rtRunBranchOwner},
		opRTPassCreate:          {Name: "roundtable_pass_create", Args: 5, Tx: true, Run: rtPassCreate},
		opRTPassGet:             {Name: "roundtable_pass_get", Cells: 33, Args: 1, Run: rtPassGet},
		opRTPassUpdate:          {Name: "roundtable_pass_update", Args: len(passCols), Tx: true, Run: rtPassUpdate},
		opRTPassLatest:          {Name: "roundtable_pass_latest", Cells: 33, Args: 2, Run: rtPassLatest},
		opRTPassMaxNo:           {Name: "roundtable_pass_max_no", Args: 2, Run: rtPassMaxNo},
		opRTPassMaxGroup:        {Name: "roundtable_pass_max_group", Args: 2, Run: rtPassMaxGroup},
		opRTPassGroupAgg:        {Name: "roundtable_pass_group_agg", Cells: 7, Args: 3, Run: rtPassGroupAgg},
		opRTAttemptCreate:       {Name: "roundtable_attempt_create", Args: 3, Tx: true, Run: rtAttemptCreate},
		opRTAttemptGetByRun:     {Name: "roundtable_attempt_get_by_run", Cells: 22, Args: 1, Run: rtAttemptGetByRun},
		opRTAttemptCurrent:      {Name: "roundtable_attempt_current", Cells: 22, Args: 1, Run: rtAttemptCurrent},
		opRTAttemptUpdate:       {Name: "roundtable_attempt_update", Args: len(attemptCols), Tx: true, Run: rtAttemptUpdate},
		opRTAttemptMaxNo:        {Name: "roundtable_attempt_max_no", Args: 1, Run: rtAttemptMaxNo},
		opRTAttemptSupersede:    {Name: "roundtable_attempt_supersede_others", Args: 2, Tx: true, Run: rtAttemptSupersede},
		opRTGateCreate:          {Name: "roundtable_gate_create", Args: 4, Tx: true, Run: rtGateCreate},
		opRTGateGet:             {Name: "roundtable_gate_get", Cells: 15, Args: 2, Run: rtGateGet},
		opRTGateUpdate:          {Name: "roundtable_gate_update", Args: len(gateCols), Tx: true, Run: rtGateUpdate},
		opRTGateAgeExceedsHours: {Name: "roundtable_gate_age_exceeds_hours", Args: 3, Run: rtGateAgeExceedsHours},
	},
}
