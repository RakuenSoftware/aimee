package db2

import (
	"context"
	"errors"
	"fmt"
	"math"
	"time"

	storage "github.com/JBailes/aimee/server-go/postgres"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// A Store that reaches PostgreSQL through the postgres module rather than
// through a pool of its own.
//
// This is the architecture the tree is moving to, and saying so in the present
// tense would be false today. ProductionStore still opens a pool, and the
// DEPLOYED db2 is the C build -- db2 declares runtime "c" in
// process-contracts.json, and that library is linked into the KB and connects
// through libpq. Nothing in production reaches PostgreSQL through this type;
// AIMEE_DB2_STORE=bus selects it, and the parity suites are what run it.
//
// Why it is the direction: a second module holding its own pool means two owners
// of one database, two transaction lifetimes, and two opinions about how many
// connections exist. The operations above are unchanged either way -- they are
// written against Store, and this is a Store, which is the whole reason the
// switch is one line rather than a rewrite.
//
// What gates the move is not this type. 168 files outside the module call db2_*
// in-process, and db2 calls back into the KB through registered providers, so
// the callers have to move to the bus before the runtime can.
//
// The pgx types in Store's signature are kept rather than refactored away. 445
// operations scan into them, and changing the interface would be a change to
// every one of them for no behavioural gain; adapting at the boundary is one
// file.

// BusStore satisfies Store over the storage wire.
type BusStore struct {
	client *storage.Client
	// The operation being served, which becomes the statement_id the module
	// records. Set per invocation rather than per call site, so an operation
	// cannot forget it and the module always knows what a caller was doing.
	statementID string
	// Non-zero inside a transaction. Every statement carries it, because this
	// holds no connection: the transaction lives in the postgres module.
	handle uint64
}

// NewBusStore binds a Store to the storage client.
func NewBusStore(client *storage.Client) *BusStore {
	return &BusStore{client: client, statementID: "db2"}
}

// ForOperation returns a Store that names the operation it is serving.
func (s *BusStore) ForOperation(name string) *BusStore {
	if name == "" {
		name = "db2"
	}
	copy := *s
	copy.statementID = name
	return &copy
}

func (s *BusStore) Exec(ctx context.Context, sql string, args ...any) (int64, error) {
	values, err := toValues(args)
	if err != nil {
		return 0, err
	}
	affected, err := s.client.Exec(ctx, s.statementID, s.handle, sql, values...)
	return int64(affected), err
}

func (s *BusStore) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	values, err := toValues(args)
	if err != nil {
		return nil, err
	}
	rows, err := s.client.Query(ctx, s.statementID, s.handle, sql, values...)
	if err != nil {
		return nil, err
	}
	return &busRows{rows: rows, at: -1}, nil
}

func (s *BusStore) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	values, err := toValues(args)
	if err != nil {
		return errRow{err: err}
	}
	row, err := s.client.QueryRow(ctx, s.statementID, s.handle, sql, values...)
	if err != nil {
		if errors.Is(err, storage.ErrNoRows) {
			// Translated to pgx's sentinel because every operation above
			// compares against it. A second spelling of "no rows" would be a
			// second thing each of them has to know.
			return errRow{err: pgx.ErrNoRows}
		}
		return errRow{err: err}
	}
	return &busRow{cells: row}
}

// InTx runs fn inside a transaction held by the postgres module.
//
// The handle rides on every statement fn issues, which is why fn receives a
// Store rather than using the outer one: a statement sent without the handle
// would run outside the transaction and commit on its own.
func (s *BusStore) InTx(ctx context.Context, fn func(Store) error) error {
	handle, err := s.client.Begin(ctx, s.statementID)
	if err != nil {
		return err
	}
	inner := *s
	inner.handle = handle
	if err := fn(&inner); err != nil {
		// The rollback's own error is discarded deliberately: the caller needs
		// to know why the work failed, and a rollback that also failed is a
		// second fact about a transaction ending either way.
		_ = s.client.Rollback(ctx, s.statementID, handle)
		return err
	}
	return s.client.Commit(ctx, s.statementID, handle)
}

