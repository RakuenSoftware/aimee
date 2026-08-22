package db2

import (
	"context"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// fakeStore answers from a script rather than a database, so the dispatcher and
// an operation's row handling can be tested without one.
type fakeStore struct {
	rows     *fakeRows
	row      *fakeRow
	rowQueue []*fakeRow
	queryErr error
	execErr  error
	lastSQL  string
	lastArgs []any
	// Every statement in order. An operation that issues more than one -- a
	// write and the read-back of what it wrote -- needs the first inspected,
	// and lastSQL by then holds the last.
	sqlLog     []string
	argsLog    [][]any
	execCalls  int
	execRows   int64
	execRowsAt bool
	txCalls    int
	txBeginErr error
	committed  bool
	rolledBack bool
}

func (s *fakeStore) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	s.lastSQL, s.lastArgs = sql, args
	s.sqlLog = append(s.sqlLog, sql)
	s.argsLog = append(s.argsLog, args)
	if s.queryErr != nil {
		return nil, s.queryErr
	}
	return s.rows, nil
}

func (s *fakeStore) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	s.lastSQL, s.lastArgs = sql, args
	s.sqlLog = append(s.sqlLog, sql)
	s.argsLog = append(s.argsLog, args)
	// A queue when an operation issues several single-row statements in order --
	// an insert and then the read-back of what it wrote, which scan different
	// widths and cannot share one scripted row.
	if len(s.rowQueue) > 0 {
		next := s.rowQueue[0]
		s.rowQueue = s.rowQueue[1:]
		return next
	}
	if s.row == nil {
		return &fakeRow{err: pgx.ErrNoRows}
	}
	return s.row
}

func (s *fakeStore) Exec(ctx context.Context, sql string, args ...any) (int64, error) {
	s.lastSQL, s.lastArgs = sql, args
	s.sqlLog = append(s.sqlLog, sql)
	s.argsLog = append(s.argsLog, args)
	s.execCalls++
	if s.execErr != nil {
		return 0, s.execErr
	}
	// One row affected unless a test says otherwise. execRowsAt distinguishes
	// "the test wants zero" from "the test said nothing", which matters because
	// zero is the interesting answer for a statement that matched nothing.
	if s.execRowsAt {
		return s.execRows, nil
	}
	return 1, nil
}

// InTx runs fn against this same fake, so a transactional operation is
// exercised without one. The commit is recorded rather than performed: what a
// test can check is that the operation asked for a transaction and whether it
// returned an error inside it, which is what decides commit from rollback.
func (s *fakeStore) InTx(ctx context.Context, fn func(Store) error) error {
	s.txCalls++
	if s.txBeginErr != nil {
		return s.txBeginErr
	}
	if err := fn(s); err != nil {
		s.rolledBack = true
		return err
	}
	s.committed = true
	return nil
}

// fakeRow is one scripted row, or the absence of one.
type fakeRow struct {
	values []any
	err    error
}

func (r *fakeRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) != len(r.values) {
		return errors.New("unexpected scan width")
	}
	for index, target := range dest {
		switch typed := target.(type) {
		case *int64:
			*typed = r.values[index].(int64)
		case *int32:
			*typed = r.values[index].(int32)
		case *string:
			*typed = r.values[index].(string)
		case *float64:
			*typed = r.values[index].(float64)
		case *bool:
			*typed = r.values[index].(bool)
		case **int64:
			*typed, _ = r.values[index].(*int64)
		case **string:
			*typed, _ = r.values[index].(*string)
		default:
			return errors.New("unexpected scan type")
		}
	}
	return nil
}

// fakeRows is the narrow slice of pgx.Rows an operation actually uses.
type fakeRows struct {
	values [][]any
	cursor int
	err    error
	closed bool
}

func (r *fakeRows) Next() bool {
	if r.cursor >= len(r.values) {
		return false
	}
	r.cursor++
	return true
}

func (r *fakeRows) Scan(dest ...any) error {
	row := r.values[r.cursor-1]
	if len(dest) != len(row) {
		return errors.New("unexpected scan width")
	}
	for index, target := range dest {
		switch typed := target.(type) {
		case *int64:
			*typed = row[index].(int64)
		case *int32:
			*typed = row[index].(int32)
		case *string:
			*typed = row[index].(string)
		case *float64:
			*typed = row[index].(float64)
		case *bool:
			*typed = row[index].(bool)
		case **int64:
			*typed, _ = row[index].(*int64)
		case **string:
			*typed, _ = row[index].(*string)
		default:
			return errors.New("unexpected scan type")
		}
	}
	return nil
}

