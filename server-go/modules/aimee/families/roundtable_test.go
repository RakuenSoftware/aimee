package families

import (
	"context"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- the column spec is the contract -------------------------------------------
//
// Every read and every write on these four entities is derived from the column
// lists, so a single transposed column would be wrong in the SELECT, the row
// reader and the UPDATE at once, consistently, and no behavioural test would
// catch it. These lists are transcribed from the C's RTP_*_COLS macros and the
// column order of its UPDATE statements.

const cRunCols = "id, idea, state, phase, admission_class, schema_version, done_bar, brief, " +
	"gate_digest, proposal_ref, proposal_origin_hash, diff_ref, diff_origin_hash, " +
	"chunk_index_ref, repo_root, remote, base_branch, head_branch, workspace_id, " +
	"workspace_provider, worktree_path, head_sha, base_sha, proposal_pr_number, " +
	"proposal_pr_url, impl_pr_number, impl_pr_url, cost_scope, cost_source, cost_version, " +
	"proposal_phase_cost_usd, impl_phase_cost_usd, total_cost_usd, accepted_question_count, " +
	"created_at, updated_at"

const cPassCols = "id, pipeline_id, phase, mode, pass_no, status, artifact_hash, converged, " +
	"envelope_valid, blocking_count, suggestion_count, nit_count, open_questions, " +
	"coverage_gaps, items_round, artifact_round, best_round, rounds_run, cost_usd, " +
	"result_hash, is_chunked, chunk_total, chunk_done, synthesis_done, chunk_group, " +
	"chunk_index, answered_count, chunk_offset, chunk_len, chunk_omitted, " +
	"chunk_over_budget, created_at, updated_at"

const cAttemptCols = "id, pass_id, attempt_no, run_id, is_current, capture_status, " +
	"terminal_status, parse_status, envelope_valid, items_truncated, truncated, degraded, " +
	"cost_capped, deadline_hit, cancelled, lost_result, result_hash, result_snapshot, " +
	"cost_usd, cost_known, submitted_at, terminal_at"

const cGateCols = "id, pipeline_id, gate_no, verdict, reason, actor, pr_number, " +
	"expected_head_sha, merge_sha, merge_executor, merge_command, merge_output, " +
	"merge_exit_code, resolved_at, created_at"

func colNames(cols []col) []string {
	out := make([]string, len(cols))
	for i, c := range cols {
		out[i] = c.name
	}
	return out
}

func TestColumnListsMatchTheWireContract(t *testing.T) {
	for _, test := range []struct {
		name  string
		cols  []col
		want  string
		width int
	}{
		{"run", runCols, cRunCols, 36},
		{"pass", passCols, cPassCols, 33},
		{"attempt", attemptCols, cAttemptCols, 22},
		{"gate", gateCols, cGateCols, 15},
	} {
		t.Run(test.name, func(t *testing.T) {
			want := strings.Split(test.want, ", ")
			got := colNames(test.cols)
			if len(got) != test.width {
				t.Fatalf("%d columns, want %d", len(got), test.width)
			}
			if len(want) != test.width {
				t.Fatalf("the transcribed list has %d columns, want %d -- fix the test", len(want), test.width)
			}
			for i := range want {
				if got[i] != want[i] {
					t.Fatalf("column %d = %q, want %q", i, got[i], want[i])
				}
			}
		})
	}
}

// The set columns are exactly the ones the C's UPDATE wrote, in order. Getting
// this wrong would write the right values to the wrong columns.
func TestUpdateWritesExactlyTheColumnsTheCDid(t *testing.T) {
	for _, test := range []struct {
		name string
		cols []col
		want string
	}{
		{"run", runCols, "idea, state, phase, admission_class, schema_version, done_bar, " +
			"brief, gate_digest, proposal_ref, proposal_origin_hash, diff_ref, " +
			"diff_origin_hash, chunk_index_ref, repo_root, remote, base_branch, " +
			"head_branch, workspace_id, workspace_provider, worktree_path, head_sha, " +
			"base_sha, proposal_pr_number, proposal_pr_url, impl_pr_number, impl_pr_url, " +
			"cost_scope, cost_source, cost_version, proposal_phase_cost_usd, " +
			"impl_phase_cost_usd, total_cost_usd, accepted_question_count"},
		{"pass", passCols, "status, artifact_hash, converged, envelope_valid, blocking_count, " +
			"suggestion_count, nit_count, open_questions, coverage_gaps, items_round, " +
			"artifact_round, best_round, rounds_run, cost_usd, result_hash, is_chunked, " +
			"chunk_total, chunk_done, synthesis_done, chunk_group, chunk_index, " +
			"answered_count, chunk_offset, chunk_len, chunk_omitted, chunk_over_budget"},
		{"attempt", attemptCols, "is_current, capture_status, terminal_status, parse_status, " +
			"envelope_valid, items_truncated, truncated, degraded, cost_capped, " +
			"deadline_hit, cancelled, lost_result, result_hash, result_snapshot, " +
			"cost_usd, cost_known, terminal_at"},
		{"gate", gateCols, "verdict, reason, actor, pr_number, expected_head_sha, merge_sha, " +
			"merge_executor, merge_command, merge_output, merge_exit_code, resolved_at"},
	} {
		t.Run(test.name, func(t *testing.T) {
			var got []string
			for _, c := range test.cols {
				if c.set {
					got = append(got, c.name)
				}
			}
			want := strings.Split(test.want, ", ")
			if len(got) != len(want) {
				t.Fatalf("%d written columns, want %d: %v", len(got), len(want), got)
			}
			for i := range want {
				if got[i] != want[i] {
					t.Fatalf("written column %d = %q, want %q", i, got[i], want[i])
				}
			}
		})
	}
}

// The placeholders must be numbered in the order the arguments are built, and
// the id must be the last one.
func TestUpdateSQLNumbersItsPlaceholdersInArgumentOrder(t *testing.T) {
	sql := updateSQL("t", gateCols, false)
	for i, name := range []string{"verdict", "reason", "actor", "pr_number"} {
		want := name + " = $" + store.Itoa(i+1)
		if !strings.Contains(sql, want) {
			t.Fatalf("missing %q in %s", want, sql)
		}
	}
	// eleven written columns, so the id is $12
	if !strings.HasSuffix(sql, "WHERE id = $12") {
		t.Fatalf("the id is not the last parameter: %s", sql)
	}
	if strings.Contains(sql, "updated_at") {
		t.Fatalf("gates have no updated_at: %s", sql)
	}

	sql = updateSQL("t", runCols, true)
	if !strings.Contains(sql, "updated_at = now()") {
		t.Fatalf("the run update does not touch updated_at: %s", sql)
	}
	// 33 written columns plus updated_at, so the id is $34
	if !strings.HasSuffix(sql, "WHERE id = $34") {
		t.Fatalf("the id is not the last parameter: %s", sql)
	}
}

// updateArgs reads field i for column i, so a row read back and sent unchanged
// round-trips.
func TestUpdateArgsReadsTheFieldAtEachColumnsIndex(t *testing.T) {
	fields := make([]string, len(gateCols))
	fields[0] = "7"          // id, not written
	fields[3] = "approved"   // verdict
	fields[4] = "looks good" // reason
	fields[6] = "42"         // pr_number
	fields[12] = "0"         // merge_exit_code
	fields[13] = ""          // resolved_at, unset

	args, ok := updateArgs(gateCols, fields)
	if !ok {
		t.Fatalf("updateArgs refused a valid row")
	}
	if len(args) != 11 {
		t.Fatalf("%d args, want 11", len(args))
	}
	if args[0] != "approved" || args[1] != "looks good" {
		t.Fatalf("args = %v", args[:2])
	}
	if args[3].(int64) != 42 {
		t.Fatalf("pr_number = %v", args[3])
	}
	// '' means "not yet", which is NULL in the column -- not an empty string.
	if args[10] != nil {
		t.Fatalf("an unset resolved_at bound as %#v, want NULL", args[10])
	}

	fields[13] = "2026-08-22 09:00:00"
	args, _ = updateArgs(gateCols, fields)
	if args[10] != "2026-08-22 09:00:00" {
		t.Fatalf("a set resolved_at bound as %#v", args[10])
	}
}

func TestUpdateArgsRefusesAMalformedNumber(t *testing.T) {
	fields := make([]string, len(gateCols))
	fields[6] = "forty-two"
	if _, ok := updateArgs(gateCols, fields); ok {
		t.Fatalf("updateArgs accepted a non-numeric pr_number")
	}
}

// A nullable timestamp reads back as ” rather than as the string "NULL" or a
// zero time.
func TestSelectListRendersTimestampsForTheWire(t *testing.T) {
	list := selectList(attemptCols)
	if !strings.Contains(list, "to_char(submitted_at AT TIME ZONE 'UTC'") {
		t.Fatalf("submitted_at is not rendered: %s", list)
	}
	if !strings.Contains(list, "coalesce(to_char(terminal_at") {
		t.Fatalf("a NULL terminal_at would not render as '': %s", list)
	}
	// Everything else is selected plainly.
	if !strings.Contains(list, "capture_status") {
		t.Fatalf("plain columns are missing: %s", list)
	}
}

// --- a roundtable-shaped fake ----------------------------------------------------

type rtRows struct {
	rows [][]any
	at   int
}

func (r *rtRows) Close()     {}
func (r *rtRows) Err() error { return nil }
func (r *rtRows) Next() bool {
	if r.at >= len(r.rows) {
		return false
	}
	r.at++
	return true
}
func (r *rtRows) Scan(dest ...any) error {
	row := r.rows[r.at-1]
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = row[i].(string)
		case *int64:
			*p = row[i].(int64)
		case *bool:
			*p = row[i].(bool)
		}
	}
	return nil
}