// toValues converts what an operation passed into wire values.
//
// Every type here appears in the 445 operations. An unrecognised one is an
// error rather than a fmt.Sprint, because sending a rendering of a value is how
// a typed wire quietly becomes a text one.
func toValues(args []any) ([]storage.Value, error) {
	out := make([]storage.Value, 0, len(args))
	for index, arg := range args {
		switch typed := arg.(type) {
		case nil:
			out = append(out, storage.Null())
		case string:
			out = append(out, storage.Text(typed))
		case int:
			out = append(out, storage.Int(int64(typed)))
		case int32:
			out = append(out, storage.Int(int64(typed)))
		case int64:
			out = append(out, storage.Int(typed))
		case uint32:
			out = append(out, storage.Int(int64(typed)))
		case uint64:
			out = append(out, storage.Int(int64(typed)))
		case float32:
			out = append(out, storage.Float(float64(typed)))
		case float64:
			out = append(out, storage.Float(typed))
		case bool:
			out = append(out, storage.Bool(typed))
		case []byte:
			out = append(out, storage.Bytes(typed))
		case []string:
			out = append(out, storage.Texts(typed))
		case *string:
			if typed == nil {
				out = append(out, storage.Null())
			} else {
				out = append(out, storage.Text(*typed))
			}
		case *int64:
			if typed == nil {
				out = append(out, storage.Null())
			} else {
				out = append(out, storage.Int(*typed))
			}
		case *float64:
			if typed == nil {
				out = append(out, storage.Null())
			} else {
				out = append(out, storage.Float(*typed))
			}
		case time.Time:
			out = append(out, storage.Text(typed.UTC().Format(
				"2006-01-02T15:04:05Z")))
		default:
			return nil, fmt.Errorf(
				"db2: argument %d is a %T, which the storage wire has no type for",
				index, arg)
		}
	}
	return out, nil
}

// busRows adapts a result set to pgx.Rows.
//
// Only Next, Scan, Err and Close are used by the operations above; the rest of
// the interface is present because pgx.Rows requires it. They are implemented
// honestly rather than left to panic, so a future caller that reaches for one
// gets an empty answer rather than a crash.
type busRows struct {
	rows storage.Rows
	at   int
	err  error
}

func (r *busRows) Next() bool {
	if r.err != nil {
		return false
	}
	r.at++
	return r.at < len(r.rows)
}

func (r *busRows) Scan(dest ...any) error {
	if r.at < 0 || r.at >= len(r.rows) {
		return errors.New("db2: Scan called without a row")
	}
	if err := scanCells(r.rows[r.at], dest); err != nil {
		r.err = err
		return err
	}
	return nil
}

func (r *busRows) Err() error                                   { return r.err }
func (r *busRows) Close()                                       {}
func (r *busRows) CommandTag() pgconn.CommandTag                { return pgconn.CommandTag{} }
func (r *busRows) FieldDescriptions() []pgconn.FieldDescription { return nil }
func (r *busRows) RawValues() [][]byte                          { return nil }
func (r *busRows) Conn() *pgx.Conn                              { return nil }

func (r *busRows) Values() ([]any, error) {
	if r.at < 0 || r.at >= len(r.rows) {
		return nil, errors.New("db2: Values called without a row")
	}
	out := make([]any, 0, len(r.rows[r.at]))
	for _, cell := range r.rows[r.at] {
		out = append(out, cellValue(cell))
	}
	return out, nil
}

type busRow struct{ cells storage.Row }

func (r *busRow) Scan(dest ...any) error { return scanCells(r.cells, dest) }

type errRow struct{ err error }

func (r errRow) Scan(dest ...any) error { return r.err }

func cellValue(cell storage.Value) any {
	switch cell.Type {
	case 1:
		return cell.Text
	case 2:
		return cell.Int
	case 3:
		return cell.Float
	case 4:
		return cell.Bool
	case 6:
		return cell.Bytes
	}
	return nil
}

// scanCells writes one row into the destinations an operation supplied.
//
// NULL into a destination that cannot hold it is an ERROR, not a zero. A
// pointer destination accepts it; a value destination does not. That is the
// same rule the wire enforces and for the same reason: reading "" where the
// table said NULL loses the difference the whole type set exists to keep, and
// an operation that forgot a nullable column should fail loudly rather than
// record an empty string as fact.
func scanCells(cells storage.Row, dest []any) error {
	if len(dest) != len(cells) {
		return fmt.Errorf("db2: scanning %d cells into %d destinations",
			len(cells), len(dest))
	}
	for index, target := range dest {
		if err := scanCell(cells[index], target); err != nil {
			return fmt.Errorf("db2: column %d: %w", index, err)
		}
	}
	return nil
}

// mismatch names a cell that cannot become this destination.
//
// Its own function because the message is the useful part: the alternative was
// a zero value, which reads as data.
func mismatch(cell storage.Value, want string) error {
	return fmt.Errorf("a %s cell cannot be scanned into %s; a zero here would "+
		"read as data", cellTypeName(cell.Type), want)
}

func cellTypeName(kind uint8) string {
	switch kind {
	case 0:
		return "NULL"
	case 1:
		return "text"
	case 2:
		return "integer"
	case 3:
		return "float"
	case 4:
		return "boolean"
	case 5:
		return "text-array"
	case 6:
		return "bytes"
	}
	return "unknown"
}

