package aimee

import (
	"context"
	"encoding/binary"
	"errors"
	"math"
	"testing"
	"time"
)

// The store client is the whole of aimee's dependency on another module, so the
// frame is worth pinning: a side that mis-encodes a value or mis-reads a cell
// fails at runtime as a wrong answer rather than as an error.

type fakeCaller struct {
	kind, stage uint32
	deadline    time.Duration
	request     []byte
	reply       []byte
	err         error
	calls       int
}

func (f *fakeCaller) Call(_ context.Context, kind, stage uint32, _ uint64,
	deadline time.Duration, request []byte) ([]byte, error) {
	f.kind, f.stage, f.deadline, f.request = kind, stage, deadline, request
	f.calls++
	return f.reply, f.err
}

// --- building replies, the way the postgres module will -----------------------

type replyBuilder struct{ b []byte }

func ok() *replyBuilder {
	r := &replyBuilder{}
	return r.u32(StatusOK).str("").str("")
}

func refusal(sqlstate, message string) []byte {
	r := &replyBuilder{}
	return r.u32(StatusFailed).str(sqlstate).str(message).b
}

func (r *replyBuilder) u32(v uint32) *replyBuilder {
	var x [4]byte
	binary.LittleEndian.PutUint32(x[:], v)
	r.b = append(r.b, x[:]...)
	return r
}

func (r *replyBuilder) u64(v uint64) *replyBuilder {
	var x [8]byte
	binary.LittleEndian.PutUint64(x[:], v)
	r.b = append(r.b, x[:]...)
	return r
}

func (r *replyBuilder) str(s string) *replyBuilder {
	r.u32(uint32(len(s)))
	r.b = append(r.b, s...)
	return r
}

func (r *replyBuilder) text(s string) *replyBuilder {
	r.b = append(r.b, wireText)
	return r.str(s)
}

func (r *replyBuilder) num(v int64) *replyBuilder {
	r.b = append(r.b, wireInt)
	return r.u64(uint64(v))
}

func (r *replyBuilder) real(v float64) *replyBuilder {
	r.b = append(r.b, wireFloat)
	return r.u64(math.Float64bits(v))
}

func (r *replyBuilder) boolean(v bool) *replyBuilder {
	r.b = append(r.b, wireBool)
	if v {
		r.b = append(r.b, 1)
	} else {
		r.b = append(r.b, 0)
	}
	return r
}

func (r *replyBuilder) null() *replyBuilder {
	r.b = append(r.b, wireNull)
	return r
}

func (r *replyBuilder) shape(width, count uint32) *replyBuilder {
	return r.u32(width).u32(count)
}

func newStore(t *testing.T, f *fakeCaller) DB {
	t.Helper()
	db, err := NewStore(f)
	if err != nil {
		t.Fatalf("NewStore: %v", err)
	}
	return db
}

// --- the basics ---------------------------------------------------------------

func TestExecReportsTheRowsTheStoreChanged(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(3).b}
	db := newStore(t, f)
	tag, err := db.Exec(t.Context(), "DELETE FROM x WHERE id = $1", int64(7))
	if err != nil {
		t.Fatalf("Exec: %v", err)
	}
	if got := tag.RowsAffected(); got != 3 {
		t.Errorf("RowsAffected = %d, want 3", got)
	}
	if f.kind != EventPostgresSQL || f.stage != StagePostgresSQL {
		t.Errorf("called kind %d stage %d, want %d/%d",
			f.kind, f.stage, EventPostgresSQL, StagePostgresSQL)
	}
}

func TestQueryReadsTypedCellsIntoTypedDestinations(t *testing.T) {
	// One row of every type the wire carries.
	f := &fakeCaller{reply: ok().shape(4, 1).
		text("alice").num(7).real(1.5).boolean(true).b}
	db := newStore(t, f)
	rows, err := db.Query(t.Context(), "SELECT name, n, ratio, ok FROM x")
	if err != nil {
		t.Fatalf("Query: %v", err)
	}
	defer rows.Close()
	if !rows.Next() {
		t.Fatal("no row")
	}
	var (
		name  string
		n     int64
		ratio float64
		flag  bool
	)
	if err := rows.Scan(&name, &n, &ratio, &flag); err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if name != "alice" || n != 7 || ratio != 1.5 || !flag {
		t.Errorf("got %q/%d/%v/%v", name, n, ratio, flag)
	}
}

