package families

import (
	"context"
	"fmt"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a delegation-shaped fake --------------------------------------------------

type delRow struct {
	values []any
	err    error
}

func (r delRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if r.values == nil {
		return store.ErrNoRows
	}
	// A type mismatch here is a fixture that does not describe the row the
	// query reads, so it is reported rather than panicking three frames down.
	for i := range dest {
		if i >= len(r.values) {
			return fmt.Errorf("fixture has %d values, query scans %d", len(r.values), len(dest))
		}
		switch p := dest[i].(type) {
		case *string:
			v, ok := r.values[i].(string)
			if !ok {
				return fmt.Errorf("column %d: fixture holds %T, query scans a string", i, r.values[i])
			}
			*p = v
		case *int64:
			v, ok := r.values[i].(int64)
			if !ok {
				return fmt.Errorf("column %d: fixture holds %T, query scans an int64", i, r.values[i])
			}
			*p = v
		case *bool:
			v, ok := r.values[i].(bool)
			if !ok {
				return fmt.Errorf("column %d: fixture holds %T, query scans a bool", i, r.values[i])
			}
			*p = v
		case *float64:
			v, ok := r.values[i].(float64)
			if !ok {
				return fmt.Errorf("column %d: fixture holds %T, query scans a float64", i, r.values[i])
			}
			*p = v
		}
	}
	return nil
}

type delDB struct {
	// counts is consumed in order by the learning eviction's repeated
	// count(*) reads, so a test can describe a store that shrinks.
	counts   []int64
	row      []any
	rows     [][]any
	present  bool
	execRows int

	executed []string
	args     [][]any
}

func newDelDB() *delDB { return &delDB{present: true, execRows: 1} }

func (d *delDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return store.RowsAffected(d.execRows), nil
}
func (d *delDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return &rtRows{rows: d.rows}, nil
}
func (d *delDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	switch {
	case strings.Contains(sql, "to_regclass"):
		return delRow{values: []any{d.present}}
	case strings.Contains(sql, "count(*) FROM delegate_learnings"):
		if len(d.counts) == 0 {
			return delRow{values: []any{int64(0)}}
		}
		n := d.counts[0]
		d.counts = d.counts[1:]
		return delRow{values: []any{n}}
	}
	return delRow{values: d.row}
}
func (d *delDB) Begin(context.Context) (store.Tx, error) { return delTx{d}, nil }

type delTx struct{ *delDB }

func (t delTx) Commit(context.Context) error   { return nil }
func (t delTx) Rollback(context.Context) error { return nil }

func delCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := Delegation.Handler(db)(bus.ModuleInvocation{StageID: StageDelegation}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// --- the recursive walks are bounded --------------------------------------------

// Nothing prevents a cycle in parent_delegation_id on write, and the C's CTEs
// had no bound: a loop recursed until the process gave up. Every recursive
// statement now carries a depth guard.
//
// This checks the SHAPE -- that a guard is present and its bound is a bound
// parameter. It cannot check the EFFECT: whether the recursion terminates is a
// database behaviour, and a guard can be textually present but semantically
// neutered (`true OR d.depth < $2` contains everything this looks for, and a
// mutation that wrote exactly that went unnoticed here). The assertion with
// teeth is in scripts/test-family-delegation.sql, which builds an actual cycle
// and requires the query to return.
func TestRecursiveWalksAreDepthBounded(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
		row    []any
	}{
		{"find root", opDelegationSpawnFindRoot, []string{"d-1"}, []any{"d-root"}},
		{"count descendants", opDelegationSpawnCountDescendants, []string{"d-1"}, []any{int64(3)}},
		{"cancel recursive", opDelegationSpawnCancelRecursive, []string{"1"}, nil},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newDelDB()
			db.row = test.row
			delCall(t, db, test.op, test.fields)
			var recursive string
			for _, sql := range db.executed {
				if strings.Contains(sql, "WITH RECURSIVE") {
					recursive = sql
				}
			}
			if recursive == "" {
				t.Fatalf("no recursive statement ran: %v", db.executed)
			}
			if !strings.Contains(recursive, "depth <") {
				t.Fatalf("the walk has no depth guard, so a cycle never terminates:\n%s", recursive)
			}
			var bounded bool
			for _, args := range db.args {
				for _, a := range args {
					if n, ok := a.(int); ok && n == treeDepthMax {
						bounded = true
					}
				}
			}
			if !bounded {
				t.Fatalf("the depth bound was not passed as a parameter")
			}
		})
	}
}