type rtDB struct {
	row      []any
	rows     [][]any
	execRows int

	executed []string
	args     [][]any
}

func newRTDB() *rtDB { return &rtDB{execRows: 1} }

func (d *rtDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return store.RowsAffected(d.execRows), nil
}
func (d *rtDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return &rtRows{rows: d.rows}, nil
}

// rtRow is a single row, or the absence of one. It is separate from rtRows
// because an empty result must report ErrNoRows rather than being scanned.
type rtRow struct{ values []any }

func (r rtRow) Scan(dest ...any) error {
	if r.values == nil {
		return store.ErrNoRows
	}
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = r.values[i].(string)
		case *int64:
			*p = r.values[i].(int64)
		case *bool:
			*p = r.values[i].(bool)
		}
	}
	return nil
}

func (d *rtDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return rtRow{values: d.row}
}
func (d *rtDB) Begin(context.Context) (store.Tx, error) { return rtTx{d}, nil }

type rtTx struct{ *rtDB }

func (t rtTx) Commit(context.Context) error   { return nil }
func (t rtTx) Rollback(context.Context) error { return nil }

func rtCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := Roundtable.Handler(db)(bus.ModuleInvocation{StageID: StageRoundtable}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// --- the compare-and-swap ---------------------------------------------------------

// The expected state is in the WHERE clause. A read then a write would let two
// concurrent callers both pass the read and both transition.
func TestCASStateIsAtomicAndReportsALostRace(t *testing.T) {
	db := newRTDB()
	status, _ := rtCall(t, db, opRTRunCASState, []string{"1", "drafting", "reviewing"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(db.executed) != 1 {
		t.Fatalf("the CAS read before writing: %v", db.executed)
	}
	if !strings.Contains(db.executed[0], "AND state = $3") {
		t.Fatalf("the CAS is not guarded on the expected state: %s", db.executed[0])
	}

	db = newRTDB()
	db.execRows = 0
	if status, _ := rtCall(t, db, opRTRunCASState, []string{"1", "drafting", "reviewing"}); status != store.StatusFailed {
		t.Fatalf("a lost race: status = %d, want %d", status, store.StatusFailed)
	}
}

// --- the group aggregate's counting rules -------------------------------------------

// These rules are the C's and each one is load-bearing. A chunk_index below
// zero is the SYNTHESIS member -- a whole-artifact check, not a chunk -- so it
// is not part of the chunk total.
func TestGroupAggregateCountingRules(t *testing.T) {
	// columns: chunk_index, status, envelope_valid, blocking, suggestions,
	//          chunk_omitted, chunk_over_budget
	row := func(idx int64, status string, valid, blocking, sugg, omitted, over int64) []any {
		return []any{idx, status, valid, blocking, sugg, omitted, over}
	}
	agg := func(t *testing.T, rows [][]any) []string {
		t.Helper()
		db := newRTDB()
		db.rows = rows
		status, cells := rtCall(t, db, opRTPassGroupAgg, []string{"1", "review", "2"})
		if status != store.StatusOK {
			t.Fatalf("status = %d", status)
		}
		if len(cells) != 7 {
			t.Fatalf("%d cells, want 7", len(cells))
		}
		return cells
	}

	t.Run("chunks count, synthesis does not", func(t *testing.T) {
		cells := agg(t, [][]any{
			row(0, "captured", 1, 0, 0, 0, 0),
			row(1, "captured", 1, 0, 0, 0, 0),
			row(-1, "captured", 1, 0, 0, 0, 0), // synthesis
		})
		if cells[0] != "2" {
			t.Fatalf("total = %s, want 2 -- the synthesis member was counted", cells[0])
		}
		if cells[1] != "2" {
			t.Fatalf("done = %s, want 2", cells[1])
		}
		if cells[5] != "1" || cells[6] != "1" {
			t.Fatalf("synthesis present/done = %s/%s, want 1/1", cells[5], cells[6])
		}
	})

	// Invalid chunks are COUNTED, not flagged: the caller reports how many.
	t.Run("each invalid chunk is counted", func(t *testing.T) {
		cells := agg(t, [][]any{
			row(0, "captured", 0, 0, 0, 0, 0),
			row(1, "captured", 0, 0, 0, 0, 0),
			row(2, "captured", 1, 0, 0, 0, 0),
		})
		if cells[2] != "2" {
			t.Fatalf("invalid = %s, want 2 -- invalid is a count, not a boolean", cells[2])
		}
		if cells[1] != "1" {
			t.Fatalf("done = %s, want 1", cells[1])
		}
	})

	// A synthesis unit that omitted spans or overflowed its budget is not a
	// complete whole-artifact check, so it blocks the aggregate whatever the
	// roundtable's verdict was.
	t.Run("an incomplete synthesis blocks regardless of verdict", func(t *testing.T) {
		for _, test := range []struct {
			name          string
			omitted, over int64
		}{
			{"omitted spans", 1, 0},
			{"over budget", 0, 1},
		} {
			t.Run(test.name, func(t *testing.T) {
				cells := agg(t, [][]any{
					row(0, "captured", 1, 0, 0, 0, 0),
					row(-1, "captured", 1, 0, 0, test.omitted, test.over),
				})
				if cells[6] != "0" {
					t.Fatalf("synthesis_done = %s, want 0", cells[6])
				}
				if cells[2] != "1" {
					t.Fatalf("invalid = %s, want 1", cells[2])
				}
			})
		}
	})

	// 'done' counts as captured alongside 'captured'.
	t.Run("done counts as captured", func(t *testing.T) {
		cells := agg(t, [][]any{row(0, "done", 1, 0, 0, 0, 0)})
		if cells[1] != "1" {
			t.Fatalf("done = %s, want 1", cells[1])
		}
	})

	// An uncaptured chunk is neither done nor invalid: it has not answered yet.
	t.Run("an open chunk is neither done nor invalid", func(t *testing.T) {
		cells := agg(t, [][]any{row(0, "open", 0, 0, 0, 0, 0)})
		if cells[0] != "1" || cells[1] != "0" || cells[2] != "0" {
			t.Fatalf("total/done/invalid = %s/%s/%s, want 1/0/0", cells[0], cells[1], cells[2])
		}
	})

	// Counts sum across every member including the synthesis one.
	t.Run("blocking and suggestions sum across all members", func(t *testing.T) {
		cells := agg(t, [][]any{
			row(0, "captured", 1, 2, 3, 0, 0),
			row(1, "captured", 1, 4, 5, 0, 0),
			row(-1, "captured", 1, 6, 7, 0, 0),
		})
		if cells[3] != "12" || cells[4] != "15" {
			t.Fatalf("blocking/suggestions = %s/%s, want 12/15", cells[3], cells[4])
		}
	})
}

// --- terminal states are excluded consistently ---------------------------------------

func TestLiveQueriesExcludeTerminalRuns(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"unfiltered list", opRTRunList, []string{"", "10"}},
		{"active count", opRTRunCountActive, nil},
		{"branch owner", opRTRunBranchOwner, []string{"/repo", "feature", "0"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newRTDB()
			db.row = []any{int64(0)}
			rtCall(t, db, test.op, test.fields)
			sql := db.executed[0]
			if !strings.Contains(sql, "state <> ALL(") {
				t.Fatalf("terminal runs are not excluded: %s", sql)
			}
			var states []string
			for _, args := range db.args[0] {
				if s, ok := args.([]string); ok {
					states = s
				}
			}
			if len(states) != 3 {
				t.Fatalf("bound terminal states = %v, want three", states)
			}
		})
	}

	// A state filter asks for exactly that state, terminal or not.
	db := newRTDB()
	rtCall(t, db, opRTRunList, []string{"done", "10"})
	if strings.Contains(db.executed[0], "<> ALL(") {
		t.Fatalf("an explicit state filter still excluded terminal runs: %s", db.executed[0])
	}
}

// --- the small semantics ---------------------------------------------------------------

// 0 means nobody owns the branch, which is an answer rather than an absence:
// the caller asks this as a guard before claiming one.
func TestBranchOwnerAnswersZeroForNobody(t *testing.T) {
	db := newRTDB()
	status, cells := rtCall(t, db, opRTRunBranchOwner, []string{"/repo", "feature", "0"})
	if status != store.StatusOK || cells[0] != "0" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}

	db = newRTDB()
	db.row = []any{int64(42)}
	_, cells = rtCall(t, db, opRTRunBranchOwner, []string{"/repo", "feature", "0"})
	if cells[0] != "42" {
		t.Fatalf("cells = %v", cells)
	}
}