func TestQueryRowReportsNoRowsRatherThanAnEmptyAnswer(t *testing.T) {
	f := &fakeCaller{reply: ok().shape(1, 0).b}
	db := newStore(t, f)
	var v string
	err := db.QueryRow(t.Context(), "SELECT x FROM y WHERE id = $1", "nope").Scan(&v)
	if !errors.Is(err, ErrNoRows) {
		t.Fatalf("Scan on an empty result = %v, want ErrNoRows", err)
	}
	if !IsNoRows(err) {
		t.Error("IsNoRows did not recognise it")
	}
}

func TestScanRefusesAWidthItWasNotGiven(t *testing.T) {
	// Dropping the second column would hand back a row that is quietly wrong.
	f := &fakeCaller{reply: ok().shape(2, 1).text("a").text("b").b}
	db := newStore(t, f)
	var only string
	if err := db.QueryRow(t.Context(), "SELECT a, b FROM x").Scan(&only); err == nil {
		t.Fatal("scanning 2 columns into 1 destination was accepted")
	}
}

// --- the reason values carry their type ---------------------------------------

func TestABooleanIsNotSilentlyReadAsANumber(t *testing.T) {
	// This is the bug the type byte exists for. Read as text and parsed, a
	// BOOLEAN gives atoi("t") == 0 and every row reads false -- including the
	// rows whose default is true. Refusing it names the column instead.
	f := &fakeCaller{reply: ok().shape(1, 1).boolean(true).b}
	db := newStore(t, f)
	var wrong int64
	err := db.QueryRow(t.Context(), "SELECT enabled FROM mining_jobs").Scan(&wrong)
	if err == nil {
		t.Fatal("a BOOLEAN was accepted into an int64 destination")
	}
	if wrong != 0 {
		t.Errorf("the destination was written despite the refusal: %d", wrong)
	}
}

func TestNullIsNotAnEmptyString(t *testing.T) {
	f := &fakeCaller{reply: ok().shape(1, 1).null().b}
	db := newStore(t, f)

	// A destination that cannot hold NULL must say so rather than take "".
	var flat string
	err := db.QueryRow(t.Context(), "SELECT heartbeat_at FROM agent_jobs").Scan(&flat)
	if err == nil {
		t.Fatal("NULL was flattened into a plain string destination")
	}

	// One that can, gets nil -- "never beaten", not "beat at the zero time".
	f.reply = ok().shape(1, 1).null().b
	var maybe *string
	if err := db.QueryRow(t.Context(), "SELECT heartbeat_at FROM agent_jobs").
		Scan(&maybe); err != nil {
		t.Fatalf("Scan into **string: %v", err)
	}
	if maybe != nil {
		t.Errorf("NULL scanned as %q, want nil", *maybe)
	}
}

func TestAPresentValueScansThroughAPointerToo(t *testing.T) {
	f := &fakeCaller{reply: ok().shape(1, 1).text("2026-08-23").b}
	db := newStore(t, f)
	var maybe *string
	if err := db.QueryRow(t.Context(), "SELECT heartbeat_at FROM agent_jobs").
		Scan(&maybe); err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if maybe == nil || *maybe != "2026-08-23" {
		t.Errorf("got %v, want a pointer to the value", maybe)
	}
}

// --- refusals carry their reason ----------------------------------------------

func TestARefusalCarriesItsSQLSTATE(t *testing.T) {
	f := &fakeCaller{reply: refusal("23505", `duplicate key value violates "jti_pkey"`)}
	db := newStore(t, f)
	_, err := db.Exec(t.Context(), "INSERT INTO jti VALUES ($1)", "abc")
	if err == nil {
		t.Fatal("a refusal was reported as success")
	}
	// The distinction aimee could not make before this crossed: a replay is an
	// expected answer, an outage is not.
	if !IsUniqueViolation(err) {
		t.Errorf("IsUniqueViolation did not recognise %v", err)
	}
	if IsForeignKeyViolation(err) {
		t.Error("a unique violation was classified as a foreign-key violation")
	}
	if errors.Is(err, ErrStoreUnavailable) {
		t.Error("a refusal was classified as the store being unreachable")
	}
}