// The descendant walks use UNION rather than UNION ALL: on a tree they are
// identical, and on a cycle only one of them stops.
func TestDescendantWalksDedupe(t *testing.T) {
	for _, test := range []struct {
		op     uint32
		fields []string
	}{
		{opDelegationSpawnCountDescendants, []string{"d-1"}},
		{opDelegationSpawnCancelRecursive, []string{"1"}},
	} {
		db := newDelDB()
		db.row = []any{int64(0)}
		delCall(t, db, test.op, test.fields)
		for _, sql := range db.executed {
			if !strings.Contains(sql, "WITH RECURSIVE") {
				continue
			}
			if strings.Contains(sql, "UNION ALL") {
				t.Fatalf("a descendant walk uses UNION ALL, which never converges on a cycle:\n%s", sql)
			}
		}
	}
}

// --- the two races ---------------------------------------------------------------

// Both use RETURNING rather than a row count. The C's sqlite3_changes() was
// connection-global, so a worker racing on the shared connection could replace
// the count between the step and the read -- making a successful claim report
// failure, or a cancellation that happened report that it had not.
func TestLeaseAndCancelUseReturningNotARowCount(t *testing.T) {
	db := newDelDB()
	db.row = []any{int64(7)}
	status, _ := delCall(t, db, opAgentJobTakeLease, []string{"7", "worker-1"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if !strings.Contains(db.executed[0], "RETURNING id") {
		t.Fatalf("the lease does not use RETURNING: %s", db.executed[0])
	}
	if !strings.Contains(db.executed[0], "status = 'pending'") {
		t.Fatalf("the lease is not guarded on pending: %s", db.executed[0])
	}

	// A job somebody else already holds returns no row.
	db = newDelDB()
	if status, _ := delCall(t, db, opAgentJobTakeLease, []string{"7", "worker-2"}); status != store.StatusFailed {
		t.Fatalf("a lost lease: status = %d, want %d", status, store.StatusFailed)
	}

	db = newDelDB()
	db.row = []any{int64(7)}
	_, cells := delCall(t, db, opAgentJobCancelUnassigned, []string{"7", "reason", "60"})
	if cells[0] != "1" {
		t.Fatalf("cancel = %s, want 1", cells[0])
	}
	if !strings.Contains(db.executed[0], "RETURNING id") {
		t.Fatalf("the cancel does not use RETURNING: %s", db.executed[0])
	}

	db = newDelDB()
	_, cells = delCall(t, db, opAgentJobCancelUnassigned, []string{"7", "reason", "60"})
	if cells[0] != "0" {
		t.Fatalf("a lost cancel = %s, want 0", cells[0])
	}
}

// The unassigned cancel must only touch a job nobody has picked up: pending, or
// running with no agent named.
func TestCancelUnassignedOnlyTouchesUnclaimedJobs(t *testing.T) {
	db := newDelDB()
	db.row = []any{int64(7)}
	delCall(t, db, opAgentJobCancelUnassigned, []string{"7", "", "60"})
	sql := db.executed[0]
	if !strings.Contains(sql, "btrim(agent_name) = ''") {
		t.Fatalf("a running job with an agent could be cancelled: %s", sql)
	}
	if !strings.Contains(sql, "coalesce(heartbeat_at, created_at)") {
		t.Fatalf("a job that never beat has no age: %s", sql)
	}
	// The default reason is applied rather than storing an empty one.
	if db.args[0][1].(string) == "" {
		t.Fatalf("an empty reason was stored as empty")
	}
}

// --- no statement is built by string concatenation ---------------------------------

// Every one of these was snprintf'd in the C because a TEXT timestamp column
// gave it no interval arithmetic to do.
func TestAgeThresholdsAreBoundParametersNotSplicedText(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
		needle string
	}{
		{"spawn stale sweep", opDelegationSpawnCancelStale, nil, "make_interval(hours =>"},
		{"job stale sweep", opAgentJobCancelStale, []string{"3600", "reason"}, "make_interval(secs =>"},
		{"unassigned cancel", opAgentJobCancelUnassigned, []string{"7", "r", "3600"}, "make_interval(secs =>"},
		{"heartbeat staleness", opAgentJobHeartbeatIsStale, []string{"2026-08-22 09:00:00", "30"}, "make_interval(mins =>"},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newDelDB()
			db.row = []any{int64(7)}
			if test.op == opAgentJobHeartbeatIsStale {
				db.row = []any{true}
			}
			delCall(t, db, test.op, test.fields)
			sql := db.executed[0]
			if !strings.Contains(sql, test.needle) {
				t.Fatalf("the threshold is not an interval parameter: %s", sql)
			}
			for _, spliced := range []string{"3600", "30 minutes", "-24 hours"} {
				if strings.Contains(sql, spliced) {
					t.Fatalf("a value is spliced into the statement text (%q): %s", spliced, sql)
				}
			}
		})
	}

	// The learning injection's LIMIT was spliced too.
	db := newDelDB()
	delCall(t, db, opDelegateLearningInjectPrompt, []string{"reviewer", "prompt", "5"})
	if !strings.Contains(db.executed[0], "LIMIT $2") {
		t.Fatalf("the top-N limit is spliced: %s", db.executed[0])
	}
	if db.args[0][1].(int) != 5 {
		t.Fatalf("top-N bound as %v", db.args[0][1])
	}
}

