package families

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The eval-candidate and approach-memory operations against a real PostgreSQL.
//
// Every other test in this package runs the handlers over a stub, which proves
// the wiring and never executes a statement. These ten operations were ported
// from SQLite C, and the parts most likely to be wrong are the parts a stub
// cannot reach: the jsonb session set, ON CONFLICT arithmetic, the transition
// guards, and byte-counted CHECK constraints.
//
// Set AIMEE_TEST_PG_URL to a database this test may DROP TABLE in. Without it
// the test skips, so it is coverage only where a database is actually offered.
//
// The database must be UTF8. In a SQL_ASCII cluster octet_length and
// char_length are the same function, so the byte-counted constraints would pass
// there whether or not they count the right unit.

func livePool(t *testing.T) *pgxpool.Pool {
	t.Helper()
	url := os.Getenv("AIMEE_TEST_PG_URL")
	if url == "" {
		t.Skip("AIMEE_TEST_PG_URL not set")
	}
	pool, err := pgxpool.New(context.Background(), url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	t.Cleanup(pool.Close)

	var encoding string
	if err := pool.QueryRow(context.Background(),
		`SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = current_database()`).
		Scan(&encoding); err != nil {
		t.Fatalf("read encoding: %v", err)
	}
	if encoding != "UTF8" {
		t.Fatalf("database encoding is %s, want UTF8: a byte-counted CHECK "+
			"cannot be distinguished from a character-counted one in SQL_ASCII", encoding)
	}
	return pool
}

// liveQueryer adapts a pgx pool to the interface families are written against.
// Production satisfies that interface with a bus client instead; here the point
// is to run the SQL, so the pool is the shortest path to a real planner.
type liveQueryer struct{ pool *pgxpool.Pool }

func (q liveQueryer) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	tag, err := q.pool.Exec(ctx, sql, args...)
	if err != nil {
		return nil, err
	}
	return store.RowsAffected(tag.RowsAffected()), nil
}

func (q liveQueryer) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	rows, err := q.pool.Query(ctx, sql, args...)
	if err != nil {
		return nil, err
	}
	return liveRows{rows}, nil
}

func (q liveQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return q.pool.QueryRow(ctx, sql, args...)
}

type liveRows struct {
	inner interface {
		Next() bool
		Scan(...any) error
		Err() error
		Close()
	}
}

func (r liveRows) Next() bool             { return r.inner.Next() }
func (r liveRows) Scan(dest ...any) error { return r.inner.Scan(dest...) }
func (r liveRows) Err() error             { return r.inner.Err() }
func (r liveRows) Close()                 { r.inner.Close() }

// freshSchema drops and recreates the two tables migration 22 introduces, plus
// eval_results, which the ablation grid reads. Taken from the migration itself
// rather than restated here: a test carrying its own copy of the DDL proves the
// copy works, not the schema that ships.
func freshSchema(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	ctx := context.Background()
	if _, err := pool.Exec(ctx,
		`DROP TABLE IF EXISTS eval_candidates, approach_failures, eval_results`); err != nil {
		t.Fatalf("drop: %v", err)
	}
	all, err := Migrations()
	if err != nil {
		t.Fatalf("migrations: %v", err)
	}
	for _, m := range all {
		if m.Name != "schema_eval_candidates.sql" && m.Name != "schema_telemetry.sql" {
			continue
		}
		for _, stmt := range m.Statements {
			if _, err := pool.Exec(ctx, stmt); err != nil {
				// schema_telemetry.sql carries tables beyond eval_results whose
				// dependencies are not loaded here; only eval_results matters.
				if m.Name == "schema_telemetry.sql" {
					continue
				}
				t.Fatalf("%s: %v", m.Name, err)
			}
		}
	}
	var n int
	if err := pool.QueryRow(ctx,
		`SELECT count(*) FROM information_schema.tables
		  WHERE table_name IN ('eval_candidates','approach_failures','eval_results')`).Scan(&n); err != nil {
		t.Fatalf("verify: %v", err)
	}
	if n != 3 {
		t.Fatalf("expected 3 tables after migration, found %d", n)
	}
}

// run dispatches through the family's op table, so a handler registered under
// the wrong number or with the wrong arg count fails here rather than passing a
// direct call and failing on the wire.
func run(t *testing.T, q store.Queryer, op uint32, fields ...string) (uint32, []string) {
	t.Helper()
	entry, ok := Telemetry.Ops[op]
	if !ok {
		t.Fatalf("op %d is not registered", op)
	}
	if entry.Args != len(fields) {
		t.Fatalf("op %d (%s) takes %d args, test passed %d",
			op, entry.Name, entry.Args, len(fields))
	}
	code, cells, err := entry.Run(context.Background(), q, fields)
	if err != nil {
		t.Fatalf("op %d (%s): %v", op, entry.Name, err)
	}
	return code, cells
}

