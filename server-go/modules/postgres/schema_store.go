package postgres

import (
	"context"
	"errors"

	"github.com/jackc/pgx/v5"
)

// poolSchemaStore satisfies SchemaStore over this module's pool.
//
// The migration engine is written against an interface so its rules -- ordering,
// checksums, one transaction, the advisory lock -- can be driven without a
// database. This is the half that needs one.
type poolSchemaStore struct{}

// translate maps the driver's no-rows sentinel onto the engine's, so the engine
// can distinguish "nothing recorded" from a failure without importing pgx.
func translate(err error) error {
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNoSchemaRows
	}
	return err
}

func (s *poolSchemaStore) Exec(ctx context.Context, sql string, args ...any) error {
	pool, err := productionProbe.getPool()
	if err != nil {
		return err
	}
	_, execErr := pool.Exec(ctx, sql, args...)
	return execErr
}

type poolSchemaRow struct {
	row pgx.Row
}

func (r poolSchemaRow) Scan(dest ...any) error {
	return translate(r.row.Scan(dest...))
}

func (s *poolSchemaStore) QueryRow(ctx context.Context, sql string, args ...any) SchemaRow {
	pool, err := productionProbe.getPool()
	if err != nil {
		return failedRow{err: err}
	}
	return poolSchemaRow{row: pool.QueryRow(ctx, sql, args...)}
}

// failedRow carries an acquisition failure to the Scan, so a caller sees the
// real reason rather than a nil dereference.
type failedRow struct{ err error }

func (r failedRow) Scan(dest ...any) error { return r.err }

// InTx runs the migration inside one transaction.
//
// The statements and the row recording them land together or not at all. A
// migration that half-applied and was recorded as complete can never be re-run
// and can never be trusted, which is the state this exists to prevent.
func (s *poolSchemaStore) InTx(ctx context.Context, fn func(SchemaStore) error) error {
	pool, err := productionProbe.getPool()
	if err != nil {
		return err
	}
	tx, beginErr := pool.Begin(ctx)
	if beginErr != nil {
		return beginErr
	}
	if err := fn(&txSchemaStore{tx: tx}); err != nil {
		_ = tx.Rollback(ctx)
		return err
	}
	return tx.Commit(ctx)
}

type txSchemaStore struct{ tx pgx.Tx }

func (s *txSchemaStore) Exec(ctx context.Context, sql string, args ...any) error {
	_, err := s.tx.Exec(ctx, sql, args...)
	return err
}

func (s *txSchemaStore) QueryRow(ctx context.Context, sql string, args ...any) SchemaRow {
	return poolSchemaRow{row: s.tx.QueryRow(ctx, sql, args...)}
}

// InTx inside a transaction runs directly: the engine asks for one transaction
// and nesting a second would either be a no-op or a savepoint nobody asked for.
func (s *txSchemaStore) InTx(ctx context.Context, fn func(SchemaStore) error) error {
	return fn(s)
}

// CurrentVersionAndChecksum answers an owner's recorded head and the checksum it
// was recorded with.
//
// The checksum comes back so a caller can verify its own file against what
// actually ran before sending anything -- a mismatch at the head is the same
// "the source and the database disagree" failure the migration path refuses,
// caught one step earlier and without a write.
func CurrentVersionAndChecksum(ctx context.Context, store SchemaStore, owner string) (
	int64, string, error,
) {
	version, err := CurrentVersion(ctx, store, owner)
	if err != nil || version == 0 {
		return version, "", err
	}
	var checksum string
	scanErr := store.QueryRow(ctx,
		`SELECT checksum FROM aimee_schema_version WHERE owner = $1 AND version = $2`,
		owner, version).Scan(&checksum)
	if scanErr != nil && !errors.Is(scanErr, ErrNoSchemaRows) {
		return 0, "", scanErr
	}
	return version, checksum, nil
}
