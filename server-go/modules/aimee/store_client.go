package aimee

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"
)

// The store, as another module.
//
// aimee keeps no database. It serves nineteen families of typed operations to its
// callers and, to store what they give it, calls the postgres module over the
// bus like any other module capability. That module owns the connection, the
// DSN and the pooling policy; this file asks it, and store_wire.go is the only
// place that knows how the asking is framed.
//
// Why a module and not a library: two processes holding the same store is the
// thing moving aimee behind a module removed. A pool opened here would put it
// straight back, and it would be invisible -- the module would look like a
// module and behave like a second writer.
//
// -----------------------------------------------------------------------------
// THE CONTRACT, agreed with the session building the postgres module. The stage
// numbering follows the bus rule with postgres at principal ref 28: health is
// stage 1 (11265), SQL is stage 2 (11266).
//
// NOT YET WIRED IN THE REGISTRY, on purpose. A stage may not be declared before
// it is served -- TestAdvertisedStagesMatchTheContractFile enforces exactly
// that, because a declared-but-unserved stage is worse than an absent one: the
// daemon routes to it and the caller gets a capability error from a module that
// is plainly running. Two entries land alongside the postgres module's SQL
// stage, in one commit:
//
//   * postgres gains stage 2 (postgres-sql, kind 11266) in
//     src/modules/process-contracts.json
//   * aimee-postgres joins its clients at principal ref 68, requesting that
//     kind and serving nothing, because a serving grant requests nothing
//
// Ref 68 rather than 67: 67 was claimed by the session building peer messaging,
// which could land its descriptor immediately while this one waits on the
// postgres module's SQL stage to exist in the same tree. Two clients on one ref
// are two callers the bus cannot tell apart.
//
// Until then this module has a store client and no store, which is the honest
// state and the one the daemon reports.
// -----------------------------------------------------------------------------

const (
	// PostgresPrincipalRef is the postgres module's principal.
	PostgresPrincipalRef = 28
	// StagePostgresSQL is the stage that answers SQL.
	StagePostgresSQL = 2
	// EventPostgresSQL is the kind that stage listens on.
	EventPostgresSQL = 4096 + PostgresPrincipalRef*256 + StagePostgresSQL

	// StoreDeadline bounds one call. The invocation's own remaining time is
	// used when there is one; this is the fallback.
	StoreDeadline = DefaultTimeout
)

// StageCaller is the bus call this client needs, as an interface so aimee's
// dispatch is testable without a bus.
type StageCaller interface {
	Call(ctx context.Context, kind, stage uint32, trace uint64,
		deadline time.Duration, request []byte) ([]byte, error)
}

// ErrStoreUnavailable is what every operation reports when the postgres module
// is not answering. Distinct from a refusal: the store said nothing, rather
// than saying no -- and only one of those is worth retrying.
var ErrStoreUnavailable = errors.New("aimee: the postgres module is not answering")

// Store is what the module holds at startup: the storage capability plus the
// migration one.
//
// Deliberately wider than the DB an operation is given. An operation gets DB
// and therefore CANNOT migrate -- the right to read and write rows does not
// carry the right to reshape them, on this side of the wire as much as on the
// store's, where MIGRATE is a separate grant.
type Store interface {
	DB
	// CurrentSchemaVersion reports how far an owner's history has been applied.
	CurrentSchemaVersion(ctx context.Context, owner string) (int64, string, error)
	// Migrate applies one version and records it, in one transaction.
	Migrate(ctx context.Context, m MigrationRequest) error
}

// NewStore returns the storage capability, backed by the postgres module.
func NewStore(caller StageCaller) (Store, error) {
	if caller == nil {
		return nil, errors.New("aimee: no bus caller for the postgres module")
	}
	return &storeDB{caller: caller}, nil
}

type storeDB struct {
	caller StageCaller
	// statementID is the operation being served, carried on every statement it
	// runs. Set by the dispatcher through WithStatementID; empty outside one.
	statementID string
}

// WithStatementID returns a view of the store that tags its statements with the
// operation being served.
//
// The store does not parse SQL, so this is the only thing that lets it log,
// rate-limit or authorize per operation rather than per connection. Taken from
// aimee's operation table -- the same names the catalog check verifies -- so it
// cannot drift from something real, and set once per dispatch rather than at
// each of five hundred call sites, where it would rot at the first copy-paste.
func (d *storeDB) WithStatementID(id string) DB {
	next := *d
	next.statementID = id
	return &next
}

// storeTx is one transaction, named by the handle the store gave back. The
// handle rides on every statement so the store puts it in the right
// transaction; aimee holds no connection to pin it to. The store binds the handle
// to this principal and reclaims it if aimee goes away.
type storeTx struct {
	db     *storeDB
	handle uint64
	mu     sync.Mutex
	done   bool
}

func (d *storeDB) Exec(ctx context.Context, sql string, args ...any) (Tag, error) {
	return d.exec(ctx, 0, sql, args)
}

