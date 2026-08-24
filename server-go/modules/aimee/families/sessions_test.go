package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a sessions-shaped fake ---------------------------------------------------

type sessRow struct {
	db     *sessDB
	sql    string
	values []any
}

func (r sessRow) Scan(dest ...any) error {
	if r.db.scanErr != nil {
		return r.db.scanErr
	}
	if r.values == nil {
		return store.ErrNoRows
	}
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = r.values[i].(string)
		case *int64:
			*p = r.values[i].(int64)
		case *int:
			*p = r.values[i].(int)
		}
	}
	return nil
}

type sessRows struct {
	rows [][]any
	at   int
}

func (r *sessRows) Close()     {}
func (r *sessRows) Err() error { return nil }
func (r *sessRows) Next() bool {
	if r.at >= len(r.rows) {
		return false
	}
	r.at++
	return true
}
func (r *sessRows) Scan(dest ...any) error {
	row := r.rows[r.at-1]
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = row[i].(string)
		case *int64:
			*p = row[i].(int64)
		}
	}
	return nil
}

type sessDB struct {
	// row is returned by the next QueryRow that is not a persona-state or
	// ownership probe; those have their own fields so a test can set them
	// independently.
	row          []any
	rows         [][]any
	personaState *int
	ownedByOther bool
	scanErr      error
	execRows     int

	executed []string
	args     [][]any
}

func newSessDB() *sessDB { return &sessDB{execRows: 1} }

func (d *sessDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return store.RowsAffected(d.execRows), nil
}
func (d *sessDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return &sessRows{rows: d.rows}, nil
}
func (d *sessDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	switch {
	case strings.Contains(sql, "SELECT persona_delivery_state"):
		if d.personaState == nil {
			return sessRow{db: d, sql: sql}
		}
		return sessRow{db: d, sql: sql, values: []any{*d.personaState}}
	case strings.Contains(sql, "SELECT 1 FROM webchat_claude_sessions"):
		if !d.ownedByOther {
			return sessRow{db: d, sql: sql}
		}
		return sessRow{db: d, sql: sql, values: []any{1}}
	}
	return sessRow{db: d, sql: sql, values: d.row}
}
func (d *sessDB) Begin(context.Context) (store.Tx, error) { return sessTx{d}, nil }

type sessTx struct{ *sessDB }

func (t sessTx) Commit(context.Context) error   { return nil }
func (t sessTx) Rollback(context.Context) error { return nil }

func sessCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

func ran(db *sessDB, fragment string) bool {
	for _, sql := range db.executed {
		if strings.Contains(sql, fragment) {
			return true
		}
	}
	return false
}

// --- persona delivery: the part that must be race-free ------------------------

// The claim is a guarded UPDATE, not a read then a write. Two connections
// opening at the same instant both pass a read; only one changes a row.
func TestPersonaClaimIsAGuardedUpdate(t *testing.T) {
	db := newSessDB()
	status, cells := sessCall(t, db, opPersonaDeliveryClaim, []string{"sess"})
	if status != store.StatusOK || cells[0] != "1" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if len(db.executed) != 1 {
		t.Fatalf("the claim read before writing: %v", db.executed)
	}
	sql := db.executed[0]
	if !strings.Contains(sql, "persona_delivery_state = 0") {
		t.Fatalf("the claim is not guarded on unclaimed: %s", sql)
	}
}

// Losing the race and there being no such session are different answers: the
// first is "already handled", the second is a failure.
func TestPersonaClaimDistinguishesALostRaceFromAMissingSession(t *testing.T) {
	for _, test := range []struct {
		name  string
		state *int
		want  uint32
		cell  string
	}{
		{"another caller holds it", intPtr(personaInFlight), store.StatusOK, "0"},
		{"already delivered", intPtr(personaDelivered), store.StatusOK, "0"},
		{"no such session", nil, store.StatusFailed, ""},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newSessDB()
			db.execRows = 0 // the guarded update matched nothing
			db.personaState = test.state
			status, cells := sessCall(t, db, opPersonaDeliveryClaim, []string{"sess"})
			if status != test.want {
				t.Fatalf("status = %d, want %d", status, test.want)
			}
			if test.cell != "" && (len(cells) != 1 || cells[0] != test.cell) {
				t.Fatalf("cells = %v, want [%s]", cells, test.cell)
			}
		})
	}
}

func intPtr(v int) *int { return &v }

