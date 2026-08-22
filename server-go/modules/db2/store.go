package db2

import (
	"context"
	"errors"
	"os"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// defaultOperationTimeout bounds an operation that did not inherit a shorter
// deadline from its caller. It is not a target: an operation reaching it has
// already lost, and the value exists so a lost one lets go of its connection.
const defaultOperationTimeout = 5 * time.Second

// Store is what an operation is allowed to do to the database.
//
// An interface rather than *pgxpool.Pool so an operation can be tested without
// one, and so the port cannot grow a second way to reach Postgres: everything
// goes through Query or QueryRow, and neither hands out the pool.
type Store interface {
	// Query runs a statement expecting rows.
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	// QueryRow runs a statement expecting at most one row.
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
	// Exec runs a statement expecting none, and answers how many rows it
	// affected -- which is the difference between a write that changed
	// something and one that matched nothing.
	Exec(ctx context.Context, sql string, args ...any) (int64, error)
}

// PoolStore is the production Store: one pgx pool over AIMEE_DB2_URL.
type PoolStore struct {
	pool *pgxpool.Pool
}

func (s *PoolStore) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	return s.pool.Query(ctx, sql, args...)
}

func (s *PoolStore) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	return s.pool.QueryRow(ctx, sql, args...)
}

func (s *PoolStore) Exec(ctx context.Context, sql string, args ...any) (int64, error) {
	tag, err := s.pool.Exec(ctx, sql, args...)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

// Close releases the pool. Safe on a nil store so a caller need not branch.
func (s *PoolStore) Close() {
	if s != nil && s.pool != nil {
		s.pool.Close()
	}
}

type poolState struct {
	mu    sync.Mutex
	store *PoolStore
}

var production poolState

// ProductionStore opens the pool on first use and reuses it after.
//
// A failed open is deliberately not latched. A DSN corrected after start, or a
// database that was not up yet, recovers on the next call rather than requiring
// a restart -- the same posture the postgres health module takes, and for the
// same reason: refusing forever because of a transient is its own outage.
func ProductionStore() (*PoolStore, error) {
	production.mu.Lock()
	defer production.mu.Unlock()
	if production.store != nil {
		return production.store, nil
	}
	dsn := os.Getenv("AIMEE_DB2_URL")
	if dsn == "" {
		return nil, errors.New("db2: AIMEE_DB2_URL is unset")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		// The parse error is not returned: it renders the DSN, and this is
		// reached on a path that logs.
		return nil, errors.New("db2: invalid AIMEE_DB2_URL")
	}
	pool, err := pgxpool.NewWithConfig(context.Background(), config)
	if err != nil {
		return nil, errors.New("db2: connection pool initialization failed")
	}
	production.store = &PoolStore{pool: pool}
	return production.store, nil
}

// CloseProductionStore drops the pool, so the next call opens a new one.
func CloseProductionStore() {
	production.mu.Lock()
	defer production.mu.Unlock()
	production.store.Close()
	production.store = nil
}
