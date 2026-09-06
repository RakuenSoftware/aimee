// Package postgres is the PostgreSQL store module.
//
// One provider, used by both daemons through the shared server-go/db contract.
// Pool, transaction and migration behavior does not depend on whether the
// consumer is KB or server. Deployment configuration selects the database;
// domain ownership is not a separate physical database tier.
//
// This file serves health; sql.go serves the database transport. Domain
// operations and their schemas remain with their owning modules.
//
// A different database is a different module. Swapping PostgreSQL out for
// another engine means installing that engine's module, not configuring this
// one -- which is what makes the store pluggable without this package having to
// know it might be replaced.
package postgres

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"os"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	EventHealth uint32 = 11265
	StageHealth uint32 = 1

	requestMagic  uint32 = 0x51484750 // "PGHQ"
	responseMagic uint32 = 0x52484750 // "PGHR"
	wireVersion   uint32 = 1

	requestLen  = 8
	responseLen = 16

	flagSchema   = uint32(1 << 0)
	flagPGTrgm   = uint32(1 << 1)
	flagKBTables = uint32(1 << 2)

	probeTimeout = 400 * time.Millisecond
)

const healthQuery = `SELECT
  EXISTS (
    SELECT 1
      FROM information_schema.tables
     WHERE table_schema = current_schema()
       AND table_name = 'memories'
  ),
  EXISTS (
    SELECT 1
      FROM pg_extension
     WHERE extname = 'pg_trgm'
  ),
  (
    SELECT COUNT(*) = 2
      FROM information_schema.tables
     WHERE table_schema = current_schema()
       AND table_name IN ('kb_documents', 'kb_async_jobs')
  )`

type healthEvidence struct {
	schemaOK   bool
	havePGTrgm bool
	kbTablesOK bool
}

type defaultProbeState struct {
	mu   sync.Mutex
	pool *pgxpool.Pool
}

var (
	productionProbe   defaultProbeState
	productionHandler = newHandler(defaultProbe)
)

func (state *defaultProbeState) getPool() (*pgxpool.Pool, error) {
	state.mu.Lock()
	defer state.mu.Unlock()
	if state.pool != nil {
		return state.pool, nil
	}
	dsn := os.Getenv("AIMEE_STORE_URL")
	if dsn == "" {
		return nil, errors.New("postgres: AIMEE_STORE_URL is unset")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, errors.New("postgres: invalid AIMEE_STORE_URL")
	}
	// The health probe keeps its own small pool rather than the module's shared
	// one: it must be able to answer "can this process reach its database" even
	// when the shared pool is what is broken.
	config.MaxConns = 2
	config.MinConns = 0
	pool, err := pgxpool.NewWithConfig(context.Background(), config)
	if err != nil {
		return nil, errors.New("postgres: connection pool initialization failed")
	}
	state.pool = pool
	return pool, nil
}

func (state *defaultProbeState) close() {
	state.mu.Lock()
	defer state.mu.Unlock()
	if state.pool != nil {
		state.pool.Close()
		state.pool = nil
	}
}

func defaultProbe(ctx context.Context) (healthEvidence, error) {
	pool, err := productionProbe.getPool()
	if err != nil {
		// Failed initialization is deliberately not latched: a corrected secret or
		// transient startup failure can recover on a later bounded health call.
		return healthEvidence{}, err
	}
	var evidence healthEvidence
	if err := pool.QueryRow(ctx, healthQuery).Scan(
		&evidence.schemaOK,
		&evidence.havePGTrgm,
		&evidence.kbTablesOK,
	); err != nil {
		// Do not wrap the driver error: it may contain connection details. The
		// process boundary reports a typed failure and keeps the DSN private.
		return healthEvidence{}, errors.New("postgres: health query failed")
	}
	return evidence, nil
}

func requestValid(request []byte) bool {
	return len(request) == requestLen &&
		binary.LittleEndian.Uint32(request[0:4]) == requestMagic &&
		binary.LittleEndian.Uint32(request[4:8]) == wireVersion
}

func healthResponse(evidence healthEvidence) []byte {
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	var flags uint32
	if evidence.schemaOK {
		flags |= flagSchema
	}
	if evidence.havePGTrgm {
		flags |= flagPGTrgm
	}
	if evidence.kbTablesOK {
		flags |= flagKBTables
	}
	binary.LittleEndian.PutUint32(response[8:12], flags)
	return response
}

// newHandler builds the health stage around a bounded probe. Production uses
// the package Handle entry point; tests inject the same evidence without a DB.
func newHandler(probe func(context.Context) (healthEvidence, error)) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != StageHealth || !requestValid(request) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if probe == nil {
			return nil, bus.ModuleStatusInternal
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		timeout := invocation.Remaining(probeTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		evidence, err := probe(ctx)
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if err != nil {
			// Why the health probe failed IS the health answer. Returning a bare
			// Internal told the caller only that the check did not complete, so
			// an unreachable database, a refused login and a probe timeout were
			// one indistinguishable status.
			log.Printf("postgres: health probe failed: %v", err)
			return nil, bus.ModuleStatusInternal
		}
		return healthResponse(evidence), bus.ModuleStatusOK
	}
}

// Handle is the health stage's entry point. No SQL, DSN, or identity data
// crosses the bus: the process reads its own environment secret.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	return productionHandler(invocation, request)
}

// Close releases the process-local connection pool during graceful module
// shutdown. Configuration rotation is applied by restarting this process.
func Close() {
	productionProbe.close()
}