// Finishing with delivered = 0 returns the session to unclaimed so a later
// request retries, rather than the persona being lost because one request
// failed after claiming.
func TestPersonaFinishReleasesTheClaimOnFailure(t *testing.T) {
	for _, test := range []struct {
		name      string
		delivered string
		want      int
	}{
		{"delivered", "1", personaDelivered},
		{"not delivered returns it to unclaimed", "0", personaUnclaimed},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newSessDB()
			status, _ := sessCall(t, db, opPersonaDeliveryFinish, []string{"sess", test.delivered})
			if status != store.StatusOK {
				t.Fatalf("status = %d", status)
			}
			if got := db.args[0][1].(int); got != test.want {
				t.Fatalf("wrote state %d, want %d", got, test.want)
			}
		})
	}

	// Finishing without holding the claim changes nothing and is reported.
	db := newSessDB()
	db.execRows = 0
	if status, _ := sessCall(t, db, opPersonaDeliveryFinish, []string{"sess", "1"}); status != store.StatusFailed {
		t.Fatalf("status = %d, want %d", status, store.StatusFailed)
	}
}

// The finish is guarded on in-flight, or a caller that never claimed could mark
// the persona delivered.
func TestPersonaFinishIsGuardedOnHoldingTheClaim(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opPersonaDeliveryFinish, []string{"sess", "1"})
	if !strings.Contains(db.executed[0], "persona_delivery_state = 2") {
		t.Fatalf("the finish is not guarded on in-flight: %s", db.executed[0])
	}
}

// --- expiry is parameterised ---------------------------------------------------

// The C built these two statements with snprintf, splicing the threshold into
// the SQL text. It is a bound parameter now.
func TestExpiryThresholdIsAParameterNotSpliced(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
		at     int
	}{
		{"list", opServerSessionListExpired, []string{"3600", "10"}, 0},
		{"delete", opServerSessionDeleteExpired, []string{"3600"}, 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newSessDB()
			sessCall(t, db, test.op, test.fields)
			sql := db.executed[0]
			if strings.Contains(sql, "3600") {
				t.Fatalf("the threshold is spliced into the SQL: %s", sql)
			}
			if !strings.Contains(sql, "make_interval(secs => $1)") {
				t.Fatalf("the threshold is not an interval parameter: %s", sql)
			}
			if got := db.args[0][test.at].(int); got != 3600 {
				t.Fatalf("bound %v, want 3600", got)
			}
		})
	}
}

