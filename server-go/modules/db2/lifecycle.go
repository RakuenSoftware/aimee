package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// LifecycleBackend is the lifecycle family's seam.
//
// It covers the three operations whose answer the Go module can already
// produce. The family's other seven — the embedding dimension, the refusal
// counters, the re-embed progress and its two clears, the embedder serving id,
// and the dimension reset — read or mutate process-local state that still lives
// in the C module. They are deliberately absent rather than stubbed: a stub
// would answer confidently with a default and make an unmigrated operation look
// migrated.
type LifecycleBackend interface {
	// HealthProbe answers the schema and extension evidence.
	HealthProbe(ctx context.Context) (db2contract.HealthEvidence, error)
	// PostgresStatus reports what the server says about itself.
	PostgresStatus(ctx context.Context) (db2contract.PostgresStatus, error)
	// PoolStatus reports the connection pool's own accounting.
	PoolStatus(ctx context.Context) (db2contract.PoolStatus, error)
}

// NewLifecycleHandler builds the Go provider for the lifecycle family
// (stage 1).
//
// Like the memory provider, this stays out of the module process registry
// until the atomic DB2 ownership cutover.
func NewLifecycleHandler(backend LifecycleBackend) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != db2contract.StageHealth {
			return nil, bus.ModuleStatusInvalidRequest
		}

		// Health predates the envelope and carries its own magic frame, so it
		// is recognised before any header decode is attempted. Reading it as an
		// envelope would reject the family's oldest operation as malformed.
		operation := db2contract.OperationHealth
		if db2contract.DecodeHealthRequest(request) != nil {
			header, err := db2contract.DecodeRequestHeader(request)
			if err != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			operation = header.Operation
		}

		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if backend == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}

		timeout := invocation.Remaining(healthProbeTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		finish := func(encode func() ([]byte, error), err error) ([]byte, bus.ModuleStatus) {
			if err != nil {
				if invocation.Cancelled() || ctx.Err() != nil {
					return nil, bus.ModuleStatusCancelled
				}
				return nil, bus.ModuleStatusInternal
			}
			if invocation.Cancelled() {
				return nil, bus.ModuleStatusCancelled
			}
			reply, encodeErr := encode()
			if encodeErr != nil {
				return nil, bus.ModuleStatusInternal
			}
			return reply, bus.ModuleStatusOK
		}

		switch operation {
		case db2contract.OperationHealth:
			evidence, err := backend.HealthProbe(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeHealthResponse(evidence), nil
			}, err)

		case db2contract.OperationPostgresStatus:
			if db2contract.DecodePostgresStatusRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			status, err := backend.PostgresStatus(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodePostgresStatusReply(db2contract.ResultOK, status)
			}, err)

		case db2contract.OperationPoolStatus:
			if db2contract.DecodePoolStatusRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			status, err := backend.PoolStatus(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodePoolStatusReply(db2contract.ResultOK, status)
			}, err)
		}

		return nil, bus.ModuleStatusInvalidRequest
	}
}