func TestAnUnreachableStoreIsNotARefusal(t *testing.T) {
	f := &fakeCaller{err: errors.New("bus: capability absent")}
	db := newStore(t, f)
	_, err := db.Exec(t.Context(), "SELECT 1")
	if !errors.Is(err, ErrStoreUnavailable) {
		t.Fatalf("Exec against an absent store = %v, want ErrStoreUnavailable", err)
	}
	if IsUniqueViolation(err) {
		t.Error("an outage was classified as a constraint violation")
	}
}

// --- what travels with a statement ---------------------------------------------

func TestArgumentsCarryTheirType(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(0).b}
	db := newStore(t, f)
	_, err := db.Exec(t.Context(), "UPDATE x SET a = $1 WHERE s = ANY($2)",
		int64(5), []string{"a", "b"})
	if err != nil {
		t.Fatalf("Exec: %v", err)
	}
	r := frameReader{buf: f.request}
	mustU32(t, &r) // op
	mustStr(t, &r) // statement id
	mustU64(t, &r) // tx handle
	mustStr(t, &r) // sql
	if argc := mustU32(t, &r); argc != 2 {
		t.Fatalf("encoded %d arguments, want 2", argc)
	}
	if got := mustByte(t, &r); got != wireInt {
		t.Errorf("first argument tagged %d, want wireInt", got)
	}
	mustU64(t, &r)
	if got := mustByte(t, &r); got != wireTextArray {
		t.Errorf("second argument tagged %d, want wireTextArray", got)
	}
}

func TestTheOperationNameTravelsWithEveryStatement(t *testing.T) {
	// The store does not parse SQL, so this is the only thing that lets it
	// attribute work to an operation rather than to a connection.
	f := &fakeCaller{reply: ok().u64(0).b}
	db := newStore(t, f).(*storeDB).WithStatementID("runtime_state_get")
	if _, err := db.Exec(t.Context(), "SELECT 1"); err != nil {
		t.Fatalf("Exec: %v", err)
	}
	r := frameReader{buf: f.request}
	mustU32(t, &r)
	if got := mustStr(t, &r); got != "runtime_state_get" {
		t.Errorf("statement id %q, want the operation name", got)
	}
}

func TestADeadlineDoesNotOutliveTheCaller(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(0).b}
	db := newStore(t, f)
	ctx, cancel := context.WithTimeout(t.Context(), 50*time.Millisecond)
	defer cancel()
	if _, err := db.Exec(ctx, "SELECT 1"); err != nil {
		t.Fatalf("Exec: %v", err)
	}
	if f.deadline <= 0 || f.deadline > 50*time.Millisecond {
		t.Errorf("passed deadline %v, want the caller's remaining time", f.deadline)
	}
}

// --- transactions ---------------------------------------------------------------

func TestATransactionCarriesItsHandleOnEveryStatement(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(42).b}
	db := newStore(t, f)
	tx, err := db.Begin(t.Context())
	if err != nil {
		t.Fatalf("Begin: %v", err)
	}
	f.reply = ok().u64(1).b
	if _, err := tx.Exec(t.Context(), "INSERT INTO x VALUES ($1)", "v"); err != nil {
		t.Fatalf("Exec in tx: %v", err)
	}
	r := frameReader{buf: f.request}
	mustU32(t, &r)
	mustStr(t, &r)
	if got := mustU64(t, &r); got != 42 {
		t.Errorf("statement carried handle %d, want 42", got)
	}
}

func TestATransactionEndsOnlyOnce(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(1).b}
	db := newStore(t, f)
	tx, _ := db.Begin(t.Context())
	f.reply = ok().u64(0).b
	if err := tx.Commit(t.Context()); err != nil {
		t.Fatalf("Commit: %v", err)
	}
	before := f.calls
	if err := tx.Rollback(t.Context()); !errors.Is(err, ErrTxClosed) {
		t.Errorf("second finish = %v, want ErrTxClosed", err)
	}
	if f.calls != before {
		t.Error("the second finish was sent to the store")
	}
}

// --- migration -------------------------------------------------------------------