// --- heartbeat staleness -----------------------------------------------------------

// A job that has never beaten has not gone quiet -- it has not started. Calling
// it stale would race the worker about to take its lease.
func TestAnAbsentHeartbeatIsNotStale(t *testing.T) {
	db := newDelDB()
	status, cells := delCall(t, db, opAgentJobHeartbeatIsStale, []string{"", "30"})
	if status != store.StatusOK || cells[0] != "0" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if len(db.executed) != 0 {
		t.Fatalf("an absent heartbeat was queried anyway")
	}

	db = newDelDB()
	db.row = []any{true}
	_, cells = delCall(t, db, opAgentJobHeartbeatIsStale, []string{"2026-08-22 09:00:00", "30"})
	if cells[0] != "1" {
		t.Fatalf("cells = %v", cells)
	}
}

// --- the reservation ----------------------------------------------------------------

// A deployment without the control plane's migrations has no reservation table.
// Reporting a miss keeps that degradation quiet and safe rather than failing
// every delegate launch.
func TestReservationDegradesWhenTheTableIsAbsent(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"get", opDelegateReservationGet, []string{"key:hash"}},
		{"adopt", opDelegateReservationAdopt, []string{"key:hash", "work-1"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newDelDB()
			db.present = false
			status, _ := delCall(t, db, test.op, test.fields)
			if status != store.StatusMissing {
				t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
			}
		})
	}
}

// A row whose job id is unusable is worse than no row: it would replay a launch
// that can never be polled.
func TestReservationTreatsAnUnusableJobIDAsAMiss(t *testing.T) {
	db := newDelDB()
	db.row = []any{int64(0), "token"}
	status, _ := delCall(t, db, opDelegateReservationGet, []string{"key:hash"})
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d", status, store.StatusMissing)
	}

	db = newDelDB()
	db.row = []any{int64(42), "token"}
	status, cells := delCall(t, db, opDelegateReservationGet, []string{"key:hash"})
	if status != store.StatusOK || cells[0] != "42" || cells[1] != "token" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

// The prefix is the key up to and including its last ':' -- the key without its
// content hash. Adoption only happens when that matches exactly one seat.
func TestAdoptComputesThePrefixAndRequiresExactlyOneSeat(t *testing.T) {
	db := newDelDB()
	db.row = []any{int64(9), "token"}
	status, cells := delCall(t, db, opDelegateReservationAdopt, []string{"repo:stage:abc123", "work-1"})
	if status != store.StatusOK || cells[0] != "9" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	// $3 is the prefix length, $4 the prefix itself.
	args := db.args[len(db.args)-1]
	if args[2].(int) != len("repo:stage:") {
		t.Fatalf("prefix length = %v, want %d", args[2], len("repo:stage:"))
	}
	if args[3].(string) != "repo:stage:" {
		t.Fatalf("prefix = %q", args[3])
	}
	// The cardinality check is what stops a grouped seat being guessed at.
	sql := db.executed[len(db.executed)-1]
	if !strings.Contains(sql, "1 = (") || !strings.Contains(sql, "count(*)") {
		t.Fatalf("the adoption has no cardinality check: %s", sql)
	}

	// No row back means there was no seat, or more than one. Both are "do not
	// adopt" rather than an error.
	db = newDelDB()
	if status, _ := delCall(t, db, opDelegateReservationAdopt, []string{"repo:stage:abc", "work-1"}); status != store.StatusMissing {
		t.Fatalf("status = %d, want %d", status, store.StatusMissing)
	}
}

func TestAdoptRefusesAKeyWithNoHashSeparator(t *testing.T) {
	for _, key := range []string{"nocolon", ":leading", "trailing:"} {
		db := newDelDB()
		status, _ := delCall(t, db, opDelegateReservationAdopt, []string{key, "work-1"})
		if status != store.StatusInvalid {
			t.Fatalf("key %q: status = %d, want invalid", key, status)
		}
		if len(db.executed) != 0 {
			t.Fatalf("key %q was queried anyway", key)
		}
	}
}

