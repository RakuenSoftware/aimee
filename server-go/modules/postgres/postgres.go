// Package postgres implements bounded PostgreSQL operations for the KB process.
package postgres

import (
	"context"
	"encoding/binary"
	"errors"
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

	flagSchema = uint32(1 << 0)
	flagPGTrgm = uint32(1 << 1)

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
  )`

type defaultProbeState struct {
	once sync.Once
	pool *pgxpool.Pool
	err  error
}

var (
	productionProbe   defaultProbeState
	productionHandler = newHandler(defaultProbe)
)

func (state *defaultProbeState) open() {
	dsn := os.Getenv("AIMEE_DB2_URL")
	if dsn == "" {
		state.err = errors.New("postgres: AIMEE_DB2_URL is unset")
		return
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		state.err = errors.New("postgres: invalid AIMEE_DB2_URL")
		return
	}
	// The C substrate already owns the KB's main connection pool. This module's
	// first bounded slice needs only enough capacity for concurrent health calls.
	config.MaxConns = 2
	config.MinConns = 0
	pool, err := pgxpool.NewWithConfig(context.Background(), config)
	if err != nil {
		state.err = errors.New("postgres: connection pool initialization failed")
		return
	}
	state.pool = pool
}

func defaultProbe(ctx context.Context) (bool, bool, error) {
	productionProbe.once.Do(productionProbe.open)
	if productionProbe.err != nil {
		return false, false, productionProbe.err
	}
	var schemaOK, havePGTrgm bool
	if err := productionProbe.pool.QueryRow(ctx, healthQuery).Scan(&schemaOK, &havePGTrgm); err != nil {
		// Do not wrap the driver error: it may contain connection details. The
		// process boundary reports a typed failure and keeps the DSN private.
		return false, false, errors.New("postgres: health query failed")
	}
	return schemaOK, havePGTrgm, nil
}

func requestValid(request []byte) bool {
	return len(request) == requestLen &&
		binary.LittleEndian.Uint32(request[0:4]) == requestMagic &&
		binary.LittleEndian.Uint32(request[4:8]) == wireVersion
}

func healthResponse(schemaOK, havePGTrgm bool) []byte {
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	var flags uint32
	if schemaOK {
		flags |= flagSchema
	}
	if havePGTrgm {
		flags |= flagPGTrgm
	}
	binary.LittleEndian.PutUint32(response[8:12], flags)
	return response
}

// newHandler builds the health stage around a bounded probe. Production uses
// the package Handle entry point; tests inject the same evidence without a DB.
func newHandler(probe func(context.Context) (bool, bool, error)) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != StageHealth || !requestValid(request) || probe == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), probeTimeout)
		defer cancel()
		schemaOK, havePGTrgm, err := probe(ctx)
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return healthResponse(schemaOK, havePGTrgm), bus.ModuleStatusOK
	}
}

// Handle is the production module entry point. No SQL, DSN, or identity data
// crosses the bus; the KB-local process reads its existing environment secret.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	return productionHandler(invocation, request)
}