func TestEvalCandidateObserveCountsDistinctReporters(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	const sig = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
	observe := func(session string) {
		t.Helper()
		if code, _ := run(t, q, 49, sig, "regressions", "task-one", `{"k":1}`,
			"scan", "ref-1", session); code != store.StatusOK {
			t.Fatalf("observe(%q): status %d", session, code)
		}
	}

	read := func() (occurrences, distinct string) {
		t.Helper()
		code, cells := run(t, q, 50, sig)
		if code != store.StatusOK {
			t.Fatalf("get: status %d", code)
		}
		if len(cells) != evalCandidateCells {
			t.Fatalf("get returned %d cells, want %d", len(cells), evalCandidateCells)
		}
		return cells[8], cells[9]
	}

	observe("session-a")
	if occ, dis := read(); occ != "1" || dis != "1" {
		t.Fatalf("after first observation: occurrences=%s distinct=%s, want 1 and 1", occ, dis)
	}

	// The same session reporting again is the same observation -- a re-run scan
	// over the same ledger -- and must not inflate the count.
	observe("session-a")
	if occ, dis := read(); occ != "1" || dis != "1" {
		t.Fatalf("re-observing from one session: occurrences=%s distinct=%s, want 1 and 1", occ, dis)
	}

	observe("session-b")
	if occ, dis := read(); occ != "2" || dis != "2" {
		t.Fatalf("after a second reporter: occurrences=%s distinct=%s, want 2 and 2", occ, dis)
	}

	// No session id is no identity to deduplicate on, so it always counts --
	// and retains nothing, so the distinct set does not move.
	observe("")
	if occ, dis := read(); occ != "3" || dis != "2" {
		t.Fatalf("anonymous observation: occurrences=%s distinct=%s, want 3 and 2", occ, dis)
	}
}

func TestEvalCandidateSessionSetStopsAtTheCap(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	const sig = "b1b2c3d4e5f60718293a4b5c6d7e8f90"
	for _, s := range []string{"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10"} {
		if code, _ := run(t, q, 49, sig, "", "", "", "", "", s); code != store.StatusOK {
			t.Fatalf("observe(%s): status %d", s, code)
		}
	}
	_, cells := run(t, q, 50, sig)
	if cells[9] != "8" {
		t.Fatalf("retained sessions = %s, want the cap of 8", cells[9])
	}
	// Past the cap the set stops growing but the reporter still counted: ten
	// distinct sessions reported, so occurrences is ten.
	if cells[8] != "10" {
		t.Fatalf("occurrences = %s, want 10 -- a session past the cap still counts", cells[8])
	}
}

func TestEvalCandidateTransitionsRefuseIllegalMoves(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	id := func(sig string) string {
		t.Helper()
		if code, _ := run(t, q, 49, sig, "", "", "", "", "", "s1"); code != store.StatusOK {
			t.Fatal("observe failed")
		}
		_, cells := run(t, q, 50, sig)
		return cells[0]
	}

	t.Run("admitted then archived", func(t *testing.T) {
		row := id("c1b2c3d4e5f60718293a4b5c6d7e8f90")
		if code, _ := run(t, q, 52, row, "operator-a", "/suites/x.json"); code != store.StatusOK {
			t.Fatalf("admit: status %d", code)
		}
		if code, _ := run(t, q, 54, row); code != store.StatusOK {
			t.Fatalf("archive: status %d", code)
		}
		// Archived is not admitted, so archiving again has nothing to move.
		if code, _ := run(t, q, 54, row); code != store.StatusInvalid {
			t.Fatalf("second archive: status %d, want invalid", code)
		}
	})

	t.Run("rejected is terminal", func(t *testing.T) {
		row := id("d1b2c3d4e5f60718293a4b5c6d7e8f90")
		if code, _ := run(t, q, 53, row, "not reproducible"); code != store.StatusOK {
			t.Fatalf("reject: status %d", code)
		}
		if code, _ := run(t, q, 52, row, "operator-b", "/suites/y.json"); code != store.StatusInvalid {
			t.Fatalf("admitting a rejected candidate: status %d, want invalid", code)
		}
		// Observing it again bumps the counter and leaves the verdict alone.
		if code, _ := run(t, q, 49, "d1b2c3d4e5f60718293a4b5c6d7e8f90",
			"", "", "", "", "", "s2"); code != store.StatusOK {
			t.Fatalf("re-observe: status %d", code)
		}
		_, cells := run(t, q, 50, "d1b2c3d4e5f60718293a4b5c6d7e8f90")
		if cells[2] != "rejected" {
			t.Fatalf("state after re-observation = %q, want rejected", cells[2])
		}
		if cells[12] != "not reproducible" {
			t.Fatalf("reject_reason = %q, want it preserved", cells[12])
		}
	})

	t.Run("two operators race to admit", func(t *testing.T) {
		row := id("e1b2c3d4e5f60718293a4b5c6d7e8f90")
		if code, _ := run(t, q, 52, row, "operator-a", "/a.json"); code != store.StatusOK {
			t.Fatalf("first admit: status %d", code)
		}
		// The second requires state 'candidate', which the first consumed.
		if code, _ := run(t, q, 52, row, "operator-b", "/b.json"); code != store.StatusInvalid {
			t.Fatalf("second admit: status %d, want invalid", code)
		}
		_, cells := run(t, q, 50, "e1b2c3d4e5f60718293a4b5c6d7e8f90")
		if cells[10] != "operator-a" {
			t.Fatalf("admitted_by = %q, want the winner of the race", cells[10])
		}
	})

	t.Run("unknown id", func(t *testing.T) {
		if code, _ := run(t, q, 52, "999999", "op", "/p"); code != store.StatusInvalid {
			t.Fatalf("admitting a row that does not exist: status %d, want invalid", code)
		}
	})
}