// --- the learning store ------------------------------------------------------------------

// Entries that have been acted on go first; only if the store is still over cap
// does a pending one -- which nobody has looked at yet -- get dropped.
func TestLearningEvictionPrefersActedOnEntries(t *testing.T) {
	t.Run("under cap evicts nothing", func(t *testing.T) {
		db := newDelDB()
		db.counts = []int64{10}
		delCall(t, db, opDelegateLearningRecord,
			[]string{"s", "role", "mode", "lesson", "{}", "0.5"})
		for _, sql := range db.executed {
			if strings.Contains(sql, "DELETE") {
				t.Fatalf("an under-cap store evicted: %s", sql)
			}
		}
	})

	t.Run("acted-on entries are enough", func(t *testing.T) {
		db := newDelDB()
		db.counts = []int64{learningCap + 5, learningCap}
		delCall(t, db, opDelegateLearningRecord,
			[]string{"s", "role", "mode", "lesson", "{}", "0.5"})
		var deletes [][]any
		for i, sql := range db.executed {
			if strings.Contains(sql, "DELETE") {
				deletes = append(deletes, db.args[i])
			}
		}
		if len(deletes) != 1 {
			t.Fatalf("%d evictions, want 1", len(deletes))
		}
		statuses := deletes[0][0].([]string)
		if len(statuses) != 2 || statuses[0] != "reviewed" || statuses[1] != "rejected" {
			t.Fatalf("evicted %v, want the acted-on statuses", statuses)
		}
		if deletes[0][1].(int64) != 5 {
			t.Fatalf("evicted %v rows, want the overage of 5", deletes[0][1])
		}
	})

	t.Run("still over cap drops pending", func(t *testing.T) {
		db := newDelDB()
		db.counts = []int64{learningCap + 5, learningCap + 3}
		delCall(t, db, opDelegateLearningRecord,
			[]string{"s", "role", "mode", "lesson", "{}", "0.5"})
		var evicted [][]string
		for i, sql := range db.executed {
			if strings.Contains(sql, "DELETE") {
				evicted = append(evicted, db.args[i][0].([]string))
			}
		}
		if len(evicted) != 2 {
			t.Fatalf("%d evictions, want 2", len(evicted))
		}
		if evicted[1][0] != "pending" {
			t.Fatalf("the second eviction took %v, want pending", evicted[1])
		}
	})
}

func TestLearningRecordValidatesConfidence(t *testing.T) {
	for _, confidence := range []string{"-0.1", "1.1", "high"} {
		db := newDelDB()
		status, _ := delCall(t, db, opDelegateLearningRecord,
			[]string{"s", "role", "mode", "lesson", "{}", confidence})
		if status != store.StatusInvalid {
			t.Fatalf("confidence %q: status = %d, want invalid", confidence, status)
		}
		if len(db.executed) != 0 {
			t.Fatalf("an invalid confidence ran statements")
		}
	}
}

// An empty evidence document is stored as {} rather than as nothing.
func TestLearningRecordDefaultsItsEvidence(t *testing.T) {
	db := newDelDB()
	db.counts = []int64{0}
	delCall(t, db, opDelegateLearningRecord, []string{"s", "role", "mode", "lesson", "", "0.5"})
	for i, sql := range db.executed {
		if strings.Contains(sql, "INSERT INTO delegate_learnings") {
			if db.args[i][4].(string) != "{}" {
				t.Fatalf("evidence stored as %q", db.args[i][4])
			}
			return
		}
	}
	t.Fatalf("no insert ran")
}

// --- spawn status questions ----------------------------------------------------------------

// A spawn that does not exist is not stopped, not cancelled and not active.
func TestSpawnFlagsAnswerZeroForAnUnknownSpawn(t *testing.T) {
	for _, op := range []uint32{
		opDelegationSpawnIsStopped, opDelegationSpawnIsCancelled, opDelegationSpawnIsActive,
	} {
		db := newDelDB()
		status, cells := delCall(t, db, op, []string{"d-unknown"})
		if status != store.StatusOK || cells[0] != "0" {
			t.Fatalf("op %d: status = %d, cells = %v", op, status, cells)
		}
	}
}

