package db2

import (
	"context"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// memoryOpTimeout is the default budget for one memory-family operation when
// the caller supplied no deadline of its own. These are single indexed reads
// against `memories`; a slower answer than this is a sick database, not a busy
// one, and the caller is better served by a prompt failure.
const memoryOpTimeout = 400 * time.Millisecond

// MemoryBackend is the memory family's database seam.
//
// It mirrors the C module's backend struct of function pointers
// (aimee_db2_module_backend_t) rather than inventing a new shape, because the
// two implementations must stay answerable to the same contract while the
// migration is in flight — and a reviewer comparing them should be reading one
// list, not two.
//
// Every method takes a context: the C side bounds these calls with a pooled
// statement timeout, and the Go side must not be the implementation that
// reintroduces an unbounded query.
type MemoryBackend interface {
	// Level3Count counts memories in tier L3.
	Level3Count(ctx context.Context) (uint32, error)
	// Level2Count counts memories in tier L2.
	Level2Count(ctx context.Context) (uint32, error)
	// OrphanedL0Count counts L0 memories older than the orphan horizon.
	OrphanedL0Count(ctx context.Context) (uint32, error)
	// TotalCount counts every memory, at u64 because this one is not bounded
	// by a tier.
	TotalCount(ctx context.Context) (uint64, error)
	// SessionL2Count counts L2 memories originating in one session.
	SessionL2Count(ctx context.Context, sourceSession string) (uint32, error)
	// KeyExists reports whether any memory carries the key.
	KeyExists(ctx context.Context, key string) (bool, error)
	// FindIDByKeyKind resolves one memory id from its key and kind.
	FindIDByKeyKind(ctx context.Context, key, kind string) (bool, uint64, error)
	// KeyExistsInTierPair reports whether the key exists in either tier.
	KeyExistsInTierPair(ctx context.Context, key, tierA, tierB string) (bool, error)
}

// boolReply is the wire's spelling of a boolean: the contract carries these as
// u32 rather than a byte, and one grammar for it here keeps every operation
// answering the same way. A previous slice of this migration shipped a boolean
// encoded two different ways on one wire, which is the defect this avoids.
func boolReply(v bool) uint32 {
	if v {
		return 1
	}
	return 0
}

// NewMemoryHandler builds the Go provider for the memory family (stage 3).
//
// Like the lifecycle provider, this is not yet registered in the module process
// registry: it exists so the memory operations are implemented, tested and
// reviewable ahead of the atomic DB2 ownership cutover, not so that half the
// family can be served from Go while the other half is served from C.
func NewMemoryHandler(backend MemoryBackend) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != db2contract.StageLevel3Count {
			return nil, bus.ModuleStatusInvalidRequest
		}
		header, err := db2contract.DecodeRequestHeader(request)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if backend == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}

		timeout := invocation.Remaining(memoryOpTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		// finish converts a backend answer into a reply, collapsing the three
		// checks every operation owes: the backend's error, a cancellation that
		// landed while it ran, and the encoder's own bound on the value.
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
				// The value cleared the database but not the contract's bound.
				// Reporting it as internal is deliberate: answering with a
				// truncated count would be a wrong answer presented as a right
				// one.
				return nil, bus.ModuleStatusInternal
			}
			return reply, bus.ModuleStatusOK
		}

		switch header.Operation {
		case db2contract.OperationLevel3Count:
			if db2contract.DecodeLevel3CountRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			count, err := backend.Level3Count(ctx)
			return finish(func() ([]byte, error) { return db2contract.EncodeLevel3CountReply(count) }, err)

		case db2contract.OperationLevel2Count:
			if db2contract.DecodeLevel2CountRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			count, err := backend.Level2Count(ctx)
			return finish(func() ([]byte, error) { return db2contract.EncodeLevel2CountReply(count) }, err)

		case db2contract.OperationOrphanedL0Count:
			if db2contract.DecodeOrphanedL0CountRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			count, err := backend.OrphanedL0Count(ctx)
			return finish(func() ([]byte, error) { return db2contract.EncodeOrphanedL0CountReply(count) }, err)

		case db2contract.OperationTotalCount:
			if db2contract.DecodeTotalCountRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			count, err := backend.TotalCount(ctx)
			return finish(func() ([]byte, error) { return db2contract.EncodeTotalCountReply(count) }, err)

		case db2contract.OperationSessionL2Count:
			session, decodeErr := db2contract.DecodeSessionL2CountRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			count, err := backend.SessionL2Count(ctx, session)
			return finish(func() ([]byte, error) { return db2contract.EncodeSessionL2CountReply(count) }, err)

		case db2contract.OperationKeyExists:
			key, decodeErr := db2contract.DecodeKeyExistsRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			exists, err := backend.KeyExists(ctx, key)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeKeyExistsReply(boolReply(exists))
			}, err)

		case db2contract.OperationFindIDByKeyKind:
			key, kind, decodeErr := db2contract.DecodeFindIDByKeyKindRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			found, id, err := backend.FindIDByKeyKind(ctx, key, kind)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeFindIDByKeyKindReply(boolReply(found), id)
			}, err)

		case db2contract.OperationKeyExistsInTierPair:
			key, tierA, tierB, decodeErr := db2contract.DecodeKeyExistsInTierPairRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			exists, err := backend.KeyExistsInTierPair(ctx, key, tierA, tierB)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeKeyExistsInTierPairReply(boolReply(exists))
			}, err)
		}

		// An operation this stage does not serve. Reported as invalid rather
		// than absent: the family is present, and claiming otherwise would tell
		// a caller to stop trying the whole stage.
		return nil, bus.ModuleStatusInvalidRequest
	}
}