func (d *storeDB) Query(ctx context.Context, sql string, args ...any) (Rows, error) {
	return d.query(ctx, 0, sql, args)
}

func (d *storeDB) QueryRow(ctx context.Context, sql string, args ...any) Row {
	return &storeRow{db: d, sql: sql, args: args, ctx: ctx}
}

func (d *storeDB) Begin(ctx context.Context) (Tx, error) {
	rep, err := d.call(ctx, opStoreBegin, "begin", 0, "", nil)
	if err != nil {
		return nil, err
	}
	handle, err := rep.r.u64()
	if err != nil {
		return nil, err
	}
	return &storeTx{db: d, handle: handle}, nil
}

func (t *storeTx) Exec(ctx context.Context, sql string, args ...any) (Tag, error) {
	return t.db.exec(ctx, t.handle, sql, args)
}

func (t *storeTx) Query(ctx context.Context, sql string, args ...any) (Rows, error) {
	return t.db.query(ctx, t.handle, sql, args)
}

func (t *storeTx) QueryRow(ctx context.Context, sql string, args ...any) Row {
	return &storeRow{db: t.db, handle: t.handle, sql: sql, args: args, ctx: ctx}
}

func (t *storeTx) Commit(ctx context.Context) error { return t.finish(ctx, opStoreCommit, "commit") }

func (t *storeTx) Rollback(ctx context.Context) error {
	return t.finish(ctx, opStoreRollback, "rollback")
}

// finish ends the transaction exactly once. The refusal path in Family.run
// rolls back and the caller may roll back again through a defer, so a second
// call reports ErrTxClosed rather than asking the store to end a transaction it
// has already forgotten.
func (t *storeTx) finish(ctx context.Context, op uint32, name string) error {
	t.mu.Lock()
	if t.done {
		t.mu.Unlock()
		return ErrTxClosed
	}
	t.done = true
	t.mu.Unlock()
	_, err := t.db.call(ctx, op, name, t.handle, "", nil)
	return err
}

func (d *storeDB) exec(ctx context.Context, handle uint64, sql string, args []any) (Tag, error) {
	rep, err := d.call(ctx, opStoreExec, d.statementID, handle, sql, args)
	if err != nil {
		return nil, err
	}
	n, err := rep.r.u64()
	if err != nil {
		return nil, err
	}
	return RowsAffected(int64(n)), nil
}

func (d *storeDB) query(ctx context.Context, handle uint64, sql string, args []any) (Rows, error) {
	rep, err := d.call(ctx, opStoreQuery, d.statementID, handle, sql, args)
	if err != nil {
		return nil, err
	}
	cells, width, err := rep.rows()
	if err != nil {
		return nil, err
	}
	return &storeRows{cells: cells, width: width, at: -1}, nil
}

// --- migration ---------------------------------------------------------------

// CurrentSchemaVersion asks how far an owner's schema history has been applied.
// A fresh database answers 0.
func (d *storeDB) CurrentSchemaVersion(ctx context.Context, owner string) (int64, string, error) {
	body, err := d.caller.Call(ctx, EventPostgresSQL, StagePostgresSQL, 0,
		d.deadline(ctx), encodeCurrentVersion(owner))
	if err != nil {
		return 0, "", ErrStoreUnavailable
	}
	rep, err := decodeReply(body, "current_version")
	if err != nil {
		return 0, "", err
	}
	version, err := rep.r.u64()
	if err != nil {
		return 0, "", err
	}
	sum, err := rep.r.str()
	if err != nil {
		return 0, "", err
	}
	return int64(version), sum, nil
}

// Migrate applies one versioned schema change and records that it ran, in one
// transaction. The store refuses a version whose checksum differs from the one
// it recorded, a version applied out of order, and a partial application.
func (d *storeDB) Migrate(ctx context.Context, m MigrationRequest) error {
	request, err := encodeMigrate(m)
	if err != nil {
		return err
	}
	body, err := d.caller.Call(ctx, EventPostgresSQL, StagePostgresSQL, 0,
		d.deadline(ctx), request)
	if err != nil {
		return ErrStoreUnavailable
	}
	_, err = decodeReply(body, fmt.Sprintf("migrate %s v%d", m.Owner, m.Version))
	return err
}

// --- the call ----------------------------------------------------------------

// deadline is the caller's remaining time, so a statement cannot outlive the
// request that asked for it. The store sets its statement_timeout from this.
func (d *storeDB) deadline(ctx context.Context) time.Duration {
	if until, ok := ctx.Deadline(); ok {
		if left := time.Until(until); left > 0 {
			return left
		}
		return time.Millisecond
	}
	return StoreDeadline
}

func (d *storeDB) call(ctx context.Context, op uint32, statementID string,
	handle uint64, sql string, args []any) (*storeReply, error) {
	request, err := encodeStatement(op, statementID, handle, sql, args)
	if err != nil {
		return nil, err
	}
	body, err := d.caller.Call(ctx, EventPostgresSQL, StagePostgresSQL, 0,
		d.deadline(ctx), request)
	if err != nil {
		return nil, ErrStoreUnavailable
	}
	name := statementID
	if name == "" {
		name = "statement"
	}
	return decodeReply(body, name)
}