// coalesce so a pipeline with no passes answers 0: the caller adds one to get
// the next number, and nothing would give it no number at all.
func TestMaxNumbersCoalesceToZero(t *testing.T) {
	for _, test := range []struct {
		op     uint32
		fields []string
	}{
		{opRTPassMaxNo, []string{"1", "review"}},
		{opRTPassMaxGroup, []string{"1", "review"}},
		{opRTAttemptMaxNo, []string{"1"}},
	} {
		db := newRTDB()
		db.row = []any{int64(0)}
		status, cells := rtCall(t, db, test.op, test.fields)
		if status != store.StatusOK || cells[0] != "0" {
			t.Fatalf("status = %d, cells = %v", status, cells)
		}
		if !strings.Contains(db.executed[0], "coalesce(max(") {
			t.Fatalf("an empty set would answer nothing: %s", db.executed[0])
		}
	}
}

// An empty phase leaves the phase alone rather than blanking it.
func TestSetStateLeavesAnEmptyPhaseAlone(t *testing.T) {
	db := newRTDB()
	rtCall(t, db, opRTRunSetState, []string{"1", "reviewing", ""})
	if strings.Contains(db.executed[0], "phase") {
		t.Fatalf("an empty phase was written: %s", db.executed[0])
	}

	db = newRTDB()
	rtCall(t, db, opRTRunSetState, []string{"1", "reviewing", "impl"})
	if !strings.Contains(db.executed[0], "phase = $3") {
		t.Fatalf("a supplied phase was not written: %s", db.executed[0])
	}
}

