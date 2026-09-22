package postgres

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// EvaluationStore is a standalone evaluation capability, never a live daemon's
// store. The provider owns its pools, SQL wire and disposable database. Domain
// modules receive the same driver-neutral Store they use over the module bus.
// Evaluation SQL uses the designated database owner's permissions; production
// runtime-role authorization must be tested separately.
type EvaluationStore struct {
	db.Store
	mu      sync.Mutex
	ctx     context.Context
	admin   *pgxpool.Pool
	pool    *pgxpool.Pool
	handler *sqlHandler
	name    string
	closed  bool
}

// OpenEvaluationStore requires an explicit disposable PostgreSQL admin DSN in
// AIMEE_DB2_EVAL_URL. It never falls back to a live storage URL, changes the
// supplied database, or reuses public there. A separate database is necessary:
// knowledge schema functions deliberately qualify their objects with public.
// Call Close even after cancellation; its error reports failed cleanup.
func OpenEvaluationStore(ctx context.Context, schema string) (*EvaluationStore, error) {
	if ctx == nil {
		return nil, errors.New("postgres evaluation: context required")
	}
	lifetime := ctx
	ctx, cancel := context.WithTimeout(ctx, time.Minute)
	defer cancel()
	url := os.Getenv("AIMEE_DB2_EVAL_URL")
	if url == "" {
		return nil, errors.New("postgres evaluation requires AIMEE_DB2_EVAL_URL (disposable PostgreSQL admin DSN)")
	}
	config, err := parseStoreConfigFor("AIMEE_DB2_EVAL_URL", url)
	if err != nil {
		return nil, err
	}
	config.MaxConns = 2
	admin, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		return nil, errors.New("postgres evaluation: cannot initialize admin pool")
	}
	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		admin.Close()
		return nil, err
	}
	name := "aimee_memory_eval_" + hex.EncodeToString(nonce[:])
	quoted := pgx.Identifier{name}.Sanitize()
	s := &EvaluationStore{ctx: lifetime, admin: admin, name: name}
	fail := func(err error) (*EvaluationStore, error) {
		cleanupErr := s.Close()
		// No caller receives s after a failed open. Release the admin pool even
		// when database cleanup fails; the error names the database to retry.
		if s.admin != nil {
			s.admin.Close()
			s.admin = nil
		}
		return nil, errors.Join(err, cleanupErr)
	}
	if _, err := admin.Exec(ctx, "CREATE DATABASE "+quoted+" TEMPLATE template0"); err != nil {
		// Cancellation may arrive after PostgreSQL created the database but
		// before its receipt. Attempt cleanup even when CREATE reports failure.
		return fail(errors.New("postgres evaluation: cannot create isolated database"))
	}
	if _, err := admin.Exec(ctx, "REVOKE CONNECT ON DATABASE "+quoted+" FROM PUBLIC"); err != nil {
		return fail(errors.New("postgres evaluation: cannot restrict isolated database"))
	}
	config = config.Copy()
	config.ConnConfig.Database = name
	config.ConnConfig.RuntimeParams["search_path"] = "public"
	config.MaxConns = 8
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		return fail(errors.New("postgres evaluation: cannot initialize isolated pool"))
	}
	s.pool = pool
	if err := pool.Ping(ctx); err != nil {
		return fail(errors.New("postgres evaluation: cannot connect to isolated database"))
	}
	// The caller supplies its packaged bootstrap, which may contain functions
	// and transaction boundaries. Execute it intact, only in the new database.
	if _, err := pool.Exec(ctx, schema); err != nil {
		return fail(fmt.Errorf("postgres evaluation: schema bootstrap failed: %w", err))
	}
	s.handler = &sqlHandler{txs: newTxRegistry(),
		poolFn:          func(context.Context) (*pgxpool.Pool, error) { return pool, nil },
		migrationPoolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	s.handler.txs.maxOpen = int(config.MaxConns) - 1
	s.Store, err = db.NewStore(evaluationCaller{s})
	if err != nil {
		return fail(err)
	}
	return s, nil
}

type evaluationCaller struct{ store *EvaluationStore }

func (c evaluationCaller) Call(ctx context.Context, kind, stage uint32, trace uint64,
	deadline time.Duration, request []byte) ([]byte, error) {
	s := c.store
	// Serialize the local wire, including transaction registry access, against
	// cleanup. A forgotten transaction must not keep pool.Close waiting forever.
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, errors.New("postgres evaluation: store closed")
	}
	if kind != db.EventPostgresSQL || stage != db.StagePostgresSQL {
		return nil, errors.New("postgres evaluation: invalid SQL route")
	}
	if err := s.ctx.Err(); err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, deadline)
	defer cancel()
	stop := context.AfterFunc(s.ctx, cancel)
	defer stop()
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	body, status := s.handler.handleContext(ctx, bus.ModuleInvocation{StageID: stage, TraceID: trace}, request)
	if status != bus.ModuleStatusOK {
		return nil, fmt.Errorf("postgres evaluation: SQL transport status %d", status)
	}
	return body, nil
}

// Close rolls back abandoned transactions before closing the pool and dropping
// only this instance's randomly named database. Failed drops can be retried.
func (s *EvaluationStore) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	if s.admin == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if s.handler != nil {
		r := s.handler.txs
		r.mu.Lock()
		for id, tx := range r.open {
			_ = tx.tx.Rollback(ctx)
			tx.conn.Release()
			delete(r.open, id)
		}
		r.mu.Unlock()
	}
	if s.pool != nil {
		s.pool.Close()
		s.pool = nil
	}
	if _, err := s.admin.Exec(ctx, "DROP DATABASE IF EXISTS "+pgx.Identifier{s.name}.Sanitize()); err != nil {
		return fmt.Errorf("postgres evaluation: cleanup of %s failed", s.name)
	}
	s.admin.Close()
	s.admin = nil
	return nil
}
