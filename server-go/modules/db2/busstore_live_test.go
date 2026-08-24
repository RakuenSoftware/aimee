package db2

import (
	"context"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	modulepg "github.com/JBailes/aimee/server-go/modules/postgres"
	storage "github.com/JBailes/aimee/server-go/postgres"
	"github.com/jackc/pgx/v5"
)

// The operations, running through the postgres module instead of their own pool.
//
// This is the architecture rather than an optimisation: the postgres module owns
// PostgreSQL, and db2 holding a second pool would mean two owners of one
// database. The 445 operations are unchanged -- they are written against Store,
// and BusStore is a Store -- so what these prove is that the adapter carries
// their values faithfully across the wire.
//
//	AIMEE_DB2_URL=postgres://... go test ./modules/db2/ -run BusStoreLive

func liveBusStore(t *testing.T) *BusStore {
	t.Helper()
	if os.Getenv("AIMEE_DB2_URL") == "" {
		t.Skip("set AIMEE_DB2_URL to run the bus-backed store suite")
	}
	handler := modulepg.NewSQLHandler()
	invocation := bus.ModuleInvocation{
		StageID: modulepg.StageSQL, PrincipalRef: 29, SrcHandle: 1}
	client := storage.New(func(ctx context.Context, body []byte) ([]byte, error) {
		reply, status := handler(invocation, body)
		if status != bus.ModuleStatusOK {
			t.Fatalf("module status = %v", status)
		}
		return reply, nil
	})
	return NewBusStore(client).ForOperation("db2_live")
}

func TestBusStoreLiveCarriesEveryValueKind(t *testing.T) {
	store := liveBusStore(t)
	ctx := context.Background()
	if _, err := store.Exec(ctx,
		`CREATE TABLE IF NOT EXISTS bs (id BIGINT PRIMARY KEY, name TEXT, weight DOUBLE PRECISION, live BOOLEAN, raw BYTEA, note TEXT)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := store.Exec(ctx, `DELETE FROM bs`); err != nil {
		t.Fatalf("clear: %v", err)
	}
	if _, err := store.Exec(ctx,
		`INSERT INTO bs (id, name, weight, live, raw, note) VALUES ($1, $2, $3, $4, $5, $6)`,
		int64(1), "a", 2.5, true, []byte{0x00, 0xff}, nil); err != nil {
		t.Fatalf("insert: %v", err)
	}

	// Scanned the way the operations scan: value destinations for the columns
	// that cannot be NULL, a pointer for the one that can.
	var id int64
	var name string
	var weight float64
	var live bool
	var raw []byte
	var note *string
	if err := store.QueryRow(ctx,
		`SELECT id, name, weight, live, raw, note FROM bs WHERE id = $1`,
		int64(1)).Scan(&id, &name, &weight, &live, &raw, &note); err != nil {
		t.Fatalf("scan: %v", err)
	}
	if id != 1 || name != "a" || weight != 2.5 || !live ||
		string(raw) != "\x00\xff" || note != nil {
		t.Fatalf("row = %d %q %v %v %#v %v", id, name, weight, live, raw, note)
	}
}

func TestBusStoreLiveRefusesNullIntoAValueDestination(t *testing.T) {
	// The rule the wire exists to protect, at the point an operation reads.
	// Writing a zero here would record "" where the table said NULL, and the
	// operation that forgot a nullable column would never find out.
	store := liveBusStore(t)
	ctx := context.Background()
	if _, err := store.Exec(ctx,
		`CREATE TABLE IF NOT EXISTS bs_null (id BIGINT PRIMARY KEY, note TEXT)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := store.Exec(ctx, `DELETE FROM bs_null`); err != nil {
		t.Fatalf("clear: %v", err)
	}
	if _, err := store.Exec(ctx,
		`INSERT INTO bs_null (id, note) VALUES ($1, $2)`, int64(1), nil); err != nil {
		t.Fatalf("insert: %v", err)
	}
	var note string
	err := store.QueryRow(ctx, `SELECT note FROM bs_null WHERE id = 1`).Scan(&note)
	if err == nil {
		t.Fatalf("NULL scanned into a string as %q instead of refusing", note)
	}
}

func TestBusStoreLiveReportsNoRows(t *testing.T) {
	// Translated to pgx's sentinel because every operation compares against it.
	// A second spelling of "no rows" would be a second thing all 445 have to
	// know.
	store := liveBusStore(t)
	ctx := context.Background()
	if _, err := store.Exec(ctx,
		`CREATE TABLE IF NOT EXISTS bs_empty (id BIGINT PRIMARY KEY)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	var id int64
	if err := store.QueryRow(ctx,
		`SELECT id FROM bs_empty WHERE id = -1`).Scan(&id); err != pgx.ErrNoRows {
		t.Fatalf("err = %v, want pgx.ErrNoRows", err)
	}
}

func TestBusStoreLiveTransactionRollsBack(t *testing.T) {
	// Every statement inside carries the handle, which is why fn receives a
	// Store rather than closing over the outer one: a statement sent without
	// the handle runs outside the transaction and commits on its own.
	store := liveBusStore(t)
	ctx := context.Background()
	if _, err := store.Exec(ctx,
		`CREATE TABLE IF NOT EXISTS bs_tx (id BIGINT PRIMARY KEY)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := store.Exec(ctx, `DELETE FROM bs_tx`); err != nil {
		t.Fatalf("clear: %v", err)
	}

	deliberate := errString("rolled back on purpose")
	if err := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, `INSERT INTO bs_tx (id) VALUES (1)`); err != nil {
			return err
		}
		return deliberate
	}); err != deliberate {
		t.Fatalf("InTx returned %v, want the body's own error", err)
	}

	var count int64
	if err := store.QueryRow(ctx, `SELECT COUNT(*) FROM bs_tx`).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 0 {
		t.Fatalf("%d row(s) survived a rollback", count)
	}

	if err := store.InTx(ctx, func(tx Store) error {
		_, err := tx.Exec(ctx, `INSERT INTO bs_tx (id) VALUES (2)`)
		return err
	}); err != nil {
		t.Fatalf("commit: %v", err)
	}
	if err := store.QueryRow(ctx, `SELECT COUNT(*) FROM bs_tx`).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 1 {
		t.Fatalf("%d row(s) after a commit, want 1", count)
	}
}

func TestBusStoreLiveRowsIterate(t *testing.T) {
	store := liveBusStore(t)
	ctx := context.Background()
	if _, err := store.Exec(ctx,
		`CREATE TABLE IF NOT EXISTS bs_many (id BIGINT PRIMARY KEY)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := store.Exec(ctx, `DELETE FROM bs_many`); err != nil {
		t.Fatalf("clear: %v", err)
	}
	for index := int64(1); index <= 3; index++ {
		if _, err := store.Exec(ctx,
			`INSERT INTO bs_many (id) VALUES ($1)`, index); err != nil {
			t.Fatalf("insert %d: %v", index, err)
		}
	}
	rows, err := store.Query(ctx, `SELECT id FROM bs_many ORDER BY id`)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	defer rows.Close()
	var seen []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			t.Fatalf("scan: %v", err)
		}
		seen = append(seen, id)
	}
	if rows.Err() != nil {
		t.Fatalf("rows: %v", rows.Err())
	}
	if len(seen) != 3 || seen[0] != 1 || seen[2] != 3 {
		t.Fatalf("rows = %v", seen)
	}
}

type errString string

func (e errString) Error() string { return string(e) }