// schema_version 0 means unset and becomes 1: a row written with no version is
// version 1, not version 0.
func TestRunUpdateNormalisesAnUnsetSchemaVersion(t *testing.T) {
	fields := make([]string, len(runCols))
	for i, c := range runCols {
		if c.kind == kInt || c.kind == kFloat {
			fields[i] = "0"
		}
	}
	fields[0] = "1" // after the fill: id is numeric too, and 0 is not an id
	db := newRTDB()
	if status, _ := rtCall(t, db, opRTRunUpdate, fields); status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	// schema_version is the fifth written column, so args[4].
	if got := db.args[0][4].(int64); got != 1 {
		t.Fatalf("schema_version written as %d, want 1", got)
	}

	fields[5] = "3"
	db = newRTDB()
	rtCall(t, db, opRTRunUpdate, fields)
	if got := db.args[0][4].(int64); got != 3 {
		t.Fatalf("an explicit schema_version was overwritten with %d", got)
	}
}

// A missing gate is not an overdue gate.
func TestGateAgeAnswersZeroWhenThereIsNoGate(t *testing.T) {
	db := newRTDB()
	status, cells := rtCall(t, db, opRTGateAgeExceedsHours, []string{"1", "1", "24"})
	if status != store.StatusOK || cells[0] != "0" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}

	db = newRTDB()
	db.row = []any{true}
	_, cells = rtCall(t, db, opRTGateAgeExceedsHours, []string{"1", "1", "24"})
	if cells[0] != "1" {
		t.Fatalf("cells = %v", cells)
	}
	// The age is measured against the database's clock.
	if !strings.Contains(db.executed[0], "now() - make_interval(hours =>") {
		t.Fatalf("the age is not an interval against now(): %s", db.executed[0])
	}
}