func TestEvalCandidateGetReportsMissing(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	if code, cells := run(t, q, 50, "00000000000000000000000000000000"); code != store.StatusMissing {
		t.Fatalf("status %d with %d cells, want missing", code, len(cells))
	}
}

func TestEvalCandidateListFiltersAndBounds(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	for _, sig := range []string{
		"11111111111111111111111111111111",
		"22222222222222222222222222222222",
		"33333333333333333333333333333333",
	} {
		run(t, q, 49, sig, "", "", "", "", "", "s1")
	}
	_, cells := run(t, q, 50, "22222222222222222222222222222222")
	run(t, q, 53, cells[0], "no")

	code, all := run(t, q, 51, "", "10")
	if code != store.StatusOK || len(all) != 3*evalCandidateCells {
		t.Fatalf("unfiltered list: status %d, %d cells, want 3 rows", code, len(all))
	}
	_, rejected := run(t, q, 51, "rejected", "10")
	if len(rejected) != evalCandidateCells {
		t.Fatalf("state filter returned %d cells, want one row", len(rejected))
	}

	// The contract declares max_rows 128; a caller asking for more is asking
	// for a reply it has not reserved room for.
	if code, _ := run(t, q, 51, "", "129"); code != store.StatusInvalid {
		t.Fatalf("max=129: status %d, want invalid", code)
	}
	if code, _ := run(t, q, 51, "", "0"); code != store.StatusInvalid {
		t.Fatalf("max=0: status %d, want invalid", code)
	}
}

func TestApproachFailureRecordAccumulatesOnThePair(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	const goal = "9f9f9f9f9f9f9f9f9f9f9f9f9f9f9f9f"
	const approach = "8e8e8e8e8e8e8e8e8e8e8e8e8e8e8e8e"

	if code, _ := run(t, q, 57, goal, "make the build reproducible", "build_id reproducible",
		approach, "pin the timestamp", "timestamps still differ", "agent", "run-1"); code != store.StatusOK {
		t.Fatalf("record: status %d", code)
	}
	// Meeting the same dead end again is more evidence for one row.
	if code, _ := run(t, q, 57, goal, "make the build reproducible", "build_id reproducible",
		approach, "pin the timestamp", "the archive index differs too", "agent", "run-2"); code != store.StatusOK {
		t.Fatalf("second record: status %d", code)
	}

	code, cells := run(t, q, 58, "", "10")
	if code != store.StatusOK {
		t.Fatalf("candidates: status %d", code)
	}
	if len(cells) != approachFailureCells {
		t.Fatalf("got %d cells, want one row of %d", len(cells), approachFailureCells)
	}
	if cells[9] != "2" {
		t.Fatalf("occurrences = %s, want 2", cells[9])
	}
	// The newest failure mode wins: it is what this approach did most recently.
	if cells[6] != "the archive index differs too" {
		t.Fatalf("failure_mode = %q, want the latest", cells[6])
	}

	// Both halves of the identity are required.
	if code, _ := run(t, q, 57, "", "g", "t", approach, "a", "m", "s", "r"); code != store.StatusInvalid {
		t.Fatalf("no goal signature: status %d, want invalid", code)
	}
	if code, _ := run(t, q, 57, goal, "g", "t", "", "a", "m", "s", "r"); code != store.StatusInvalid {
		t.Fatalf("no approach signature: status %d, want invalid", code)
	}
}

