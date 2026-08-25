package db2

import (
	"context"
	"math"
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

	// SetEffectiveness records an effectiveness score for one memory.
	SetEffectiveness(ctx context.Context, memoryID uint64, value float64) error
	// ClearEffectiveness returns one memory's effectiveness to unmeasured.
	// This is a distinct operation from setting zero: zero is a score, and
	// NULL is the absence of one, and the demotion sweep treats them
	// differently.
	ClearEffectiveness(ctx context.Context, memoryID uint64) error
	// RetentionDelete removes memories of one sensitivity older than days,
	// answering how many rows went.
	RetentionDelete(ctx context.Context, sensitivity string, days uint32) (uint32, error)
	// DemoteLowEffectiveness moves scored L2 memories below the threshold down
	// to L1, answering how many moved.
	DemoteLowEffectiveness(ctx context.Context, threshold float64) (uint32, error)
	// EffectivenessStats summarises the effectiveness column.
	EffectivenessStats(ctx context.Context, lowThreshold float64) (db2contract.EffectivenessStats, error)
	// ListL2MemoryIDs lists L2 memory ids, bounded by max.
	ListL2MemoryIDs(ctx context.Context, max uint32) ([]uint64, error)

	// HealthCounters aggregates the rolling health window the host derives its
	// rates from.
	HealthCounters(ctx context.Context, promoteUseCount uint32, promoteConfidence float64) (db2contract.HealthCounters, error)
	// StatsCounts breaks the corpus down by tier and kind.
	StatsCounts(ctx context.Context) (db2contract.MemoryStats, error)

	// --- expiry ---

	// DeleteL0Provenance removes provenance rows for L0 memories. It runs
	// before DeleteL0 so no memory row outlives the record of where it came
	// from.
	DeleteL0Provenance(ctx context.Context) (uint32, error)
	// DeleteL0 removes the scratch tier outright.
	DeleteL0(ctx context.Context) (uint32, error)
	// ListKindsInTier lists the distinct kinds present in a tier, bounded by
	// max, so the sweeps iterate only over kinds that actually exist.
	ListKindsInTier(ctx context.Context, tier string, max uint32) ([]string, error)
	// KindExpireDays is the idle window after which a kind's L1 rows expire.
	KindExpireDays(ctx context.Context, kind string) (uint32, error)
	// DeleteStaleL1Provenance mirrors DeleteL0Provenance for the stale sweep.
	DeleteStaleL1Provenance(ctx context.Context, kind, window string) (uint32, error)
	// DeleteStaleL1 removes a kind's stale L1 rows.
	DeleteStaleL1(ctx context.Context, kind, window string) (uint32, error)

	// --- demotion and promotion ---

	// KindDemotePolicy is the confidence floor and idle window a kind must
	// fall through before its L2 rows demote.
	KindDemotePolicy(ctx context.Context, kind string) (confidence float64, days uint32, err error)
	// DemoteKind demotes one kind's qualifying L2 rows, stamping them.
	DemoteKind(ctx context.Context, stamp, kind string, confidence float64, window string) (uint32, error)
	// DemoteCascade weakens the confidence of rows depending on what this
	// call's stamp just demoted.
	DemoteCascade(ctx context.Context, stamp string) (uint32, error)
	// PromoteStable moves long-settled L2 rows to L3.
	PromoteStable(ctx context.Context, stamp string) (uint32, error)
	// ReclassifyDirectives moves directive-shaped L3 rows to L4.
	ReclassifyDirectives(ctx context.Context, requireApproval bool) (uint32, error)
	// RecordL4Approval records an operator's approval for an L4 promotion.
	RecordL4Approval(ctx context.Context, memoryID uint64, approver, note string) error

	// --- health snapshots ---

	// CountMemories counts the whole corpus for a health snapshot.
	CountMemories(ctx context.Context) (uint32, error)
	// CountRecentConflicts counts conflicts detected inside the window.
	CountRecentConflicts(ctx context.Context, days uint32) (uint32, error)
	// HealthRecord writes one health-cycle snapshot.
	HealthRecord(ctx context.Context, snapshot HealthSnapshot) error
	// PruneHealth drops health snapshots past the retention window.
	PruneHealth(ctx context.Context, days uint32) (uint32, error)
	// PruneContradictions drops contradiction-log rows past the window.
	PruneContradictions(ctx context.Context, days uint32) (uint32, error)
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

		case db2contract.OperationEffectivenessUpdate:
			memoryID, hasValue, value, decodeErr := db2contract.DecodeEffectivenessUpdateRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			// hasValue == 0 clears the score rather than storing zero. The
			// contract already refuses a nonzero value with hasValue == 0, so
			// the two cases cannot be confused on the wire.
			var updateErr error
			if hasValue == 0 {
				updateErr = backend.ClearEffectiveness(ctx, memoryID)
			} else {
				updateErr = backend.SetEffectiveness(ctx, memoryID, value)
			}
			// This operation's reply is a closed result rather than a status:
			// a memory that is not there is a state the caller must be told
			// about, not a fault of the module.
			result := uint32(db2contract.ResultOK)
			if updateErr != nil {
				if invocation.Cancelled() || ctx.Err() != nil {
					return nil, bus.ModuleStatusCancelled
				}
				result = db2contract.ResultInvalidState
			}
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEffectivenessUpdateReply(result)
			}, nil)

		case db2contract.OperationRetentionEnforce:
			if db2contract.DecodeRetentionEnforceRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			// Both sensitivities are swept in one operation, and the reply is
			// their sum, because the caller's question is "how much did
			// retention remove", not "how much of each".
			restricted, err := backend.RetentionDelete(ctx,
				db2contract.RetentionRestricted, db2contract.RetentionRestrictedDays)
			if err == nil {
				var sensitive uint32
				sensitive, err = backend.RetentionDelete(ctx,
					db2contract.RetentionSensitive, db2contract.RetentionSensitiveDays)
				restricted += sensitive
			}
			return finish(func() ([]byte, error) {
				return db2contract.EncodeRetentionEnforceReply(restricted)
			}, err)

		case db2contract.OperationEffectivenessDemote:
			if db2contract.DecodeEffectivenessDemoteRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			demoted, err := backend.DemoteLowEffectiveness(ctx, effectivenessDemoteThreshold())
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEffectivenessDemoteReply(demoted)
			}, err)

		case db2contract.OperationEffectivenessStats:
			if db2contract.DecodeEffectivenessStatsRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			stats, err := backend.EffectivenessStats(ctx, effectivenessStatsLowThreshold())
			return finish(func() ([]byte, error) {
				return db2contract.EncodeEffectivenessStatsReply(stats)
			}, err)

		case db2contract.OperationL2MemoryIDs:
			if db2contract.DecodeL2MemoryIDsRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			ids, err := backend.ListL2MemoryIDs(ctx, db2contract.L2MemoryIDsMax)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeL2MemoryIDsReply(ids)
			}, err)

		case db2contract.OperationHealthCounters:
			if db2contract.DecodeHealthCountersRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			counters, err := backend.HealthCounters(ctx,
				db2contract.HealthCountersPromoteUseCount,
				math.Float64frombits(db2contract.HealthCountersPromoteConfidenceBits))
			return finish(func() ([]byte, error) {
				return db2contract.EncodeHealthCountersReply(counters)
			}, err)

		case db2contract.OperationStatsCounts:
			if db2contract.DecodeStatsCountsRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			stats, err := backend.StatsCounts(ctx)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeStatsCountsReply(stats)
			}, err)

		case db2contract.OperationExpire:
			if db2contract.DecodeExpireRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			level0, stale, err := expireSweep(ctx, invocation, backend)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeExpireReply(level0, stale)
			}, err)

		case db2contract.OperationDemote:
			if db2contract.DecodeDemoteRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			demoted, cascaded, err := demoteSweep(ctx, invocation, backend)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeDemoteReply(demoted, cascaded)
			}, err)

		case db2contract.OperationPromoteStable:
			if db2contract.DecodePromoteStableRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			promoted, err := backend.PromoteStable(ctx, nowStamp(backend))
			return finish(func() ([]byte, error) {
				return db2contract.EncodePromoteStableReply(promoted)
			}, err)

		case db2contract.OperationReclassifyDirectives:
			requireApproval, decodeErr := db2contract.DecodeReclassifyDirectivesRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			reclassified, err := backend.ReclassifyDirectives(ctx, requireApproval != 0)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeReclassifyDirectivesReply(reclassified)
			}, err)

		case db2contract.OperationRecordL4Approval:
			memoryID, approver, note, decodeErr := db2contract.DecodeRecordL4ApprovalRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			err := backend.RecordL4Approval(ctx, memoryID, approver, note)
			return finish(func() ([]byte, error) {
				return db2contract.EncodeRecordL4ApprovalReply()
			}, err)

		case db2contract.OperationHealthRecord:
			promotions, demotions, expirations, decodeErr := db2contract.DecodeHealthRecordRequest(request)
			if decodeErr != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			// The caller reports what its cycle did; the module measures the
			// corpus itself. Trusting a caller's totals would let one cycle's
			// bad arithmetic become the recorded history.
			total, err := backend.CountMemories(ctx)
			var contradictions uint32
			if err == nil {
				contradictions, err = backend.CountRecentConflicts(ctx,
					db2contract.HealthRecordConflictWindowDays)
			}
			if err == nil {
				err = backend.HealthRecord(ctx, HealthSnapshot{
					TotalMemories:          total,
					ContradictionsDetected: contradictions,
					Promotions:             promotions,
					Demotions:              demotions,
					Expirations:            expirations,
				})
			}
			return finish(func() ([]byte, error) {
				return db2contract.EncodeHealthRecordReply()
			}, err)

		case db2contract.OperationHealthRetention:
			if db2contract.DecodeHealthRetentionRequest(request) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			snapshots, err := backend.PruneHealth(ctx, db2contract.HealthRetentionSnapshotDays)
			var contradictions uint32
			if err == nil {
				contradictions, err = backend.PruneContradictions(ctx,
					db2contract.HealthRetentionContradictionDays)
			}
			return finish(func() ([]byte, error) {
				return db2contract.EncodeHealthRetentionReply(snapshots, contradictions)
			}, err)
		}

		// An operation this stage does not serve. Reported as invalid rather
		// than absent: the family is present, and claiming otherwise would tell
		// a caller to stop trying the whole stage.
		return nil, bus.ModuleStatusInvalidRequest
	}
}

// The thresholds the contract fixes as float64 bit patterns.
//
// They are published as bits rather than decimals so the wire cannot drift from
// the policy through a decimal literal that rounds differently in two
// languages, which is a real hazard for a value compared against a stored
// column.
func effectivenessDemoteThreshold() float64 {
	return math.Float64frombits(db2contract.EffectivenessDemoteThresholdBits)
}

func effectivenessStatsLowThreshold() float64 {
	return math.Float64frombits(db2contract.EffectivenessStatsLowThresholdBits)
}
