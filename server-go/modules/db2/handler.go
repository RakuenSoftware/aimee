// Package db2 implements the nonselected Go provider for the DB2 module.
package db2

import (
	"context"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

const healthProbeTimeout = 400 * time.Millisecond

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

// HealthRow is the result capability needed by the lifecycle probe. A later
// pgxpool owner supplies its QueryRow result without exposing the pool or DSN
// to the bus handler.
type HealthRow interface {
	Scan(dest ...any) error
}

// QueryRowFunc is the lifecycle provider's current database seam. Keeping it
// explicit makes the provider replayable before pool and schema ownership move
// from C in a later slice.
type QueryRowFunc func(context.Context, string, ...any) HealthRow

// NewHandler builds the nonselected Go DB2 lifecycle provider. This package is
// deliberately absent from the module process registry until the atomic DB2
// ownership cutover.
func NewHandler(queryRow QueryRowFunc) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != db2contract.StageHealth ||
			db2contract.DecodeHealthRequest(request) != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if queryRow == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		timeout := invocation.Remaining(healthProbeTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		row := queryRow(ctx, healthQuery)
		if row == nil {
			return nil, bus.ModuleStatusInternal
		}
		evidence := db2contract.HealthEvidence{}
		if err := row.Scan(&evidence.SchemaOK, &evidence.HavePGTrgm, &evidence.KBTablesOK); err != nil {
			if invocation.Cancelled() {
				return nil, bus.ModuleStatusCancelled
			}
			return nil, bus.ModuleStatusInternal
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		return db2contract.EncodeHealthResponse(evidence), bus.ModuleStatusOK
	}
}
