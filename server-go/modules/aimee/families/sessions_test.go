package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
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
	receiptRow   []any
	rows         [][]any
	personaState *int
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
	if sql == sessionIRHistorySQL {
		after, _ := strconv.ParseUint(args[2].(string), 10, 64)
		page := make([][]any, 0, sessionIRHistoryPageSize)
		for _, row := range d.rows {
			n, _ := strconv.ParseUint(row[0].(string), 10, 64)
			if n > after {
				page = append(page, row)
				if len(page) == sessionIRHistoryPageSize {
					break
				}
			}
		}
		return &sessRows{rows: page}, nil
	}
	return &sessRows{rows: d.rows}, nil
}
func (d *sessDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	switch {
	case strings.Contains(sql, "local_procedure_prepare_subject_erasure"):
		return sessRow{db: d, sql: sql, values: []any{int64(0)}}
	case strings.Contains(sql, "user_memory_prepare_subject_erasure"):
		return sessRow{db: d, sql: sql, values: d.receiptRow}
	case strings.Contains(sql, "SELECT persona_delivery_state"):
		if d.personaState == nil {
			return sessRow{db: d, sql: sql}
		}
		return sessRow{db: d, sql: sql, values: []any{*d.personaState}}
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

// Retired private transcript/binding op numbers are never reused or served.
// The real wire handler must refuse before any read, write, or transaction.
func TestRetiredLegacySessionOperationsRefuseBeforeStorage(t *testing.T) {
	cases := []struct {
		id     uint32
		fields []string
	}{
		{10, []string{"legacy", "agent", "provider", "[]"}},
		{11, []string{"legacy", "agent", "provider"}},
		{12, []string{"legacy", "agent", "provider"}},
		{13, []string{"2"}}, {14, []string{"legacy", "2"}},
		{18, []string{"actor", "legacy"}},
		{19, []string{"actor", "legacy", "sdk-id"}},
		{20, []string{"actor", "legacy", "sdk-id"}},
	}
	for _, test := range cases {
		for _, principal := range []uint32{0, 7} {
			db := newSessDB()
			frame, err := wire.EncodeFields(test.id, test.fields)
			if err != nil {
				t.Fatal(err)
			}
			reply, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: principal}, frame)
			if status != bus.ModuleStatusInvalidRequest || len(reply) != 0 || len(db.executed) != 0 {
				t.Fatalf("retired op %d principal %d reached storage or acquired a reply: status=%d reply=%x statements=%v", test.id, principal, status, reply, db.executed)
			}
		}
	}
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

func TestExpiryReapsMutableContentGraphButNotEvidence(t *testing.T) {
	db := newSessDB()
	sessCall(t, db, opServerSessionDeleteExpired, []string{"60"})
	sql := db.executed[0]
	for _, table := range []string{
		"delegation_messages", "delegate_learnings", "working_profile_observations_local",
		"context_cache", "context_snapshots", "context_activation_events",
		"file_snapshots", "execution_trace", "workflow_binding", "branch_ownership",
		"economizer_state", "checkpoints", "agent_log", "session_state",
		"working_memory", "payload_rewrite_state", "conv_tool_events",
		"conv_tool_chains", "conv_context_state", "windows", "primary_sessions",
		"webchat_claude_sessions", "webchat_live", "session_state_write_paths",
		"server_sessions",
	} {
		if !strings.Contains(sql, "DELETE FROM "+table) {
			t.Errorf("retention does not delete mutable family %s", table)
		}
	}
	for _, immutable := range []string{"audit", "worm", "witness", "ledger"} {
		if strings.Contains(strings.ToLower(sql), "delete from "+immutable) {
			t.Errorf("retention attempts to rewrite immutable evidence family %s", immutable)
		}
	}
}

func TestSubjectErasureIsBoundAndContentComplete(t *testing.T) {
	db := newSessDB()
	db.rows = [][]any{{"session-a"}, {"session-b"}}
	status, cells := sessCall(t, db, opServerSessionListBySubject, []string{"person@example.test", "10"})
	if status != store.StatusOK || len(cells) != 2 {
		t.Fatalf("list status = %d, cells = %v", status, cells)
	}
	if got := db.args[0][0].(string); got != "person@example.test" {
		t.Fatalf("list subject = %q", got)
	}

	db = newSessDB()
	db.execRows = 2
	db.receiptRow = []any{`[]`, int64(2)}
	status, cells = sessCall(t, db, opServerSessionEraseSubject,
		[]string{"erase-request-0123456789", "person@example.test"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "2" {
		t.Fatalf("erase status = %d, cells = %v", status, cells)
	}
	deleteAt := -1
	for i, sql := range db.executed {
		if strings.Contains(sql, "WITH subject_sessions") {
			deleteAt = i
			break
		}
	}
	if deleteAt < 0 {
		t.Fatalf("subject deletion statement was not executed: %v", db.executed)
	}
	sql := db.executed[deleteAt]
	if strings.Contains(sql, "person@example.test") || db.args[deleteAt][0] != "person@example.test" {
		t.Fatalf("subject must be a bound parameter: sql=%q args=%v", sql, db.args[deleteAt])
	}
	for _, table := range []string{"working_memory", "conv_tool_events", "primary_sessions", "webchat_claude_sessions", "server_sessions"} {
		if !strings.Contains(sql, "DELETE FROM "+table) {
			t.Errorf("subject erasure does not delete %s", table)
		}
	}
	for _, immutable := range []string{"audit", "worm", "witness", "ledger"} {
		if strings.Contains(strings.ToLower(sql), "delete from "+immutable) {
			t.Errorf("subject erasure attempts to rewrite immutable evidence family %s", immutable)
		}
	}
}

func TestSubjectErasureRetryReturnsJournaledCount(t *testing.T) {
	db := newSessDB()
	digestBytes := sha256.Sum256([]byte("person@example.test"))
	db.row = []any{hex.EncodeToString(digestBytes[:]), int64(7)}
	status, cells := sessCall(t, db, opServerSessionEraseSubject,
		[]string{"erase-request-0123456789", "person@example.test"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "7" {
		t.Fatalf("retry status = %d, cells = %v", status, cells)
	}
	for _, sql := range db.executed {
		if strings.Contains(sql, "WITH subject_sessions") {
			t.Fatal("journaled retry repeated mutable-data deletion")
		}
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
		{"write path with no path", opSessionWritePathRecord, []string{"sess", ""}},
		{"stale reads without child", opSessionStaleReads, []string{"parent", "", "10"}},
		{"live set with no session", opWebchatLiveSet, []string{"", "t", "x", "idle"}},
		{"live get with a bad revision", opWebchatLiveGet, []string{"sess", "soon"}},
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

// The append boundary must fail before touching storage for malformed content;
// model/harness conversion belongs upstream of the session log.
func TestSessionIRRejectsInvalidFramesAndCursors(t *testing.T) {
	for _, fields := range [][]string{
		{"", "e", "t", "actor", `{"kind":"message","payload":{}}`},
		{"s", "", "t", "actor", `{"kind":"message","payload":{}}`},
		{"s", "e", "t", "actor", `{"kind":"native","payload":{}}`},
		{"s", "e", "t", "actor", `{"kind":"message"}`},
		{"s", "e", "t", "actor", `not JSON`},
		{"s", "e", "t", "actor", strings.Repeat("x", (1<<20)+1)},
	} {
		db := &sessDB{}
		status, _ := sessCall(t, db, opSessionIRAppend, fields)
		if status != store.StatusInvalid {
			t.Fatalf("invalid append status %d", status)
		}
		if len(db.executed) != 0 {
			t.Fatal("malformed event reached storage")
		}
	}
	for _, fields := range [][]string{{"s", "-1", "1"}, {"s", "0", "129"}, {"s", "not-a-cursor", "1"}, {"s", "0", "0"}} {
		status, _ := sessCall(t, &sessDB{}, opSessionIRPage, append(fields, "actor"))
		if status != store.StatusInvalid {
			t.Fatalf("invalid page status %d", status)
		}
	}
}

func TestSessionIRPagePreservesRolesToolsAndExactSequence(t *testing.T) {
	db := &sessDB{rows: [][]any{
		{int64(9007199254740993), "event-1", "turn-1", "operator",
			`{"kind":"message","message_id":"m1","payload":{"role":"user","blocks":[{"type":"text","text":"hello"}]}}`},
		{int64(9007199254740994), "event-2", "turn-1", "tool-runtime",
			`{"kind":"message","message_id":"m2","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"opaque:42","content":"done"}]}}`},
	}}
	status, cells := sessCall(t, db, opSessionIRPage, []string{"session-1", "9007199254740992", "128", "actor"})
	if status != store.StatusOK || len(cells) != 1 {
		t.Fatalf("page %d %v", status, cells)
	}
	var envelopes []struct {
		Version  int             `json:"version"`
		Session  string          `json:"session_id"`
		Sequence string          `json:"sequence"`
		Event    json.RawMessage `json:"event"`
	}
	if err := json.Unmarshal([]byte(cells[0]), &envelopes); err != nil {
		t.Fatal(err)
	}
	if len(envelopes) != 2 || envelopes[0].Sequence != "9007199254740993" || envelopes[1].Sequence != "9007199254740994" {
		t.Fatalf("sequence precision lost: %s", cells[0])
	}
	if envelopes[0].Version != 1 || envelopes[0].Session != "session-1" || !strings.Contains(string(envelopes[1].Event), `"tool_id":"opaque:42"`) {
		t.Fatalf("canonical envelope lost content: %s", cells[0])
	}
}

type irAppendFixture struct {
	*sessDB
	existing  []any
	inserted  int64
	lockErr   error
	insertErr error
	reads     []string
}

func (d *irAppendFixture) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	d.reads = append(d.reads, sql)
	switch sql {
	case sessionIRLockSQL:
		return sessRow{db: &sessDB{scanErr: d.lockErr}, values: []any{"actor"}}
	case sessionIRExistingSQL:
		return sessRow{db: &sessDB{}, values: d.existing}
	case nativeRecoveryFenceSQL:
		return sessRow{db: &sessDB{scanErr: store.ErrNoRows}}
	case sessionIRInsertSQL:
		return sessRow{db: &sessDB{scanErr: d.insertErr}, values: []any{d.inserted}}
	default:
		return d.sessDB.QueryRow(ctx, sql, args...)
	}
}
func TestSessionIRAppendRetryAndFailure(t *testing.T) {
	payload := `{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[]}}`
	fields := []string{"session-1", "event-1", "turn-1", "actor", payload}
	db := &irAppendFixture{sessDB: &sessDB{}, inserted: 42}
	status, cells, err := sessionIRAppend(context.Background(), db, fields)
	if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "42" {
		t.Fatalf("append %d %v %v", status, cells, err)
	}
	if len(db.reads) != 4 || db.reads[0] != sessionIRLockSQL || db.reads[2] != nativeRecoveryFenceSQL || db.reads[3] != sessionIRInsertSQL {
		t.Fatal("append was not serialized under session lock")
	}
	db = &irAppendFixture{sessDB: &sessDB{}, existing: []any{int64(42), "turn-1", "actor", payload}}
	status, cells, err = sessionIRAppend(context.Background(), db, fields)
	if err != nil || status != store.StatusOK || cells[0] != "42" || len(db.reads) != 2 {
		t.Fatal("retry inserted another event")
	}
	for _, index := range []int{2, 3, 4} {
		changed := append([]string{}, fields...)
		changed[index] += " "
		status, _, err = sessionIRAppend(context.Background(), db, changed)
		if err != nil || status != store.StatusInvalid {
			t.Fatal("conflicting retry was accepted")
		}
	}
	db = &irAppendFixture{sessDB: &sessDB{}, insertErr: errors.New("disk unavailable")}
	_, _, err = sessionIRAppend(context.Background(), db, fields)
	if err == nil {
		t.Fatal("storage failure was hidden")
	}
}

func TestSessionIRPageBoundsIncludeEncoding(t *testing.T) {
	// HTML-sensitive text is common in code/tool results. JSON transport must not
	// expand this into an oversized envelope or an empty, non-advancing page.
	text := strings.Repeat("<>&", 240000)
	payload := `{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"text","text":"` + text + `"}]}}`
	db := &sessDB{}
	for n := 1; n <= 5; n++ {
		db.rows = append(db.rows, []any{int64(n), fmt.Sprintf("e%d", n), "turn", "actor", payload})
	}
	status, cells := sessCall(t, db, opSessionIRPage, []string{"s", "0", "128", "actor"})
	if status != store.StatusOK || len(cells) != 1 || len(cells[0]) > 3<<20 {
		t.Fatalf("invalid page: %d cells=%d", status, len(cells))
	}
	var page []struct {
		Sequence string          `json:"sequence"`
		Event    json.RawMessage `json:"event"`
	}
	if err := json.Unmarshal([]byte(cells[0]), &page); err != nil {
		t.Fatal(err)
	}
	if len(page) != 4 || page[0].Sequence != "1" || page[3].Sequence != "4" {
		t.Fatalf("page did not advance to bounded cursor: %d records", len(page))
	}
	if !strings.Contains(string(page[0].Event), "<>&") {
		t.Fatal("transport unexpectedly HTML-escaped code")
	}
}

func TestSessionHarnessMetadata(t *testing.T) {
	db := newSessDB()
	db.row = []any{"native-one"}
	status, cells := sessCall(t, db, opSessionHarnessGet, []string{"s", "claude", "owner"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "native-one" {
		t.Fatalf("get: %d %v", status, cells)
	}
	db = newSessDB()
	status, _ = sessCall(t, db, opSessionHarnessGet, []string{"s", "claude", "owner"})
	if status != store.StatusMissing {
		t.Fatalf("missing became %d", status)
	}
	db = newSessDB()
	db.row = []any{"owner"}
	status, _ = sessCall(t, db, opSessionHarnessBind, []string{"s", "codex", "native-two", "owner"})
	if status != store.StatusOK || len(db.executed) != 2 || db.executed[0] != sessionIRLockSQL || db.executed[1] != sessionHarnessBindSQL {
		t.Fatalf("bind not locked: %d %v", status, db.executed)
	}
	for _, f := range [][]string{{"", "codex", "id"}, {"s", "", "id"}, {"s", strings.Repeat("x", 129), "id"}, {"s", "codex", ""}} {
		status, _ = sessCall(t, newSessDB(), opSessionHarnessBind, append(f, "owner"))
		if status != store.StatusInvalid {
			t.Fatalf("accepted bad metadata: %v", f)
		}
	}
	db = newSessDB()
	db.scanErr = errors.New("store unavailable")
	if _, _, err := sessionHarnessGet(context.Background(), db, []string{"s", "codex", "owner"}); err == nil {
		t.Fatal("store failure became missing")
	}
}