// --- results -----------------------------------------------------------------

// storeRow defers the call to Scan, because that is where a caller can be told
// there was no row. A QueryRow that returned an error before Scan would have
// nowhere to report it: the interface has no error to return.
type storeRow struct {
	db     *storeDB
	handle uint64
	sql    string
	args   []any
	ctx    context.Context
}

func (r *storeRow) Scan(dest ...any) error {
	rows, err := r.db.query(r.ctx, r.handle, r.sql, r.args)
	if err != nil {
		return err
	}
	defer rows.Close()
	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return err
		}
		return ErrNoRows
	}
	return rows.Scan(dest...)
}

// storeRows is the whole result set, already read. The store answers one frame,
// so there is nothing to stream and nothing to hold open; Close exists to
// satisfy the interface and to keep callers written the usual way.
type storeRows struct {
	cells []cell
	width int
	at    int
	err   error
}

func (r *storeRows) Next() bool {
	if r.err != nil || r.width == 0 {
		return false
	}
	if (r.at+1)*r.width >= len(r.cells) {
		return false
	}
	r.at++
	return true
}

func (r *storeRows) Err() error { return r.err }
func (r *storeRows) Close()     {}

func (r *storeRows) Scan(dest ...any) error {
	if r.at < 0 {
		return errors.New("aimee: Scan before Next")
	}
	row := r.cells[r.at*r.width : (r.at+1)*r.width]
	if len(dest) != len(row) {
		return fmt.Errorf("aimee: scanning %d columns into %d destinations",
			len(row), len(dest))
	}
	for i, d := range dest {
		if err := assign(d, row[i]); err != nil {
			return fmt.Errorf("aimee: column %d: %w", i, err)
		}
	}
	return nil
}

// assign puts one typed cell into one destination.
//
// The store said what each value is, so nothing here parses or guesses. A
// mismatch is reported against the column it came from rather than silently
// coerced, because a coercion is how a BOOLEAN becomes a zero and a whole table
// reads false.
func assign(dest any, c cell) error {
	// NULL first: a destination that cannot hold it must say so rather than
	// take a zero, which is the flattening this wire exists to prevent. A
	// family that expects NULL scans into a pointer-to-pointer and gets nil.
	if c.kind == wireNull {
		switch d := dest.(type) {
		case **string:
			*d = nil
		case **int64:
			*d = nil
		case **float64:
			*d = nil
		case **bool:
			*d = nil
		case *[]byte:
			// The one destination that takes NULL directly. A nil []byte is not
			// a zero value standing in for absence the way "" or 0 would be --
			// it is distinct from an empty slice, so nothing is flattened and a
			// caller can still tell the two apart.
			*d = nil
		default:
			return fmt.Errorf("is NULL; scan into a **T to accept it, or COALESCE it in SQL")
		}
		return nil
	}

	switch d := dest.(type) {
	case *string:
		if c.kind != wireText {
			return typeErr("text", c)
		}
		*d = c.text
	case **string:
		if c.kind != wireText {
			return typeErr("text", c)
		}
		v := c.text
		*d = &v
	case *int64:
		if c.kind != wireInt {
			return typeErr("an integer", c)
		}
		*d = c.num
	case **int64:
		if c.kind != wireInt {
			return typeErr("an integer", c)
		}
		v := c.num
		*d = &v
	case *int:
		if c.kind != wireInt {
			return typeErr("an integer", c)
		}
		*d = int(c.num)
	case *float64:
		switch c.kind {
		case wireFloat:
			*d = c.real
		case wireInt:
			// A whole number in a numeric column arrives as an integer; taking
			// it is exact, not a coercion.
			*d = float64(c.num)
		default:
			return typeErr("a number", c)
		}
	case **float64:
		if c.kind != wireFloat {
			return typeErr("a number", c)
		}
		v := c.real
		*d = &v
	case *[]byte:
		if c.kind != wireBytes {
			return typeErr("bytes", c)
		}
		*d = c.blob
	case *bool:
		if c.kind != wireBool {
			return typeErr("a boolean", c)
		}
		*d = c.flag
	case **bool:
		if c.kind != wireBool {
			return typeErr("a boolean", c)
		}
		v := c.flag
		*d = &v
	default:
		return fmt.Errorf("unsupported destination %T", dest)
	}
	return nil
}

func typeErr(want string, c cell) error {
	return fmt.Errorf("the store sent %s where %s was expected", kindName(c.kind), want)
}

func kindName(kind uint8) string {
	switch kind {
	case wireNull:
		return "NULL"
	case wireText:
		return "text"
	case wireInt:
		return "an integer"
	case wireFloat:
		return "a number"
	case wireBool:
		return "a boolean"
	case wireBytes:
		return "bytes"
	case wireTextArray:
		return "an array"
	}
	return "an unknown type"
}