func scanCell(cell storage.Value, target any) error {
	null := cell.Type == 0
	switch typed := target.(type) {
	case *string:
		if null {
			return errors.New("NULL into *string; use **string or COALESCE")
		}
		// Checked, not assumed. Before this, a cell of any other type wrote the
		// zero value of a field nobody set: the empty string here, 0 for the
		// numeric destinations, and no error anywhere.
		if cell.Type != storage.TypeText {
			return mismatch(cell, "*string")
		}
		*typed = cell.Text
	case *int64:
		if null {
			return errors.New("NULL into *int64; use **int64 or COALESCE")
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "*int64")
		}
		*typed = cell.Int
	case *int32:
		// Five operations scan a priority or a count into this, and there was
		// no case for it: they failed over the wire and passed against a pool,
		// which is the whole disagreement the parity run exists to find.
		if null {
			return errors.New("NULL into *int32; use **int32 or COALESCE")
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "*int32")
		}
		if cell.Int > math.MaxInt32 || cell.Int < math.MinInt32 {
			return fmt.Errorf("%d does not fit in an int32", cell.Int)
		}
		*typed = int32(cell.Int)
	case *int:
		if null {
			return errors.New("NULL into *int; use **int or COALESCE")
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "*int")
		}
		// int is 64-bit everywhere this builds, but saying so in the type
		// system costs nothing and the check is free if it is.
		if cell.Int > math.MaxInt || cell.Int < math.MinInt {
			return fmt.Errorf("%d does not fit in an int", cell.Int)
		}
		*typed = int(cell.Int)
	case *uint32:
		if null {
			return errors.New("NULL into *uint32; use **uint32 or COALESCE")
		}
		// Checked rather than converted. A BIGINT of 5000000000 into a uint32
		// wraps to 705032704, and the operation goes on to use it: well-formed,
		// plausible, wrong, and silent. pgx refuses this; so does this now, or
		// the two paths disagree exactly where it matters most.
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "*uint32")
		}
		if cell.Int < 0 || cell.Int > math.MaxUint32 {
			return fmt.Errorf("%d does not fit in a uint32", cell.Int)
		}
		*typed = uint32(cell.Int)
	case *uint64:
		if null {
			return errors.New("NULL into *uint64; use **uint64 or COALESCE")
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "*uint64")
		}
		if cell.Int < 0 {
			return fmt.Errorf("%d is negative and cannot be a uint64", cell.Int)
		}
		*typed = uint64(cell.Int)
	case *float64:
		if null {
			return errors.New("NULL into *float64; use **float64 or COALESCE")
		}
		// An integer widens into a float destination, which is what pgx does and
		// what a caller scanning a count into a float means. The reverse is not
		// widened: a float into an integer is a truncation decision, and making
		// those silently is the thing this is here to stop.
		if cell.Type == storage.TypeInt {
			*typed = float64(cell.Int)
			return nil
		}
		if cell.Type != storage.TypeFloat {
			return mismatch(cell, "*float64")
		}
		*typed = cell.Float
	case *bool:
		if null {
			return errors.New("NULL into *bool; use **bool or COALESCE")
		}
		if cell.Type != storage.TypeBool {
			return mismatch(cell, "*bool")
		}
		*typed = cell.Bool
	case *[]byte:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type != storage.TypeBytes {
			return mismatch(cell, "*[]byte")
		}
		*typed = cell.Bytes
	// Pointer-to-pointer destinations are how an operation says a column is
	// nullable, and they are the only ones NULL may be written to.
	case **string:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type != storage.TypeText {
			return mismatch(cell, "**string")
		}
		v := cell.Text
		*typed = &v
	case **int64:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "**int64")
		}
		v := cell.Int
		*typed = &v
	case **int32:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type != storage.TypeInt {
			return mismatch(cell, "**int32")
		}
		if cell.Int > math.MaxInt32 || cell.Int < math.MinInt32 {
			return fmt.Errorf("%d does not fit in an int32", cell.Int)
		}
		v := int32(cell.Int)
		*typed = &v
	case **float64:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type == storage.TypeInt {
			widened := float64(cell.Int)
			*typed = &widened
			return nil
		}
		if cell.Type != storage.TypeFloat {
			return mismatch(cell, "**float64")
		}
		v := cell.Float
		*typed = &v
	case **bool:
		if null {
			*typed = nil
			return nil
		}
		if cell.Type != storage.TypeBool {
			return mismatch(cell, "**bool")
		}
		v := cell.Bool
		*typed = &v
	default:
		return fmt.Errorf("no conversion for %T", target)
	}
	return nil
}
