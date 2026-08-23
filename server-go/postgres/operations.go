package postgres

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
)

// Row is one result row. Cells carry their type, so a caller reads what the
// column held rather than parsing a rendering of it -- a BOOLEAN read as text
// and parsed back is a guess, and the guess has been wrong here before.
type Row []Value

// Rows is a whole result set.
type Rows []Row

// Exec runs a statement and answers how many rows it changed.
//
// handle is zero outside a transaction. Inside one it is the handle Begin
// returned, and it rides on every statement because this holds no connection of
// its own -- the transaction lives in the module.
func (c *Client) Exec(ctx context.Context, statementID string, handle uint64,
	sql string, args ...Value) (uint64, error) {
	body, err := statement(opExec, statementID, handle, sql, args)
	if err != nil {
		return 0, err
	}
	reply, err := c.call(ctx, body)
	if err != nil {
		return 0, err
	}
	r, err := header(reply)
	if err != nil {
		return 0, err
	}
	return r.u64()
}

// Query runs a statement and returns every row it produced.
//
// A result set past the module's ceiling is REFUSED rather than truncated, and
// arrives here as an Error with SQLSTATE 54000. That is deliberate on both
// sides: a caller handed exactly the ceiling cannot tell a capped answer from a
// complete one, and would record the cap as fact. Page the query instead.
func (c *Client) Query(ctx context.Context, statementID string, handle uint64,
	sql string, args ...Value) (Rows, error) {
	body, err := statement(opQuery, statementID, handle, sql, args)
	if err != nil {
		return nil, err
	}
	reply, err := c.call(ctx, body)
	if err != nil {
		return nil, err
	}
	r, err := header(reply)
	if err != nil {
		return nil, err
	}
	width, err := r.u32()
	if err != nil {
		return nil, err
	}
	count, err := r.u32()
	if err != nil {
		return nil, err
	}
	rows := make(Rows, 0, count)
	for index := uint32(0); index < count; index++ {
		row := make(Row, 0, width)
		for cell := uint32(0); cell < width; cell++ {
			value, err := r.value()
			if err != nil {
				return nil, err
			}
			row = append(row, value)
		}
		rows = append(rows, row)
	}
	return rows, nil
}

// ErrNoRows is returned by QueryRow when the statement matched nothing.
var ErrNoRows = errors.New("postgres: no rows")

// QueryRow returns the first row, or ErrNoRows.
func (c *Client) QueryRow(ctx context.Context, statementID string, handle uint64,
	sql string, args ...Value) (Row, error) {
	rows, err := c.Query(ctx, statementID, handle, sql, args...)
	if err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		return nil, ErrNoRows
	}
	return rows[0], nil
}

// Begin opens a transaction and returns its handle.
//
// The handle is bound by the module to the principal AND the attachment that
// opened it, so it cannot be used by anyone else and does not survive a
// reconnect. Every statement in the transaction carries it.
func (c *Client) Begin(ctx context.Context, statementID string) (uint64, error) {
	body, err := statement(opBegin, statementID, 0, "", nil)
	if err != nil {
		return 0, err
	}
	reply, err := c.call(ctx, body)
	if err != nil {
		return 0, err
	}
	r, err := header(reply)
	if err != nil {
		return 0, err
	}
	return r.u64()
}

// Commit ends the transaction, keeping its work.
//
// A commit on a handle the module no longer holds answers SQLSTATE 25P01
// rather than success. Check it: the alternative is believing a transaction
// landed when every write in it is already gone.
func (c *Client) Commit(ctx context.Context, statementID string, handle uint64) error {
	return c.finish(ctx, opCommit, statementID, handle)
}

// Rollback ends the transaction, discarding its work.
func (c *Client) Rollback(ctx context.Context, statementID string, handle uint64) error {
	return c.finish(ctx, opRollback, statementID, handle)
}

func (c *Client) finish(ctx context.Context, op uint32, statementID string,
	handle uint64) error {
	body, err := statement(op, statementID, handle, "", nil)
	if err != nil {
		return err
	}
	reply, err := c.call(ctx, body)
	if err != nil {
		return err
	}
	_, err = header(reply)
	return err
}

// InTx runs fn inside a transaction, committing when it returns nil and rolling
// back when it does not.
//
// The rollback error is deliberately not returned over fn's: the caller needs
// to know why the work failed, and a rollback that also failed is a second fact
// about a transaction that is ending either way.
func (c *Client) InTx(ctx context.Context, statementID string,
	fn func(handle uint64) error) error {
	handle, err := c.Begin(ctx, statementID)
	if err != nil {
		return err
	}
	if err := fn(handle); err != nil {
		_ = c.Rollback(ctx, statementID, handle)
		return err
	}
	return c.Commit(ctx, statementID, handle)
}

// CurrentVersion answers an owner's highest applied schema version and the
// checksum it was recorded with, so a caller can verify its own file against
// the head before sending anything.
func (c *Client) CurrentVersion(ctx context.Context, owner string) (uint64, string, error) {
	w := &writer{}
	w.u32(opCurrentVersion)
	w.str(owner)
	reply, err := c.call(ctx, w.buf)
	if err != nil {
		return 0, "", err
	}
	r, err := header(reply)
	if err != nil {
		return 0, "", err
	}
	version, err := r.u64()
	if err != nil {
		return 0, "", err
	}
	checksum, err := r.str()
	return version, checksum, err
}

// Migrate applies one version of an owner's schema.
//
// The version is EXPLICIT, never derived from a filename or a sort order: a
// file whose name later sorts into the middle would renumber everything after
// it, every recorded checksum would stop matching, and the module would
// correctly read that as every migration having been edited after it ran.
//
// The checksum is recomputed by the module over the statements it actually
// received and refused if it disagrees, so it cannot record a hash of anything
// but what it applied.
func (c *Client) Migrate(ctx context.Context, owner string, version uint64,
	checksum string, statements []string) error {
	if owner == "" {
		return errors.New("postgres: migration owner is required")
	}
	if version == 0 {
		return errors.New("postgres: migration version must be positive")
	}
	if len(statements) == 0 {
		return fmt.Errorf("postgres: %s version %d carries no statements", owner, version)
	}
	w := &writer{}
	w.u32(opMigrate)
	w.str(owner)
	w.u64(version)
	w.str(checksum)
	w.u32(uint32(len(statements)))
	for _, s := range statements {
		if len(s) > MaxStatementBytes {
			return fmt.Errorf("postgres: %s version %d has a statement of %d bytes, over %d",
				owner, version, len(s), MaxStatementBytes)
		}
		w.str(s)
	}
	reply, err := c.call(ctx, w.buf)
	if err != nil {
		return err
	}
	_, err = header(reply)
	return err
}

// Checksum computes a migration's checksum, matching the module's construction
// exactly.
//
// Exported so a caller does not write its own. Per statement: decimal length,
// NUL, the statement, NUL. Length-prefixed so statement boundaries are part of
// the digest -- without that ["a;b"] and ["a","b"] collide, and an edit that
// merged two statements into one would read as no change at all, which is
// exactly the edit this exists to catch.
func Checksum(statements []string) string {
	sum := sha256.New()
	for _, statement := range statements {
		fmt.Fprintf(sum, "%d\x00%s\x00", len(statement), statement)
	}
	return hex.EncodeToString(sum.Sum(nil))
}
