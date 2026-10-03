package postgres

import (
	"context"
	"encoding/binary"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5/pgxpool"
)

func sessionFrame(op uint32, id uint64, sql string, args ...any) []byte {
	w := &writer{}
	w.u32(op)
	w.u64(id)
	if op == sessionExec || op == sessionQuery {
		w.str(sql)
		w.u32(uint32(len(args)))
		for _, v := range args {
			if err := w.value(v); err != nil {
				panic(err)
			}
		}
	}
	return w.buf
}
func sessionReply(t *testing.T, h *sessionHandler, op uint32, id uint64, sql string, args ...any) *reader {
	t.Helper()
	body, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, sessionFrame(op, id, sql, args...))
	if status != bus.ModuleStatusOK {
		t.Fatal("session bus status", status)
	}
	r := &reader{buf: body}
	code, err := r.u32()
	state, e1 := r.str()
	message, e2 := r.str()
	if err != nil || e1 != nil || e2 != nil || code != statusOK {
		t.Fatalf("session result code=%d state=%s reason=%s", code, state, message)
	}
	return r
}
func sessionID(t *testing.T, h *sessionHandler) uint64 {
	t.Helper()
	id, err := sessionReply(t, h, sessionAcquire, 0, "").u64()
	if err != nil || id == 0 {
		t.Fatal("session ID", err)
	}
	return id
}
func sessionTextRows(t *testing.T, r *reader) [][]*string {
	t.Helper()
	if _, err := r.byte1(); err != nil {
		t.Fatal(err)
	}
	if _, err := r.byte1(); err != nil {
		t.Fatal(err)
	}
	if _, err := r.u64(); err != nil {
		t.Fatal(err)
	}
	width, e1 := r.u32()
	count, e2 := r.u32()
	if e1 != nil || e2 != nil {
		t.Fatal("row header")
	}
	for n := uint32(0); n < width; n++ {
		if _, err := r.str(); err != nil {
			t.Fatal(err)
		}
		if _, err := r.u32(); err != nil {
			t.Fatal(err)
		}
	}
	out := make([][]*string, count)
	for y := range out {
		out[y] = make([]*string, width)
		for x := range out[y] {
			n, err := r.u32()
			if err != nil {
				t.Fatal(err)
			}
			if n == ^uint32(0) {
				continue
			}
			if uint64(r.at)+uint64(n) > uint64(len(r.buf)) {
				t.Fatal("short cell")
			}
			v := string(r.buf[r.at : r.at+int(n)])
			r.at += int(n)
			out[y][x] = &v
		}
	}
	if r.at != len(r.buf) {
		t.Fatal("trailing reply")
	}
	return out
}
func TestSessionAdmissionBeforeDatabase(t *testing.T) {
	h := &sessionHandler{ctx: context.Background(), poolFn: func(context.Context) (*pgxpool.Pool, error) { t.Fatal("invalid caller reached pool"); return nil, nil }}
	for _, inv := range []bus.ModuleInvocation{{StageID: StageSession, PrincipalRef: 7}, {StageID: StageSQL}} {
		if _, status := h.handle(inv, sessionFrame(sessionAcquire, 0, "")); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
	}
	for _, frame := range [][]byte{nil, sessionFrame(99, 0, ""), sessionFrame(sessionRelease, 0, ""), sessionFrame(sessionAcquire, 1, ""), append(sessionFrame(sessionAcquire, 0, ""), 0), sessionFrame(sessionQuery, 1, "")} {
		if _, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
	}
}
func TestSessionRejectsOversizedSchemaBeforeDatabase(t *testing.T) {
	h := &sessionHandler{ctx: context.Background(), poolFn: func(context.Context) (*pgxpool.Pool, error) { t.Fatal("oversized SQL reached pool"); return nil, nil }}
	body := sessionFrame(sessionExec, 1, strings.Repeat(" ", maxSessionStatementBytes+1))
	if _, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, body); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("oversized schema accepted")
	}
}

