package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a checkpoint-shaped fake ------------------------------------------------

type checkpoint struct {
	id        int64
	taskID    int64
	session   string
	label     string
	snapshot  string
	createdAt string
}

func (c checkpoint) scanInto(dest []any) {
	*(dest[0].(*int64)) = c.id
	*(dest[1].(*int64)) = c.taskID
	*(dest[2].(*string)) = c.session
	*(dest[3].(*string)) = c.label
	*(dest[4].(*string)) = c.snapshot
	*(dest[5].(*string)) = c.createdAt
}

type cpRow struct {
	row     *checkpoint
	scanErr error
}

func (r cpRow) Scan(dest ...any) error {
	if r.scanErr != nil {
		return r.scanErr
	}
	if r.row == nil {
		return store.ErrNoRows
	}
	r.row.scanInto(dest)
	return nil
}

// cpRows is the minimum of store.Rows the list path uses.
type cpRows struct {
	rows    []checkpoint
	at      int
	iterErr error
	scanErr error
}

func (r *cpRows) Close()     {}
func (r *cpRows) Err() error { return r.iterErr }
func (r *cpRows) Next() bool {
	if r.at >= len(r.rows) {
		return false
	}
	r.at++
	return true
}
func (r *cpRows) Scan(dest ...any) error {
	if r.scanErr != nil {
		return r.scanErr
	}
	r.rows[r.at-1].scanInto(dest)
	return nil
}

type cpDB struct {
	row      *checkpoint
	list     []checkpoint
	scanErr  error
	queryErr error
	iterErr  error
	execRows int

	executed []string
	args     [][]any
}

func newCPDB() *cpDB { return &cpDB{execRows: 1} }

func (d *cpDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return store.RowsAffected(d.execRows), nil
}
func (d *cpDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	if d.queryErr != nil {
		return nil, d.queryErr
	}
	return &cpRows{rows: d.list, iterErr: d.iterErr, scanErr: d.scanErr}, nil
}
func (d *cpDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return cpRow{row: d.row, scanErr: d.scanErr}
}
func (d *cpDB) Begin(context.Context) (store.Tx, error) { return cpTx{d}, nil }

type cpTx struct{ *cpDB }

func (t cpTx) Commit(context.Context) error   { return nil }
func (t cpTx) Rollback(context.Context) error { return nil }

func sample(id int64) checkpoint {
	return checkpoint{id: id, taskID: 7, session: "sess", label: "before-edit",
		snapshot: `{"n":1}`, createdAt: "2026-08-22 09:00:00"}
}

func cpCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := Checkpoints.Handler(db)(
		bus.ModuleInvocation{StageID: StageCheckpoints}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// --- insert -------------------------------------------------------------------

// One statement, not three. The C inserted, read last_insert_rowid(), then
// SELECTed the row back -- a rowid that is only correct because nothing else
// was writing on that connection.
func TestInsertReturnsTheStoredRowInOneStatement(t *testing.T) {
	db := newCPDB()
	db.row = &checkpoint{id: 42, taskID: 7, session: "sess", label: "before-edit",
		snapshot: `{"n":1}`, createdAt: "2026-08-22 09:00:00"}
	status, cells := cpCall(t, db, opCheckpointInsert,
		[]string{"before-edit", "sess", "7", `{"n":1}`})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	want := []string{"42", "7", "sess", "before-edit", `{"n":1}`, "2026-08-22 09:00:00"}
	for i := range want {
		if cells[i] != want[i] {
			t.Fatalf("cell %d = %q, want %q", i, cells[i], want[i])
		}
	}
	if len(db.executed) != 1 {
		t.Fatalf("insert ran %d statements: %v", len(db.executed), db.executed)
	}
	if !strings.Contains(db.executed[0], "RETURNING") {
		t.Fatalf("the insert does not RETURNING: %s", db.executed[0])
	}
	if strings.Contains(strings.ToLower(db.executed[0]), "lastval") ||
		strings.Contains(strings.ToLower(db.executed[0]), "currval") {
		t.Fatalf("the insert reads a sequence back instead of RETURNING")
	}
}

func TestInsertRefusesABlankLabelOrBadTaskID(t *testing.T) {
	for _, test := range []struct {
		name   string
		fields []string
	}{
		{"blank label", []string{"", "sess", "7", "{}"}},
		{"task id not a number", []string{"lbl", "sess", "seven", "{}"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newCPDB()
			status, _ := cpCall(t, db, opCheckpointInsert, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid insert ran %d statements", len(db.executed))
			}
		})
	}
}

// The wire carries created_at as the same 'YYYY-MM-DD HH:MM:SS' UTC spelling
// SQLite's datetime('now') produced, formatted from a TIMESTAMPTZ column.
func TestCreatedAtIsFormattedAtTheBoundary(t *testing.T) {
	db := newCPDB()
	db.row = &checkpoint{id: 1, createdAt: "2026-08-22 09:00:00", label: "l"}
	cpCall(t, db, opCheckpointInsert, []string{"l", "", "0", "{}"})
	if !strings.Contains(db.executed[0], "to_char(created_at AT TIME ZONE 'UTC'") {
		t.Fatalf("created_at is not formatted at the boundary: %s", db.executed[0])
	}
}

// --- get ----------------------------------------------------------------------

func TestGetDistinguishesMissingFromFailed(t *testing.T) {
	status, cells := cpCall(t, newCPDB(), opCheckpointGet, []string{"5"})
	if status != store.StatusMissing {
		t.Fatalf("no row: status = %d, want %d (missing)", status, store.StatusMissing)
	}
	if len(cells) != 0 {
		t.Fatalf("a miss carried %d cells", len(cells))
	}

	db := newCPDB()
	db.scanErr = errors.New("store broke")
	if status, _ = cpCall(t, db, opCheckpointGet, []string{"5"}); status != store.StatusFailed {
		t.Fatalf("broken postgres: status = %d, want %d", status, store.StatusFailed)
	}

	db = newCPDB()
	row := sample(5)
	db.row = &row
	status, cells = cpCall(t, db, opCheckpointGet, []string{"5"})
	if status != store.StatusOK || len(cells) != 6 || cells[0] != "5" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

func TestGetRefusesANonNumericID(t *testing.T) {
	db := newCPDB()
	status, _ := cpCall(t, db, opCheckpointGet, []string{"latest"})
	if status != store.StatusInvalid {
		t.Fatalf("status = %d, want %d", status, store.StatusInvalid)
	}
	if len(db.executed) != 0 {
		t.Fatalf("a non-numeric id was queried")
	}
}

// --- list ---------------------------------------------------------------------

// A list reply carries no row count -- the caller divides by the row width -- so
// the cells must be a whole number of six-cell rows.
func TestListEmitsWholeRows(t *testing.T) {
	db := newCPDB()
	db.list = []checkpoint{sample(3), sample(2), sample(1)}
	status, cells := cpCall(t, db, opCheckpointList, []string{"10", "10"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != 18 {
		t.Fatalf("cells = %d, want 18 (3 rows of 6)", len(cells))
	}
	if len(cells)%6 != 0 {
		t.Fatalf("cells = %d, not a whole number of rows", len(cells))
	}
	if cells[0] != "3" || cells[6] != "2" || cells[12] != "1" {
		t.Fatalf("row order = %s %s %s", cells[0], cells[6], cells[12])
	}
}

// An empty list is OK with no cells, not MISSING: the caller asked what was
// there and the answer was nothing.
func TestEmptyListIsOKNotMissing(t *testing.T) {
	status, cells := cpCall(t, newCPDB(), opCheckpointList, []string{"10", "10"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != 0 {
		t.Fatalf("cells = %v", cells)
	}
}

func TestListValidatesItsBounds(t *testing.T) {
	for _, test := range []struct {
		name   string
		fields []string
	}{
		{"max zero", []string{"10", "0"}},
		{"max negative", []string{"10", "-1"}},
		{"max above the ceiling", []string{"10", "65"}},
		{"max not a number", []string{"10", "all"}},
		{"limit not a number", []string{"some", "10"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newCPDB()
			status, _ := cpCall(t, db, opCheckpointList, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an out-of-bounds list queried anyway")
			}
		})
	}
}

// A limit at or above max is capped rather than refused, matching the C, and a
// non-positive limit means "as many as max allows".
func TestListCapsTheLimitToMax(t *testing.T) {
	for _, test := range []struct {
		name   string
		fields []string
		want   int
	}{
		{"limit below max", []string{"3", "10"}, 3},
		{"limit above max is capped", []string{"50", "10"}, 10},
		{"limit zero means max", []string{"0", "10"}, 10},
		{"limit negative means max", []string{"-5", "10"}, 10},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newCPDB()
			cpCall(t, db, opCheckpointList, test.fields)
			if len(db.args) != 1 || len(db.args[0]) != 1 {
				t.Fatalf("args = %v", db.args)
			}
			if got := db.args[0][0].(int); got != test.want {
				t.Fatalf("bound limit %d, want %d", got, test.want)
			}
		})
	}
}

// The ordering has to be deterministic: created_at alone is not unique, so a
// LIMIT over it could return either of two checkpoints taken in the same
// instant, and a caller paging through them could see one twice.
func TestListOrderingIsTotal(t *testing.T) {
	db := newCPDB()
	cpCall(t, db, opCheckpointList, []string{"10", "10"})
	sql := db.executed[0]
	if !strings.Contains(sql, "ORDER BY created_at DESC, id DESC") {
		t.Fatalf("list ordering is not total: %s", sql)
	}
}

// A failure part-way through the rows is reported, not silently returned as a
// short list -- which on this wire is indistinguishable from a complete one.
func TestListReportsAnIterationFailureRatherThanTruncating(t *testing.T) {
	db := newCPDB()
	db.list = []checkpoint{sample(3), sample(2)}
	db.iterErr = errors.New("connection lost mid-scan")
	status, cells := cpCall(t, db, opCheckpointList, []string{"10", "10"})
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}
	if len(cells) != 0 {
		t.Fatalf("a failed list carried %d cells", len(cells))
	}

	db = newCPDB()
	db.queryErr = errors.New("query refused")
	if status, _ = cpCall(t, db, opCheckpointList, []string{"10", "10"}); status != store.StatusFailed {
		t.Fatalf("query failure: status = %d, want %d", status, store.StatusFailed)
	}
}

// --- delete -------------------------------------------------------------------

// Deleting a checkpoint that is not there is a failure: the caller named a
// specific id, and nothing having happened is not what it asked for.
func TestDeleteOfAMissingCheckpointFails(t *testing.T) {
	db := newCPDB()
	db.execRows = 0
	status, _ := cpCall(t, db, opCheckpointDelete, []string{"5"})
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}

	db = newCPDB()
	if status, _ = cpCall(t, db, opCheckpointDelete, []string{"5"}); status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
}