func (r *fakeRows) Close()                                       { r.closed = true }
func (r *fakeRows) Err() error                                   { return r.err }
func (r *fakeRows) CommandTag() pgconn.CommandTag                { return pgconn.CommandTag{} }
func (r *fakeRows) FieldDescriptions() []pgconn.FieldDescription { return nil }
func (r *fakeRows) Values() ([]any, error)                       { return nil, nil }
func (r *fakeRows) RawValues() [][]byte                          { return nil }
func (r *fakeRows) Conn() *pgx.Conn                              { return nil }

func invocation(stage uint32) bus.ModuleInvocation {
	return bus.ModuleInvocation{StageID: stage}
}

func TestDispatchRefusesAnUnregisteredOperation(t *testing.T) {
	// Capability-absent, matching what the C module answers for a backend it
	// was not given -- an operation nobody has ported yet is exactly that, and
	// it must not read as an internal fault.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeProspectiveListArmedRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageProspectiveListArmed), request); status !=
		bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v, want capability absent", status)
	}
}

func TestDispatchRefusesAMalformedFrame(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	if _, status := handler(invocation(db2contract.FamilyMaintenance), []byte{1, 2, 3}); status !=
		bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want invalid request", status)
	}
}

func TestDispatchRefusesWithoutAStore(t *testing.T) {
	handler := NewDispatchHandler(nil)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageCuratorInvalidationsSince), request); status !=
		bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v, want capability absent", status)
	}
}

func TestDispatchRoutesOnTheFamilyAndTheOperation(t *testing.T) {
	// An operation id is unique only within its family, so a request carrying
	// the right id under the wrong stage must not reach the implementation.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.FamilyMemory), request); status !=
		bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("wrong-family status = %v, want capability absent", status)
	}
	if store.lastSQL != "" {
		t.Fatal("a misrouted request reached the store")
	}
}

func TestCuratorInvalidationsSinceReadsAndEncodes(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "document", "doc-1", int32(2), "2026-08-22T09:00:00Z"},
		{int64(9), "project", "proj-1", int32(0), "2026-08-22T09:05:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCuratorInvalidationsSince), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeCuratorInvalidationsSinceReply(body)
	if err != nil {
		t.Fatalf("decode reply: %v", err)
	}
	if len(rows) != 2 || rows[0].InvalidationID != 4 || rows[0].SourceKind != "document" ||
		rows[0].ArtifactsStale != 2 || rows[1].InvalidationID != 9 ||
		rows[1].InvalidatedAt != "2026-08-22T09:05:00Z" {
		t.Fatalf("rows = %+v", rows)
	}
	// The cursor argument reaches the statement, which is the difference
	// between a cursor read and a read of everything.
	if len(store.lastArgs) != 2 || store.lastArgs[0] != int64(3) {
		t.Fatalf("args = %v", store.lastArgs)
	}
	if !store.rows.closed {
		t.Error("rows were not closed")
	}
}

func TestCuratorInvalidationsSinceEmptyIsAnAnswer(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCuratorInvalidationsSince), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeCuratorInvalidationsSinceReply(body)
	if err != nil || len(rows) != 0 {
		t.Fatalf("rows = %+v, err = %v", rows, err)
	}
}

func TestCuratorInvalidationsSinceRefusesATruncatedRead(t *testing.T) {
	// rows.Next reports false for "no more rows" AND for a failure mid-stream.
	// Without the Err check after the loop a truncated read encodes as a short
	// page, which a cursor caller would accept and advance past.
	store := &fakeStore{rows: &fakeRows{
		values: [][]any{{int64(4), "document", "doc-1", int32(2), "2026-08-22T09:00:00Z"}},
		err:    errors.New("connection lost"),
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageCuratorInvalidationsSince), request); status !=
		bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

func TestCuratorInvalidationsSinceReportsAQueryFailure(t *testing.T) {
	store := &fakeStore{queryErr: errors.New("no connection")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidationsSinceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageCuratorInvalidationsSince), request); status !=
		bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

func TestRegisterRefusesADuplicate(t *testing.T) {
	defer func() {
		if recover() == nil {
			t.Fatal("registering an operation twice did not panic")
		}
	}()
	Register(db2contract.StageCuratorInvalidationsSince,
		db2contract.OperationCuratorInvalidationsSince, curatorInvalidationsSince)
}

func TestImplementedCountsWhatIsPorted(t *testing.T) {
	if Implemented() < 1 {
		t.Fatal("no operations registered")
	}
}
