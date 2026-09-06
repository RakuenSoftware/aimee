// Package db is the shared, driver-neutral database contract for KB and server.
// It opens no database: the postgres module owns connections and credentials.
package db

import (
	"context"
	"errors"
	"time"
)

const DefaultTimeout = 2 * time.Second

// ErrNoRows reports a read that matched nothing. Domain callers share this
// sentinel rather than depending on a driver's errors. Its text is preserved
// from the original client; its identity is shared by both placements.
var ErrNoRows = errors.New("aimee: no rows in result set")

// ErrTxClosed is what committing or rolling back an already-finished
// transaction reports.
var ErrTxClosed = errors.New("aimee: transaction is closed")

// Row is one result row, if there was one. Scan reports ErrNoRows when there
// was not.
type Row interface {
	Scan(dest ...any) error
}

// Rows is a result set. The contract is the usual one -- Next until it returns
// false, then check Err -- and Close must run whatever the outcome.
type Rows interface {
	Next() bool
	Scan(dest ...any) error
	Err() error
	Close()
}

// Tag is what a write reports about itself. Only the row count is used, and
// only ever to tell "changed something" from "matched nothing".
type Tag interface {
	RowsAffected() int64
}

// RowsAffected is a Tag carrying just the count, which is all a Tag is for.
// Exported because a fake store has to answer "this write changed n rows"
// without reaching for a driver's tag type to say it.
type RowsAffected int64

func (n RowsAffected) RowsAffected() int64 { return int64(n) }

// Queryer is the storage capability a family's SQL needs, in driver-neutral types.
//
// Deliberately not written in a driver's vocabulary. aimee stores its data by
// calling the postgres module over the bus, so the thing satisfying this is a
// bus client, not a library in this process -- and an interface returning
// pgx.Rows could never be satisfied by one. Both the store and a transaction
// satisfy it, which is what lets an operation be written once and run either
// way depending on whether it declares a transaction.
type Queryer interface {
	Exec(ctx context.Context, sql string, args ...any) (Tag, error)
	Query(ctx context.Context, sql string, args ...any) (Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) Row
}

// IsNoRows reports whether a scan found nothing.
//
// Every family asks this, and asking it through the driver's own error meant
// each one imported pgx to compare against one sentinel. Naming it here keeps
// "there was no row" a question about the store rather than about the driver.
func IsNoRows(err error) bool { return errors.Is(err, ErrNoRows) }

// Tx is a Queryer that can be committed or rolled back.
type Tx interface {
	Queryer
	Commit(ctx context.Context) error
	Rollback(ctx context.Context) error
}

// DB is the database a family runs against, as an interface so a family's
// dispatch and arity behaviour is testable without a server.
type DB interface {
	Queryer
	Begin(ctx context.Context) (Tx, error)
}