func sessionTestHandler(t *testing.T) *sessionHandler {
	t.Helper()
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	cfg, err := pgxpool.ParseConfig(url)
	if err != nil {
		t.Fatal("invalid test configuration")
	}
	cfg.MaxConns = 4
	ctx, cancel := context.WithCancel(context.Background())
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal("test pool failed")
	}
	h := &sessionHandler{sessions: make(map[uint64]*postgresSession), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }, idle: time.Minute, ctx: ctx}
	t.Cleanup(func() { cancel(); h.close(); pool.Close() })
	return h
}
func TestPostgresSessionScopeAndTransactionReset(t *testing.T) {
	h := sessionTestHandler(t)
	id := sessionID(t, h)
	sessionReply(t, h, sessionExec, id, "SET aimee.session_test_scope='private'")
	sessionReply(t, h, sessionExec, id, "CREATE TEMP TABLE postgres_session_test(value bigint)")
	sessionReply(t, h, sessionExec, id, "BEGIN")
	sessionReply(t, h, sessionExec, id, "INSERT INTO postgres_session_test VALUES($1)", int64(9007199254740993))
	rows := sessionTextRows(t, sessionReply(t, h, sessionQuery, id, "SELECT value,NULL::text,''::text,true,'\\x0001ff'::bytea FROM postgres_session_test"))
	if len(rows) != 1 || *rows[0][0] != "9007199254740993" || rows[0][1] != nil || *rows[0][2] != "" || *rows[0][3] != "t" || *rows[0][4] != "\\x0001ff" {
		t.Fatal("typed/text rows changed")
	}
	state, err := sessionReply(t, h, sessionState, id, "").byte1()
	if err != nil || state != 'T' {
		t.Fatal("transaction lost", state, err)
	}
	other := sessionID(t, h)
	rows = sessionTextRows(t, sessionReply(t, h, sessionQuery, other, "SELECT COALESCE(current_setting('aimee.session_test_scope',true),''),to_regclass('pg_temp.postgres_session_test')::text"))
	if *rows[0][0] != "" || rows[0][1] != nil {
		t.Fatal("session state leaked to another lease")
	}
	sessionReply(t, h, sessionRelease, other, "")
	sessionReply(t, h, sessionRelease, id, "")
	next := sessionID(t, h)
	rows = sessionTextRows(t, sessionReply(t, h, sessionQuery, next, "SELECT COALESCE(current_setting('aimee.session_test_scope',true),''),to_regclass('pg_temp.postgres_session_test')::text"))
	if *rows[0][0] != "" || rows[0][1] != nil {
		t.Fatal("session state leaked across release")
	}
	sessionReply(t, h, sessionRelease, next, "")
	body, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, sessionFrame(sessionState, id, ""))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(body) != statusFailed || !strings.Contains(string(body), "08003") {
		t.Fatal("closed session usable")
	}
}
func TestPostgresSessionCapacityReapingAndRefusals(t *testing.T) {
	h := sessionTestHandler(t)
	first, second := sessionID(t, h), sessionID(t, h)
	body, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, sessionFrame(sessionAcquire, 0, ""))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(body) != statusLimitExceeded {
		t.Fatal("unbounded connection acquisition")
	}
	h.reap(time.Now().Add(2 * time.Minute))
	for _, id := range []uint64{first, second} {
		body, status := h.handle(bus.ModuleInvocation{StageID: StageSession}, sessionFrame(sessionState, id, ""))
		if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(body) != statusFailed {
			t.Fatal("expired session alive")
		}
	}
	id := sessionID(t, h)
	rows := sessionTextRows(t, sessionReply(t, h, sessionQuery, id, "SELECT generate_series(1,4097)"))
	if len(rows) != 4096 {
		t.Fatal("first page", len(rows))
	}
	rows = sessionTextRows(t, sessionReply(t, h, sessionFetch, id, ""))
	if len(rows) != 1 || *rows[0][0] != "4097" {
		t.Fatal("continuation lost rows")
	}
	sessionReply(t, h, sessionExec, id, "BEGIN")
	body, status = h.handle(bus.ModuleInvocation{StageID: StageSession}, sessionFrame(sessionExec, id, "SELECT 1/0"))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(body) != statusFailed || !strings.Contains(string(body), "22012") {
		t.Fatal("SQLSTATE lost")
	}
	if state, _ := sessionReply(t, h, sessionState, id, "").byte1(); state != 'E' {
		t.Fatal("failed transaction hidden")
	}
	sessionReply(t, h, sessionRelease, id, "")
	id = sessionID(t, h)
	sessionReply(t, h, sessionQuery, id, "SELECT 1")
	sessionReply(t, h, sessionRelease, id, "")
}

// Abandoning a paged result must release locks and capacity, even while a
// transaction is open. A replacement client must not inherit the old lease.
func TestPostgresSessionAbandonedPageAndDiscard(t *testing.T) {
	for _, operation := range []uint32{sessionRelease, sessionDiscard} {
		t.Run(map[uint32]string{sessionRelease: "release", sessionDiscard: "discard"}[operation], func(t *testing.T) {
			h := sessionTestHandler(t)
			owner, observer := sessionID(t, h), sessionID(t, h)
			rows := sessionTextRows(t, sessionReply(t, h, sessionQuery, owner, "SELECT pg_backend_pid()::text"))
			pid := *rows[0][0]
			sessionReply(t, h, sessionExec, owner, "BEGIN")
			sessionReply(t, h, sessionQuery, owner, "SELECT pg_advisory_lock(18743,$1::integer)", pid)
			rows = sessionTextRows(t, sessionReply(t, h, sessionQuery, owner, "SELECT generate_series(1,100000)"))
			if len(rows) != 4096 {
				t.Fatal("expected paged result")
			}
			sessionReply(t, h, operation, owner, "")
			until := time.Now().Add(3 * time.Second)
			for {
				rows = sessionTextRows(t, sessionReply(t, h, sessionQuery, observer, "SELECT pg_try_advisory_lock(18743,$1::integer)", pid))
				if *rows[0][0] == "t" {
					break
				}
				if time.Now().After(until) {
					t.Fatal("abandoned session retained advisory lock")
				}
				time.Sleep(10 * time.Millisecond)
			}
			sessionReply(t, h, sessionQuery, observer, "SELECT pg_advisory_unlock(18743,$1::integer)", pid)
			next := sessionID(t, h)
			if next == owner {
				t.Fatal("closed capability reused")
			}
			if state, _ := sessionReply(t, h, sessionState, next, "").byte1(); state != 'I' {
				t.Fatal("replacement inherited transaction")
			}
			r := sessionReply(t, h, sessionStats, 0, "")
			capacity, e1 := r.u32()
			used, e2 := r.u32()
			opening, e3 := r.u32()
			refused, e4 := r.u64()
			expired, e5 := r.u64()
			discarded, e6 := r.u64()
			if e1 != nil || e2 != nil || e3 != nil || e4 != nil || e5 != nil || e6 != nil || r.at != len(r.buf) || capacity != 2 || used != 2 || opening != 0 || refused != 0 || expired != 0 {
				t.Fatal("invalid provider health after abandonment")
			}
			if operation == sessionDiscard && discarded != 1 {
				t.Fatal("discard not reflected in health")
			}
			sessionReply(t, h, sessionRelease, next, "")
			sessionReply(t, h, sessionRelease, observer, "")
		})
	}
}
