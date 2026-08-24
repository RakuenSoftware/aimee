package db2

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	protocol "github.com/JBailes/aimee/server-go/vector"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

type db3FakeRow struct {
	values []any
	err    error
}

func assignDB3Fake(dest, value any) error {
	target := reflect.ValueOf(dest)
	if target.Kind() != reflect.Pointer || target.IsNil() {
		return errors.New("invalid destination")
	}
	source := reflect.ValueOf(value)
	if !source.IsValid() {
		target.Elem().SetZero()
		return nil
	}
	if source.Type().AssignableTo(target.Elem().Type()) {
		target.Elem().Set(source)
		return nil
	}
	if source.Type().ConvertibleTo(target.Elem().Type()) {
		target.Elem().Set(source.Convert(target.Elem().Type()))
		return nil
	}
	return errors.New("incompatible fake value")
}

func (row db3FakeRow) Scan(dest ...any) error {
	if row.err != nil {
		return row.err
	}
	if len(dest) != len(row.values) {
		return errors.New("fake row width")
	}
	for index := range dest {
		if err := assignDB3Fake(dest[index], row.values[index]); err != nil {
			return err
		}
	}
	return nil
}

type db3FakeRows struct {
	data   [][]any
	index  int
	closed bool
	err    error
}

func (rows *db3FakeRows) Close()                                       { rows.closed = true }
func (rows *db3FakeRows) Err() error                                   { return rows.err }
func (rows *db3FakeRows) CommandTag() pgconn.CommandTag                { return pgconn.NewCommandTag("SELECT") }
func (rows *db3FakeRows) FieldDescriptions() []pgconn.FieldDescription { return nil }
func (rows *db3FakeRows) Values() ([]any, error) {
	if rows.index == 0 || rows.index > len(rows.data) {
		return nil, errors.New("no current row")
	}
	return rows.data[rows.index-1], nil
}
func (rows *db3FakeRows) RawValues() [][]byte { return nil }
func (rows *db3FakeRows) Conn() *pgx.Conn     { return nil }
func (rows *db3FakeRows) Next() bool {
	if rows.index >= len(rows.data) {
		rows.closed = true
		return false
	}
	rows.index++
	return true
}
func (rows *db3FakeRows) Scan(dest ...any) error {
	if rows.index == 0 || rows.index > len(rows.data) {
		return errors.New("no current row")
	}
	return db3FakeRow{values: rows.data[rows.index-1]}.Scan(dest...)
}

type db3FakeSQL struct {
	querySQL  string
	queryArgs []any
	rows      pgx.Rows
	queryErr  error
	row       pgx.Row
	rowQueue  []pgx.Row
	execs     []db3SQLCall
}

type db3SQLCall struct {
	sql  string
	args []any
}

func (db *db3FakeSQL) Query(_ context.Context, sql string, args ...any) (pgx.Rows, error) {
	db.querySQL, db.queryArgs = sql, append([]any(nil), args...)
	return db.rows, db.queryErr
}
func (db *db3FakeSQL) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	db.querySQL, db.queryArgs = sql, append([]any(nil), args...)
	if len(db.rowQueue) > 0 {
		row := db.rowQueue[0]
		db.rowQueue = db.rowQueue[1:]
		return row
	}
	return db.row
}
func (db *db3FakeSQL) Exec(_ context.Context, sql string, args ...any) (pgconn.CommandTag, error) {
	db.execs = append(db.execs, db3SQLCall{sql: sql, args: append([]any(nil), args...)})
	return pgconn.NewCommandTag("UPDATE 1"), nil
}

func TestPGVectorOutboxClaimsCanonicalLabeledAndDeleteOperations(t *testing.T) {
	rows := &db3FakeRows{data: [][]any{
		{uint64(11), uint64(7), int64(41), "upsert", "memory", "[0.1, 0.2,0.3]",
			[]byte(`{"workspace":"w","project":"p","record_type":"memory"}`)},
		{uint64(12), uint64(7), int64(42), "delete", "memory", "", []byte(`{}`)},
	}}
	database := &db3FakeSQL{rows: rows}
	store, err := NewPGVectorOutbox(database, "worker-1")
	if err != nil {
		t.Fatal(err)
	}
	operations, err := store.Claim(context.Background(), 2)
	if err != nil {
		t.Fatal(err)
	}
	if !rows.closed || !strings.Contains(database.querySQL, "FOR UPDATE SKIP LOCKED") ||
		len(database.queryArgs) != 3 {
		t.Fatalf("claim query = %q args=%v closed=%v", database.querySQL, database.queryArgs, rows.closed)
	}
	if len(operations) != 2 || operations[0].Kind != protocol.ApplyUpsert ||
		operations[1].Kind != protocol.ApplyDelete ||
		!reflect.DeepEqual(operations[0].Vector, []float32{0.1, 0.2, 0.3}) {
		t.Fatalf("operations = %+v", operations)
	}
	wantLabels := []protocol.ExactLabel{
		{Key: "project", Value: "p"},
		{Key: "record_type", Value: "memory"},
		{Key: "workspace", Value: "w"},
	}
	if !reflect.DeepEqual(operations[0].Labels, wantLabels) || operations[1].Labels != nil {
		t.Fatalf("labels = %+v / %+v", operations[0].Labels, operations[1].Labels)
	}
}