func TestRoundtableValidatesItsArguments(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"run get with a bad id", opRTRunGet, []string{"nope"}},
		{"run get with id zero", opRTRunGet, []string{"0"}},
		{"cas with no expected state", opRTRunCASState, []string{"1", "", "next"}},
		{"list max zero", opRTRunList, []string{"", "0"}},
		{"list max too large", opRTRunList, []string{"", "99999"}},
		{"pass create with no phase", opRTPassCreate, []string{"1", "", "mode", "1", "open"}},
		{"group agg with group zero", opRTPassGroupAgg, []string{"1", "review", "0"}},
		{"attempt current with a bad pass", opRTAttemptCurrent, []string{"0"}},
		{"gate get with a bad gate no", opRTGateGet, []string{"1", "nope"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newRTDB()
			status, _ := rtCall(t, db, test.op, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid request ran %d statements", len(db.executed))
			}
		})
	}
}

// Every LIMIT read is totally ordered.
func TestRoundtableListReadsAreTotallyOrdered(t *testing.T) {
	for _, test := range []struct {
		op     uint32
		fields []string
	}{
		{opRTRunList, []string{"", "10"}},
		{opRTRunList, []string{"done", "10"}},
		{opRTPassLatest, []string{"1", "review"}},
		{opRTAttemptGetByRun, []string{"run-1"}},
		{opRTAttemptCurrent, []string{"1"}},
		{opRTGateGet, []string{"1", "1"}},
	} {
		db := newRTDB()
		db.row = nil
		rtCall(t, db, test.op, test.fields)
		sql := db.executed[0]
		if !strings.Contains(sql, "LIMIT") {
			continue
		}
		if !strings.Contains(sql, "ORDER BY") {
			t.Fatalf("a LIMIT read has no ORDER BY: %s", sql)
		}
	}
}