// The C built the LIKE pattern by concatenation with no escaping. These are goal
// TOKENS -- identifiers, where underscores are common -- so an unescaped _
// matched any character in that position and returned rows the caller never
// asked about.
func TestApproachFailureSearchTreatsWildcardsAsText(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	rows := []struct{ goal, tokens string }{
		{"a0000000000000000000000000000001", "build_id reproducible"},
		{"a0000000000000000000000000000002", "buildXid reproducible"},
	}
	for i, r := range rows {
		if code, _ := run(t, q, 57, r.goal, "", r.tokens,
			"b000000000000000000000000000000"+string(rune('1'+i)),
			"", "", "", ""); code != store.StatusOK {
			t.Fatalf("record %d: status %d", i, code)
		}
	}

	_, cells := run(t, q, 58, "build_id", "10")
	if len(cells) != approachFailureCells {
		t.Fatalf("searching for build_id returned %d rows, want exactly the one "+
			"whose tokens contain that literal", len(cells)/approachFailureCells)
	}
	if !strings.Contains(cells[3], "build_id") {
		t.Fatalf("matched the wrong row: tokens = %q", cells[3])
	}
}

func TestAblationGridAggregatesResults(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}
	ctx := context.Background()

	for _, r := range []struct {
		suite, task, ablation string
		success               bool
	}{
		{"regressions", "task-a", "full", true},
		{"regressions", "task-a", "full", false},
		{"regressions", "task-a", "no-memory", false},
		{"regressions", "task-b", "full", true},
		{"other", "task-c", "full", true},
		// task_name '' is excluded: a grid cell with no task names nothing.
		{"regressions", "", "full", true},
	} {
		if _, err := pool.Exec(ctx,
			`INSERT INTO eval_results (suite, task_name, ablation, success) VALUES ($1,$2,$3,$4)`,
			r.suite, r.task, r.ablation, r.success); err != nil {
			t.Fatalf("seed: %v", err)
		}
	}

	code, cells := run(t, q, 56, "regressions", "100")
	if code != store.StatusOK {
		t.Fatalf("grid: status %d", code)
	}
	got := map[string]string{}
	for i := 0; i+ablationCellCells <= len(cells); i += ablationCellCells {
		got[cells[i]+"/"+cells[i+1]] = cells[i+2] + " of " + cells[i+3]
	}
	want := map[string]string{
		"task-a/full":      "1 of 2",
		"task-a/no-memory": "0 of 1",
		"task-b/full":      "1 of 1",
	}
	if len(got) != len(want) {
		t.Fatalf("grid has %d cells (%v), want %d", len(got), got, len(want))
	}
	for k, v := range want {
		if got[k] != v {
			t.Fatalf("cell %s = %q, want %q", k, got[k], v)
		}
	}
}

func TestEvalCandidateSetPassingWindows(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	q := liveQueryer{pool}

	const sig = "f1b2c3d4e5f60718293a4b5c6d7e8f90"
	run(t, q, 49, sig, "", "", "", "", "", "s1")
	_, cells := run(t, q, 50, sig)
	row := cells[0]

	if code, _ := run(t, q, 55, row, "3"); code != store.StatusOK {
		t.Fatalf("set windows: status %d", code)
	}
	_, cells = run(t, q, 50, sig)
	if cells[13] != "3" {
		t.Fatalf("passing_windows = %s, want 3", cells[13])
	}
	if code, _ := run(t, q, 55, row, "-1"); code != store.StatusInvalid {
		t.Fatalf("negative windows: status %d, want invalid", code)
	}
}

// Migration 22 caps its text columns in bytes, matching the C buffers that read
// the values back. A multi-byte value inside the character count but over the
// byte count is the case that separates the two.
func TestEvalCandidateColumnCapsCountBytes(t *testing.T) {
	pool := livePool(t)
	freshSchema(t, pool)
	ctx := context.Background()

	// reject_reason caps at 255 bytes. 200 three-byte characters is 200
	// characters and 600 bytes: accepted by a character count, refused here.
	wide := strings.Repeat("世", 200)
	_, err := pool.Exec(ctx,
		`INSERT INTO eval_candidates (signature, reject_reason) VALUES ($1, $2)`,
		"aaaa0000000000000000000000000001", wide)
	if err == nil {
		t.Fatal("a 600-byte reject_reason was accepted; the CHECK is counting characters")
	}
	if !strings.Contains(err.Error(), "check constraint") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}

	// The same column holds a value at the limit measured in bytes.
	if _, err := pool.Exec(ctx,
		`INSERT INTO eval_candidates (signature, reject_reason) VALUES ($1, $2)`,
		"aaaa0000000000000000000000000002", strings.Repeat("x", 255)); err != nil {
		t.Fatalf("255 bytes should fit: %v", err)
	}
}