func TestSpawnFlagsClassifyStatusesCorrectly(t *testing.T) {
	for _, test := range []struct {
		status                       string
		stopped, cancelled, isActive string
	}{
		{"running", "0", "0", "1"},
		{"active", "0", "0", "1"},
		{"done", "1", "0", "0"},
		{"cancelled", "1", "1", "0"},
		{"preempted", "1", "0", "0"},
		{"failed", "1", "0", "0"},
	} {
		t.Run(test.status, func(t *testing.T) {
			for _, probe := range []struct {
				op   uint32
				want string
			}{
				{opDelegationSpawnIsStopped, test.stopped},
				{opDelegationSpawnIsCancelled, test.cancelled},
				{opDelegationSpawnIsActive, test.isActive},
			} {
				db := newDelDB()
				db.row = []any{test.status}
				_, cells := delCall(t, db, probe.op, []string{"d-1"})
				if cells[0] != probe.want {
					t.Fatalf("op %d on %q = %s, want %s", probe.op, test.status, cells[0], probe.want)
				}
			}
		})
	}
}

// A spawn that is still running has no stop to give a reason for.
func TestStopReasonIsMissingWhileRunning(t *testing.T) {
	db := newDelDB()
	db.row = []any{"running"}
	if status, _ := delCall(t, db, opDelegationSpawnStopReason, []string{"d-1"}); status != store.StatusMissing {
		t.Fatalf("status = %d, want %d", status, store.StatusMissing)
	}

	db = newDelDB()
	db.row = []any{"cancelled"}
	status, cells := delCall(t, db, opDelegationSpawnStopReason, []string{"d-1"})
	if status != store.StatusOK || cells[0] != "cancelled" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

// Completion and cancellation only act on a live spawn, so a finished one is
// not re-finished with a new timestamp.
func TestSpawnTransitionsOnlyActOnLiveSpawns(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"complete", opDelegationSpawnComplete, []string{"d-1"}},
		{"preempt", opDelegationSpawnPreempt, []string{"d-1"}},
		{"cancel by id", opDelegationSpawnCancelByID, []string{"1"}},
		{"cancel recursive", opDelegationSpawnCancelRecursive, []string{"1"}},
		{"cancel stale", opDelegationSpawnCancelStale, nil},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newDelDB()
			delCall(t, db, test.op, test.fields)
			sql := db.executed[len(db.executed)-1]
			if !strings.Contains(sql, "status = ANY(") {
				t.Fatalf("the transition is not guarded on a live status: %s", sql)
			}
			var states []string
			for _, args := range db.args {
				for _, a := range args {
					if s, ok := a.([]string); ok {
						states = s
					}
				}
			}
			if len(states) != 2 || states[0] != "active" || states[1] != "running" {
				t.Fatalf("guarded on %v, want the live spawn states", states)
			}
		})
	}
}

// --- validation -------------------------------------------------------------------------------

func TestDelegationValidatesItsArguments(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"message with no delegation", opDelegationMessageRecord, []string{"", "in", "x"}},
		{"spawn with a bad depth", opDelegationSpawnRecord, []string{"d", "", "s", "deep", "r"}},
		{"spawn with a negative depth", opDelegationSpawnRecord, []string{"d", "", "s", "-1", "r"}},
		{"status with no delegation", opDelegationSpawnStatus, []string{""}},
		{"list active max zero", opDelegationSpawnListActive, []string{"0"}},
		{"list active max too large", opDelegationSpawnListActive, []string{"99999"}},
		{"cancel with id zero", opDelegationSpawnCancelByID, []string{"0"}},
		{"reservation get with no key", opDelegateReservationGet, []string{""}},
		{"reservation save with a bad job", opDelegateReservationSave, []string{"k", "no", "w", "t"}},
		{"checkpoint save with no delegation", opDelegationCheckpointSave, []string{"", "j", "[]", "o", "e", "0"}},
		{"job update with id zero", opAgentJobUpdate, []string{"0", "s", "c", "r"}},
		{"job complete with a bad cost", opAgentJobComplete, []string{"1", "s", "r", "0", "free", "0"}},
		{"job list max too large", opAgentJobListRecent, []string{"", "99999"}},
		{"log list max zero", opAgentLogEntryList, []string{"", "0"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newDelDB()
			status, _ := delCall(t, db, test.op, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid request ran %d statements", len(db.executed))
			}
		})
	}
}

// The agent-job row is sixteen cells wide.
func TestAgentJobRowWidth(t *testing.T) {
	if len(agentJobCols) != 16 {
		t.Fatalf("the agent job row has %d columns, want 16", len(agentJobCols))
	}
	row := make([]any, 16)
	for i, c := range agentJobCols {
		switch c.kind {
		case kInt:
			row[i] = int64(1)
		case kFloat:
			row[i] = 1.5
		default:
			row[i] = "x"
		}
	}
	db := newDelDB()
	db.row = row
	status, cells := delCall(t, db, opAgentJobGet, []string{"1"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(cells) != 16 {
		t.Fatalf("%d cells, want 16", len(cells))
	}
}