func TestCurrentVersionAsksForOneOwner(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(19).str("abc123").b}
	db := newStore(t, f).(*storeDB)
	version, sum, err := db.CurrentSchemaVersion(t.Context(), "db1")
	if err != nil {
		t.Fatalf("CurrentSchemaVersion: %v", err)
	}
	if version != 19 || sum != "abc123" {
		t.Errorf("got v%d/%s, want v19/abc123", version, sum)
	}
	r := frameReader{buf: f.request}
	if op := mustU32(t, &r); op != opStoreCurrentVersion {
		t.Errorf("op %d, want current_version", op)
	}
	if owner := mustStr(t, &r); owner != "db1" {
		t.Errorf("asked for owner %q, want db1", owner)
	}
}

func TestMigrateSendsTheVersionAndItsChecksum(t *testing.T) {
	f := &fakeCaller{reply: ok().b}
	db := newStore(t, f).(*storeDB)
	err := db.Migrate(t.Context(), MigrationRequest{
		Owner: "db1", Version: 20, Checksum: "deadbeef",
		Statements: []string{"CREATE TABLE t (a int);"},
	})
	if err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	r := frameReader{buf: f.request}
	if op := mustU32(t, &r); op != opStoreMigrate {
		t.Errorf("op %d, want migrate", op)
	}
	if owner := mustStr(t, &r); owner != "db1" {
		t.Errorf("owner %q, want db1", owner)
	}
	if v := mustU64(t, &r); v != 20 {
		t.Errorf("version %d, want 20", v)
	}
	if sum := mustStr(t, &r); sum != "deadbeef" {
		t.Errorf("checksum %q", sum)
	}
	if n := mustU32(t, &r); n != 1 {
		t.Errorf("%d statements, want 1", n)
	}
}

func TestAnEditedMigrationIsRefusedByTheStore(t *testing.T) {
	// The store compares the checksum against what it recorded. aimee's job is to
	// surface that refusal with its reason rather than as a bare failure.
	f := &fakeCaller{reply: refusal("", "db1 version 7 was applied with a different checksum")}
	db := newStore(t, f).(*storeDB)
	err := db.Migrate(t.Context(), MigrationRequest{Owner: "db1", Version: 7, Checksum: "changed"})
	if err == nil {
		t.Fatal("an edited migration was accepted")
	}
	var se *StoreError
	if !errors.As(err, &se) {
		t.Fatalf("got %T, want a StoreError carrying the reason", err)
	}
}

func TestNewStoreRefusesWithoutACaller(t *testing.T) {
	// aimee without a store serves nothing, and saying so at construction is what
	// lets the module decline its stages rather than fail every call.
	if _, err := NewStore(nil); err == nil {
		t.Fatal("NewStore(nil) was accepted")
	}
}

// --- limits ------------------------------------------------------------------------

func TestAnOversizedStatementIsRefusedBeforeItIsSent(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(0).b}
	db := newStore(t, f)
	huge := make([]byte, MaxStatementBytes+1)
	for i := range huge {
		huge[i] = ' '
	}
	if _, err := db.Exec(t.Context(), string(huge)); err == nil {
		t.Fatal("a statement over the limit was sent")
	}
	if f.calls != 0 {
		t.Error("it reached the store")
	}
}

func TestTooManyArgumentsAreRefusedBeforeTheyAreSent(t *testing.T) {
	f := &fakeCaller{reply: ok().u64(0).b}
	db := newStore(t, f)
	args := make([]any, MaxArgs+1)
	for i := range args {
		args[i] = "x"
	}
	if _, err := db.Exec(t.Context(), "SELECT 1", args...); err == nil {
		t.Fatal("an over-long argument list was sent")
	}
	if f.calls != 0 {
		t.Error("it reached the store")
	}
}

// --- reading helpers ----------------------------------------------------------------

func mustU32(t *testing.T, r *frameReader) uint32 {
	t.Helper()
	v, err := r.u32()
	if err != nil {
		t.Fatalf("u32: %v", err)
	}
	return v
}

func mustU64(t *testing.T, r *frameReader) uint64 {
	t.Helper()
	v, err := r.u64()
	if err != nil {
		t.Fatalf("u64: %v", err)
	}
	return v
}

func mustStr(t *testing.T, r *frameReader) string {
	t.Helper()
	v, err := r.str()
	if err != nil {
		t.Fatalf("str: %v", err)
	}
	return v
}

func mustByte(t *testing.T, r *frameReader) uint8 {
	t.Helper()
	v, err := r.byte1()
	if err != nil {
		t.Fatalf("byte: %v", err)
	}
	return v
}