func TestDeleteExpiredReportsHowManyItRemoved(t *testing.T) {
	db := newSessDB()
	db.execRows = 7
	status, cells := sessCall(t, db, opServerSessionDeleteExpired, []string{"60"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "7" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

// --- the ten-cell row -----------------------------------------------------------

// The C's list selected eight columns and emitted ten cells, so source and
// chat_key were structurally always blank. The query selects all ten now.
func TestServerSessionListSelectsEveryColumnItEmits(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opServerSessionListRecent, []string{"10"})
	sql := db.executed[0]
	for _, column := range []string{"id", "client_type", "principal", "title",
		"created_at", "last_activity_at", "claude_session_id", "outcome",
		"source", "chat_key"} {
		if !strings.Contains(sql, column) {
			t.Fatalf("the list does not select %s but emits a cell for it: %s", column, sql)
		}
	}
}

func TestServerSessionRowsAreTenCellsWide(t *testing.T) {
	db := newSessDB()
	row := make([]any, 10)
	for i := range row {
		row[i] = "x"
	}
	db.rows = [][]any{row, row}
	status, cells := sessCall(t, db, opServerSessionListRecent, []string{"10"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(cells) != 20 || len(cells)%10 != 0 {
		t.Fatalf("cells = %d, want 2 rows of 10", len(cells))
	}
}

// --- the title search is case-insensitive ---------------------------------------

// SQLite's LIKE was case-insensitive, so a case-sensitive LIKE here would
// quietly stop finding sessions a person used to be able to search for.
func TestTitleSearchIsCaseInsensitive(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opServerSessionSearchByTitle, []string{"%Deploy%", "10"})
	if !strings.Contains(db.executed[0], "ILIKE") {
		t.Fatalf("the title search is case-sensitive: %s", db.executed[0])
	}
	// The pattern is passed through: this is a search, not a prefix lookup.
	if got := db.args[0][0].(string); got != "%Deploy%" {
		t.Fatalf("bound pattern %q, want it passed through", got)
	}
}

// The transcript search's wire carries a RAW query, not a pattern: the C wrapped
// it in %...%, and this must too, or a caller searching for a word gets an exact
// comparison and no results.
func TestTranscriptSearchWrapsTheRawQuery(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opPrimarySessionAllocSearch, []string{"deploy", "10"})
	if bound := db.args[0][0].(string); bound != "%deploy%" {
		t.Fatalf("bound %q, want the query wrapped as a substring pattern", bound)
	}
	// And it covers every column a person might recognise a transcript by.
	sql := db.executed[0]
	for _, column := range []string{"messages_json", "session_id", "agent_name", "provider"} {
		if !strings.Contains(sql, column) {
			t.Fatalf("the transcript search does not cover %s: %s", column, sql)
		}
	}
	if !strings.Contains(sql, "ILIKE") {
		t.Fatalf("the transcript search is case-sensitive: %s", sql)
	}
}

// --- webchat bindings -----------------------------------------------------------

// A binding is never hijacked. If another tab already owns the id the bind is
// refused rather than repointing it.
func TestBindRefusesToHijackAnotherTabsSession(t *testing.T) {
	db := newSessDB()
	db.ownedByOther = true
	status, _ := sessCall(t, db, opWebchatClaudeSessionBind,
		[]string{"webuser:a", "aimee-1", "claude-1"})
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}
	if ran(db, "UPDATE webchat_claude_sessions") || ran(db, "INSERT INTO webchat_claude_sessions") {
		t.Fatalf("a hijacking bind wrote anyway")
	}
}

// Existing databases can hold one tab under several historical principals, so
// the update touches every copy: principal is attribution, not a namespace.
func TestBindUpdatesEveryCopyAndInsertsOnlyWhenNoneExist(t *testing.T) {
	db := newSessDB()
	status, _ := sessCall(t, db, opWebchatClaudeSessionBind,
		[]string{"webuser:a", "aimee-1", "claude-1"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if !ran(db, "UPDATE webchat_claude_sessions") {
		t.Fatalf("the bind did not update")
	}
	if ran(db, "INSERT INTO webchat_claude_sessions") {
		t.Fatalf("the bind inserted despite an existing row")
	}
	// The update is keyed on the tab alone.
	for i, sql := range db.executed {
		if strings.Contains(sql, "UPDATE webchat_claude_sessions") {
			if strings.Contains(sql, "principal") {
				t.Fatalf("the update is namespaced by principal: %s", sql)
			}
			if db.args[i][1].(string) != "aimee-1" {
				t.Fatalf("the update is keyed on %v", db.args[i][1])
			}
		}
	}

	db = newSessDB()
	db.execRows = 0 // nothing to update
	sessCall(t, db, opWebchatClaudeSessionBind, []string{"webuser:a", "aimee-1", "claude-1"})
	if !ran(db, "INSERT INTO webchat_claude_sessions") {
		t.Fatalf("a new tab was not inserted")
	}
}

// A row with no binding reads the same as no row: the caller has nothing to
// resume either way.
func TestWebchatGetTreatsAnEmptyBindingAsMissing(t *testing.T) {
	db := newSessDB()
	db.row = []any{""}
	status, _ := sessCall(t, db, opWebchatClaudeSessionGet, []string{"webuser:a", "aimee-1"})
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
	}

	db = newSessDB()
	db.row = []any{"claude-1"}
	status, cells := sessCall(t, db, opWebchatClaudeSessionGet, []string{"webuser:a", "aimee-1"})
	if status != store.StatusOK || cells[0] != "claude-1" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

// --- the live turn ----------------------------------------------------------------

// rev advances on every write so a poller can tell the row moved without
// diffing the text; the upsert must increment rather than reset it.
func TestLiveSetAdvancesTheRevision(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opWebchatLiveSet, []string{"sess", "turn-1", "hello", "streaming"})
	sql := db.executed[0]
	if !strings.Contains(sql, "rev = webchat_live.rev + 1") {
		t.Fatalf("the upsert does not advance the revision: %s", sql)
	}
}

// Nothing new is MISSING, not a failure: the poller asked whether the row had
// moved and it had not.
func TestLiveGetReportsNothingNewAsMissing(t *testing.T) {
	db := newSessDB()
	status, _ := sessCall(t, db, opWebchatLiveGet, []string{"sess", "5"})
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
	}

	db = newSessDB()
	db.row = []any{"turn-1", "hello", "streaming", int64(6)}
	status, cells := sessCall(t, db, opWebchatLiveGet, []string{"sess", "5"})
	if status != store.StatusOK || len(cells) != 4 || cells[3] != "6" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	// The revision the poller already has is bound, so the query is what
	// decides whether there is anything new.
	if got := db.args[0][1].(int64); got != 5 {
		t.Fatalf("bound since_rev %v, want 5", got)
	}
}

// --- write paths --------------------------------------------------------------------

// seq is allocated from the current maximum so the caller does not track a
// per-session counter.
func TestWritePathAllocatesItsOwnSequence(t *testing.T) {
	db := newSessDB()
	status, _ := sessCall(t, db, opSessionWritePathRecord, []string{"sess", "/a"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if !strings.Contains(db.executed[0], "max(seq) + 1") {
		t.Fatalf("the insert does not allocate a sequence: %s", db.executed[0])
	}
	if !strings.Contains(db.executed[0], "coalesce(") {
		t.Fatalf("the first path in a session would get a NULL sequence: %s", db.executed[0])
	}
}

// --- validation and bounds ------------------------------------------------------------

func TestSessionsValidateTheirArguments(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"create with no id", opServerSessionCreate, []string{"", "cli", "p"}},
		{"get with no id", opServerSessionGet, []string{""}},
		{"delete with no id", opServerSessionDelete, []string{""}},
		{"list max zero", opServerSessionListRecent, []string{"0"}},
		{"list max too large", opServerSessionListRecent, []string{"99999"}},
		{"search max not a number", opServerSessionSearchByTitle, []string{"x", "all"}},
		{"expired negative threshold", opServerSessionListExpired, []string{"-1", "10"}},
		{"primary save without agent", opPrimarySessionSave, []string{"s", "", "p", "[]"}},
		{"primary load without provider", opPrimarySessionLoad, []string{"s", "a", ""}},
		{"write path with no path", opSessionWritePathRecord, []string{"sess", ""}},
		{"stale reads without child", opSessionStaleReads, []string{"parent", "", "10"}},
		{"live set with no session", opWebchatLiveSet, []string{"", "t", "x", "idle"}},
		{"live get with a bad revision", opWebchatLiveGet, []string{"sess", "soon"}},
		{"bind with no claude id", opWebchatClaudeSessionBind, []string{"p", "aimee-1", ""}},
		{"persona finish with a bad flag", opPersonaDeliveryFinish, []string{"sess", "maybe"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newSessDB()
			status, _ := sessCall(t, db, test.op, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid request ran %d statements", len(db.executed))
			}
		})
	}
}

// An empty `since` means all time rather than "since the epoch", which is why
// count is two statements and not a coalesce.
func TestCountDistinguishesAllTimeFromASince(t *testing.T) {
	db := newSessDB()
	db.row = []any{int64(42)}
	_, cells := sessCall(t, db, opServerSessionCount, []string{""})
	if cells[0] != "42" {
		t.Fatalf("cells = %v", cells)
	}
	if strings.Contains(db.executed[0], "WHERE") {
		t.Fatalf("an empty since still filtered: %s", db.executed[0])
	}

	db = newSessDB()
	db.row = []any{int64(7)}
	sessCall(t, db, opServerSessionCount, []string{"2026-01-01 00:00:00"})
	if !strings.Contains(db.executed[0], "created_at >= $1") {
		t.Fatalf("a since did not filter: %s", db.executed[0])
	}
}

func TestReadsDistinguishMissingFromFailed(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"server session get", opServerSessionGet, []string{"sess"}},
		{"primary load", opPrimarySessionLoad, []string{"s", "a", "p"}},
		{"primary latest", opPrimarySessionGetLatest, []string{"s"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			if status, _ := sessCall(t, newSessDB(), test.op, test.fields); status != store.StatusMissing {
				t.Fatalf("no row: status = %d, want %d", status, store.StatusMissing)
			}
			db := newSessDB()
			db.scanErr = errors.New("store broke")
			if status, _ := sessCall(t, db, test.op, test.fields); status != store.StatusFailed {
				t.Fatalf("broken store: status = %d, want %d", status, store.StatusFailed)
			}
		})
	}
}

// Every LIMIT read is totally ordered, or a page is not a page.
func TestListReadsAreTotallyOrdered(t *testing.T) {
	for _, test := range []struct {
		op     uint32
		fields []string
	}{
		{opServerSessionListRecent, []string{"10"}},
		{opServerSessionSearchByTitle, []string{"x", "10"}},
		{opServerSessionListExpired, []string{"60", "10"}},
		{opPrimarySessionAllocRecent, []string{"10"}},
		{opPrimarySessionAllocSearch, []string{"x", "10"}},
	} {
		db := newSessDB()
		sessCall(t, db, test.op, test.fields)
		sql := db.executed[0]
		if !strings.Contains(sql, "ORDER BY") {
			t.Fatalf("a LIMIT read has no ORDER BY: %s", sql)
		}
		// A single ordering column is not a total order unless it is unique.
		if strings.Count(sql[strings.Index(sql, "ORDER BY"):], ",") == 0 {
			t.Fatalf("the ordering has no tiebreaker: %s", sql)
		}
	}
}