func TestPGVectorOutboxRejectsMalformedDurableRows(t *testing.T) {
	for name, row := range map[string][]any{
		"vector": {uint64(11), uint64(7), int64(41), "upsert", "memory", "[NaN]",
			[]byte(`{"project":"p"}`)},
		"labels": {uint64(11), uint64(7), int64(41), "upsert", "memory", "[0.1]",
			[]byte(`{"project":7}`)},
		"kind": {uint64(11), uint64(7), int64(41), "unknown", "memory", "", []byte(`{}`)},
	} {
		t.Run(name, func(t *testing.T) {
			store, _ := NewPGVectorOutbox(&db3FakeSQL{rows: &db3FakeRows{data: [][]any{row}}},
				"worker")
			if _, err := store.Claim(context.Background(), 1); !errors.Is(err, ErrVectorMalformedRow) {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

func TestPGVectorOutboxAdmitAndAppliedAreFailClosed(t *testing.T) {
	capabilities := protocol.Capabilities{
		Generation: 7, Operations: protocol.OperationApply, MaxBatch: 16, Ready: true,
	}
	database := &db3FakeSQL{row: db3FakeRow{values: []any{2}}}
	store, _ := NewPGVectorOutbox(database, "worker")
	if err := store.AdmitProvider(context.Background(), 1001, 41, 9, capabilities); err != nil {
		t.Fatal(err)
	}
	if len(database.queryArgs) != 5 || database.queryArgs[0] != uint32(1001) ||
		!strings.Contains(database.querySQL, "db3_admit_provider") {
		t.Fatalf("admit query args = %v", database.queryArgs)
	}
	database.row = db3FakeRow{values: []any{1}}
	if err := store.AdmitProvider(context.Background(), 1001, 41, 10, capabilities); !errors.Is(err, ErrVectorProviderNotCaughtUp) {
		t.Fatalf("catch-up gate = %v", err)
	}
	database.row = db3FakeRow{values: []any{0}}
	if err := store.AdmitProvider(context.Background(), 1002, 42, 1, capabilities); !errors.Is(err, ErrVectorCorpusGeneration) {
		t.Fatalf("generation conflict = %v", err)
	}

	database.row = db3FakeRow{values: []any{true}}
	applied := protocol.Applied{
		OperationID: 11, Generation: 7, Watermark: 11, Result: protocol.AppliedOK,
	}
	if err := store.Applied(context.Background(), 1001, applied); err != nil {
		t.Fatal(err)
	}
	if len(database.queryArgs) != 5 || database.queryArgs[4] != uint64(11) {
		t.Fatalf("applied args = %v", database.queryArgs)
	}
	database.row = db3FakeRow{values: []any{false}}
	if err := store.Applied(context.Background(), 1001, applied); !errors.Is(err, ErrVectorUnknownAppliedAck) {
		t.Fatalf("unknown ack = %v", err)
	}
}

func TestPGVectorOutboxBackfillUsesBoundedIndependentTransactions(t *testing.T) {
	database := &db3FakeSQL{rowQueue: []pgx.Row{
		db3FakeRow{values: []any{1}},
		db3FakeRow{values: []any{1}},
		db3FakeRow{values: []any{0}},
	}}
	store, _ := NewPGVectorOutbox(database, "worker")
	if err := store.BackfillProvider(context.Background(), 1001); err != nil {
		t.Fatal(err)
	}
	if len(database.rowQueue) != 0 ||
		!strings.Contains(database.querySQL, "db3_backfill_provider_chunk") ||
		!reflect.DeepEqual(database.queryArgs, []any{uint32(1001), db3BackfillLimit}) {
		t.Fatalf("query=%q args=%v rows=%d", database.querySQL,
			database.queryArgs, len(database.rowQueue))
	}
}

func TestPGVectorOutboxBackfillRetriesAndExposesTransientFailure(t *testing.T) {
	transient := errors.New("connection unavailable")
	database := &db3FakeSQL{rowQueue: []pgx.Row{
		db3FakeRow{err: transient},
		db3FakeRow{values: []any{1}},
		db3FakeRow{values: []any{0}},
	}}
	store, _ := NewPGVectorOutbox(database, "worker")
	store.backfillRetry = time.Millisecond
	if err := store.runBackfillWorker(context.Background(), 1001); err != nil {
		t.Fatal(err)
	}
	if !errors.Is(store.LastBackfillError(1001), transient) || len(database.rowQueue) != 0 {
		t.Fatalf("last error=%v rows=%d", store.LastBackfillError(1001), len(database.rowQueue))
	}
}

func TestPGVectorOutboxBackfillStopsAndExposesTerminalFailure(t *testing.T) {
	terminal := &pgconn.PgError{Code: "23514", Message: "invalid provider state"}
	database := &db3FakeSQL{row: db3FakeRow{err: terminal}}
	store, _ := NewPGVectorOutbox(database, "worker")
	store.startBackfill(context.Background(), 1001)
	for range 100 {
		if errors.Is(store.LastBackfillError(1001), terminal) {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("terminal error was not exposed: %v", store.LastBackfillError(1001))
}

func TestPGVectorOutboxBackfillRetryStopsOnCancellation(t *testing.T) {
	database := &db3FakeSQL{row: db3FakeRow{err: errors.New("database unavailable")}}
	store, _ := NewPGVectorOutbox(database, "worker")
	store.backfillRetry = time.Hour
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := store.runBackfillWorker(ctx, 1001); !errors.Is(err, context.Canceled) {
		t.Fatalf("worker error = %v", err)
	}
}

func TestPGVectorOutboxPublishedRecordsReplayDeadlineAndTimestamp(t *testing.T) {
	database := &db3FakeSQL{}
	store, _ := NewPGVectorOutbox(database, "worker")
	if err := store.Published(context.Background(), 11); err != nil {
		t.Fatal(err)
	}
	if len(database.execs) != 1 ||
		!strings.Contains(database.execs[0].sql, "published_at=pg_catalog.clock_timestamp()") ||
		!strings.Contains(database.execs[0].sql, "lease_owner='',lease_until=NULL") ||
		database.execs[0].args[2] != db3AckRetryDelay.Milliseconds() {
		t.Fatalf("published call = %+v", database.execs)
	}
}

type db3DispatcherStore struct {
	cancel    context.CancelFunc
	mu        sync.Mutex
	claims    int
	markError uint64
	published []uint64
	released  []uint64
}

func (store *db3DispatcherStore) Claim(ctx context.Context, _ int) ([]protocol.Apply, error) {
	store.mu.Lock()
	defer store.mu.Unlock()
	store.claims++
	if store.claims == 1 {
		return []protocol.Apply{
			{OperationID: 1, Generation: 7, PointID: 1, Kind: protocol.ApplyDelete, Collection: "memory"},
			{OperationID: 2, Generation: 7, PointID: 2, Kind: protocol.ApplyDelete, Collection: "memory"},
		}, nil
	}
	store.cancel()
	return nil, ctx.Err()
}
func (store *db3DispatcherStore) Published(_ context.Context, operation uint64) error {
	store.mu.Lock()
	defer store.mu.Unlock()
	store.published = append(store.published, operation)
	if operation == store.markError {
		return errors.New("ledger unavailable")
	}
	return nil
}
func (store *db3DispatcherStore) Release(_ context.Context, operation uint64, _ error) error {
	store.mu.Lock()
	defer store.mu.Unlock()
	store.released = append(store.released, operation)
	return nil
}

func TestRunVectorOutboxReleasesPublishWhenLedgerMarkFails(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	store := &db3DispatcherStore{cancel: cancel, markError: 1}
	publisher := &db3DispatcherPublisher{}
	err := RunVectorOutbox(ctx, store, publisher)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("run error = %v", err)
	}
	if !reflect.DeepEqual(publisher.seen, []uint64{1, 2}) ||
		!reflect.DeepEqual(store.published, []uint64{1}) ||
		!reflect.DeepEqual(store.released, []uint64{1, 2}) {
		t.Fatalf("seen=%v published=%v released=%v",
			publisher.seen, store.published, store.released)
	}
}

type db3DispatcherPublisher struct {
	seen []uint64
}

func (publisher *db3DispatcherPublisher) PublishApply(_ context.Context,
	apply protocol.Apply) error {
	publisher.seen = append(publisher.seen, apply.OperationID)
	if apply.OperationID == 2 {
		return errors.New("blocked")
	}
	return nil
}

func TestRunVectorOutboxRecordsPublishAndReleaseSeparately(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	store := &db3DispatcherStore{cancel: cancel}
	publisher := &db3DispatcherPublisher{}
	err := RunVectorOutbox(ctx, store, publisher)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("run error = %v", err)
	}
	if !reflect.DeepEqual(publisher.seen, []uint64{1, 2}) ||
		!reflect.DeepEqual(store.published, []uint64{1}) ||
		!reflect.DeepEqual(store.released, []uint64{2}) {
		t.Fatalf("seen=%v published=%v released=%v",
			publisher.seen, store.published, store.released)
	}
}

func TestPGVectorOutboxConfigurationValidation(t *testing.T) {
	if _, err := NewPGVectorOutbox(nil, "worker"); !errors.Is(err, ErrVectorOutboxConfig) {
		t.Fatalf("nil db = %v", err)
	}
	if _, err := NewPGVectorOutbox(&db3FakeSQL{}, "bad worker"); !errors.Is(err, ErrVectorOutboxConfig) {
		t.Fatalf("bad owner = %v", err)
	}
	if err := RunVectorOutbox(nil, nil, nil); !errors.Is(err, ErrVectorOutboxConfig) {
		t.Fatalf("nil dispatcher = %v", err)
	}
}
