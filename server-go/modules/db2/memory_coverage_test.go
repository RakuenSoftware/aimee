package db2

import (
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// memoryOperations is every operation the memory family declares, paired with a
// valid request for it.
//
// This list is the completeness guard. The handler's switch returns
// InvalidRequest for an operation it does not serve, which is indistinguishable
// from a malformed envelope at the call site — so without this test, adding an
// operation to the contract and forgetting to serve it would look exactly like
// a caller sending garbage.
func memoryOperations(t *testing.T) map[string][]byte {
	t.Helper()
	must := func(request []byte, err error) []byte {
		t.Helper()
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		return request
	}
	return map[string][]byte{
		"level3_count":            db2contract.EncodeLevel3CountRequest(),
		"level2_count":            db2contract.EncodeLevel2CountRequest(),
		"orphaned_l0_count":       db2contract.EncodeOrphanedL0CountRequest(),
		"total_count":             db2contract.EncodeTotalCountRequest(),
		"session_l2_count":        must(db2contract.EncodeSessionL2CountRequest("s")),
		"key_exists":              must(db2contract.EncodeKeyExistsRequest("k")),
		"find_id_by_key_kind":     must(db2contract.EncodeFindIDByKeyKindRequest("k", "fact")),
		"key_exists_in_tier_pair": must(db2contract.EncodeKeyExistsInTierPairRequest("k", "L2", "L3")),
		"effectiveness_update":    must(db2contract.EncodeEffectivenessUpdateRequest(1, 1, 0.5)),
		"retention_enforce":       db2contract.EncodeRetentionEnforceRequest(),
		"effectiveness_demote":    db2contract.EncodeEffectivenessDemoteRequest(),
		"effectiveness_stats":     db2contract.EncodeEffectivenessStatsRequest(),
		"l2_memory_ids":           db2contract.EncodeL2MemoryIDsRequest(),
		"health_record":           must(db2contract.EncodeHealthRecordRequest(0, 0, 0)),
		"health_retention":        db2contract.EncodeHealthRetentionRequest(),
		"health_counters":         db2contract.EncodeHealthCountersRequest(),
		"stats_counts":            db2contract.EncodeStatsCountsRequest(),
		"expire":                  db2contract.EncodeExpireRequest(),
		"demote":                  db2contract.EncodeDemoteRequest(),
		"promote_stable":          db2contract.EncodePromoteStableRequest(),
		"reclassify_directives":   must(db2contract.EncodeReclassifyDirectivesRequest(0)),
		"record_l4_approval":      must(db2contract.EncodeRecordL4ApprovalRequest(1, "jb", "")),
	}
}

// The family declares twenty-two operations. If the contract grows one, this
// count fails first and names what the rest of the test is about to miss.
const declaredMemoryOperations = 22

func TestMemoryHandlerServesEveryDeclaredOperation(t *testing.T) {
	requests := memoryOperations(t)
	if len(requests) != declaredMemoryOperations {
		t.Fatalf("the guard covers %d operations, want %d", len(requests), declaredMemoryOperations)
	}

	// A backend whose sweeps have something to iterate, so the orchestrated
	// operations reach their backend calls rather than short-circuiting on an
	// empty kind list.
	backend := &fakeMemory{lifecycle: fakeLifecycle{
		kindsByTier: map[string][]string{
			db2contract.ExpireStaleTier: {"fact"},
			db2contract.DemoteTier:      {"fact"},
		},
		expireDays: map[string]uint32{"fact": 30},
		demoteDays: map[string]uint32{"fact": 60},
		demoteConf: map[string]float64{"fact": 0.7},
	}}
	handler := NewMemoryHandler(backend)

	for name, request := range requests {
		t.Run(name, func(t *testing.T) {
			_, status := handler(memoryInvocation(), request)
			if status == bus.ModuleStatusInvalidRequest {
				t.Fatalf("operation %s is declared but not served", name)
			}
			if status != bus.ModuleStatusOK {
				t.Fatalf("operation %s answered %v", name, status)
			}
		})
	}
}
