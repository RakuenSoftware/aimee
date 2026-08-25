package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// LifecycleBackend is the lifecycle family's seam.
//
// It covers all ten of the family's operations, but they do not all come from
// the same place. Three are SQL, one is the pool's own accounting, three
// describe the running embedder process, and one performs destructive DDL. The
// seam keeps them in one list because the contract does, while the fields of
// LifecycleSeams say which host supplies what.
type LifecycleBackend interface {
	// HealthProbe answers the schema and extension evidence.
	HealthProbe(ctx context.Context) (db2contract.HealthEvidence, error)
	// PostgresStatus reports what the server says about itself.
	PostgresStatus(ctx context.Context) (db2contract.PostgresStatus, error)
	// PoolStatus reports the connection pool's own accounting.
	PoolStatus(ctx context.Context) (db2contract.PoolStatus, error)

	// --- embedder runtime state ---
	//
	// These three describe the running process rather than the database: the
	// dimension it is serving, the widths it has refused, and which embedder
	// build produced them. They are supplied by whoever owns that state, which
	// is the C module until the cutover and this module afterwards.

	// EmbeddingDimension is the width this process is serving.
	EmbeddingDimension(ctx context.Context) (uint32, error)
	// EmbeddingRefusals counts vector upserts refused for width disagreement.
	EmbeddingRefusals(ctx context.Context) (db2contract.EmbeddingRefusals, error)
	// EmbedderServingID names the embedder build behind those vectors.
	EmbedderServingID(ctx context.Context) (string, error)

	// --- re-embed maintenance ---

	// ReembedStatus reports an in-flight dimension change, if there is one.
	// The bool distinguishes "no re-embed running" from a zero-valued status.
	ReembedStatus(ctx context.Context) (bool, db2contract.ReembedStatus, error)
	// ReembedClear removes the maintenance marker.
	ReembedClear(ctx context.Context) error
	// ReembedClearMaintenance is the operator escape hatch: it clears a stuck
	// marker, refusing unless forced when the recorded and running dimensions
	// disagree. The bool reports whether the marker was actually cleared.
	ReembedClearMaintenance(ctx context.Context, force bool) (bool, db2contract.ReembedClearMaintenance, error)

	// DimensionReset re-shapes every derived vector table to a new width.
	DimensionReset(ctx context.Context, target uint32, force, dryRun bool) (DimensionResetOutcome, db2contract.DimensionReset, error)
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
				return nil, statusForError(err)
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

		case db2contract.OperationEmbeddingDimension:
			if db2contract.DecodeEmbeddingDimensionRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			dimension, err := backend.EmbeddingDimension(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEmbeddingDimensionReply(db2contract.ResultOK, dimension)
			}, err)

		case db2contract.OperationEmbeddingRefusals:
			if db2contract.DecodeEmbeddingRefusalsRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			refusals, err := backend.EmbeddingRefusals(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEmbeddingRefusalsReply(db2contract.ResultOK, refusals)
			}, err)

		case db2contract.OperationEmbedderServingID:
			if db2contract.DecodeEmbedderServingIDRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			servingID, err := backend.EmbedderServingID(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEmbedderServingIDReply(db2contract.ResultOK, servingID)
			}, err)

		case db2contract.OperationReembedStatus:
			if db2contract.DecodeReembedStatusRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			running, reembed, err := backend.ReembedStatus(ctx)
			return finish(func() ([]byte, error) {
				// No re-embed running is NotFound carrying no payload, which
				// is a different answer from a running one whose fields happen
				// to be zero — and the contract refuses the latter anyway.
				if !running {
					return db2contract.EncodeReembedStatusReply(db2contract.ResultNotFound,
						db2contract.ReembedStatus{})
				}
				return db2contract.EncodeReembedStatusReply(db2contract.ResultOK, reembed)
			}, err)

		case db2contract.OperationReembedClear:
			if db2contract.DecodeReembedClearRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			clearErr := backend.ReembedClear(ctx)
			if clearErr != nil {
				if invocation.Cancelled() || ctx.Err() != nil {
					return nil, bus.ModuleStatusCancelled
				}
				// A seam this module does not have is a capability answer, not
				// a claim about the marker's state.
				if errors.Is(clearErr, ErrNoQuerier) {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
			}
			result := uint32(db2contract.ResultOK)
			if clearErr != nil {
				result = db2contract.ResultInvalidState
			}
			return finish(func() ([]byte, error) {
				return db2contract.EncodeReembedClearReply(result)
			}, nil)

		case db2contract.OperationReembedClearMaintenance:
			force, decodeErr := db2contract.DecodeReembedClearMaintenanceRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			cleared, status, err := backend.ReembedClearMaintenance(ctx, force != 0)
			return finish(func() ([]byte, error) {
				// A refusal is Conflict carrying the two disagreeing
				// dimensions, because the operator's next decision depends on
				// seeing them — telling them only that it failed would leave
				// them with force as the sole remaining move.
				if !cleared {
					return db2contract.EncodeReembedClearMaintenanceReply(
						db2contract.ResultConflict, status)
				}
				return db2contract.EncodeReembedClearMaintenanceReply(db2contract.ResultOK, status)
			}, err)

		case db2contract.OperationDimensionReset:
			target, force, dryRun, decodeErr := db2contract.DecodeDimensionResetRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			outcome, status, err := backend.DimensionReset(ctx, target, force != 0, dryRun != 0)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeDimensionResetReply(outcome.result(), status)
			}, err)
		}

		return nil, bus.ModuleStatusInvalidRequest
	}
}
