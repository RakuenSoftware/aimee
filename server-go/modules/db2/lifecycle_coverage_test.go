package db2

import (
	"context"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The family declares ten operations. If the contract grows one, this count
// fails first and names what the rest of the test is about to miss.
const declaredLifecycleOperations = 10

// fullLifecycleSeams answers every probe the family makes, so the guard
// exercises dispatch rather than a backend that happens to be unconfigured.
func fullLifecycleSeams(h *resetHarness) LifecycleSeams {
	seams := h.seams()
	base := seams.QueryRow
	seams.QueryRow = func(ctx context.Context, query string, args ...any) HealthRow {
		switch query {
		case healthQuery:
			return healthRow{}
		case sqlReembedMarker:
			return textRow{value: "1536:1750000000"}
		case sqlActiveConnections, sqlMaxConnections, sqlIsReplica, sqlReplicaLag:
			return fakeRow{values: []any{int64(1)}}
		}
		return base(ctx, query, args...)
	}
	seams.Exec = func(context.Context, string, ...any) (int64, error) { return 1, nil }
	seams.PoolStats = func(context.Context) (db2contract.PoolStatus, error) {
		return db2contract.PoolStatus{Size: 4, InUse: 1}, nil
	}
	seams.Runtime = RuntimeState{
		Dimension: func(context.Context) (uint32, error) { return 1536, nil },
		Refusals: func(context.Context) (db2contract.EmbeddingRefusals, error) {
			return db2contract.EmbeddingRefusals{RefusedCount: 2, LastOffered: 768}, nil
		},
		ServingID: func(context.Context) (string, error) { return "embedder-v3", nil },
	}
	return seams
}

func lifecycleOperations(t *testing.T) map[string][]byte {
	t.Helper()
	must := func(request []byte, err error) []byte {
		t.Helper()
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		return request
	}
	return map[string][]byte{
		"health":                    db2contract.EncodeHealthRequest(),
		"embedding_dimension":       db2contract.EncodeEmbeddingDimensionRequest(),
		"pool_status":               db2contract.EncodePoolStatusRequest(),
		"embedding_refusals":        db2contract.EncodeEmbeddingRefusalsRequest(),
		"postgres_status":           db2contract.EncodePostgresStatusRequest(),
		"reembed_status":            db2contract.EncodeReembedStatusRequest(),
		"reembed_clear":             db2contract.EncodeReembedClearRequest(),
		"reembed_clear_maintenance": must(db2contract.EncodeReembedClearMaintenanceRequest(0)),
		"embedder_serving_id":       db2contract.EncodeEmbedderServingIDRequest(),
		"dimension_reset":           must(db2contract.EncodeDimensionResetRequest(1536, 0, 1)),
	}
}

func TestLifecycleHandlerServesEveryDeclaredOperation(t *testing.T) {
	requests := lifecycleOperations(t)
	if len(requests) != declaredLifecycleOperations {
		t.Fatalf("the guard covers %d operations, want %d", len(requests), declaredLifecycleOperations)
	}

	harness := &resetHarness{recorded: 1536, vectorTables: []string{"kb_embeddings"}}
	handler := NewLifecycleHandler(NewPGLifecycleBackend(fullLifecycleSeams(harness)))

	for name, request := range requests {
		t.Run(name, func(t *testing.T) {
			_, status := handler(lifecycleInvocation(), request)
			if status == bus.ModuleStatusInvalidRequest {
				t.Fatalf("operation %s is declared but not served", name)
			}
			if status != bus.ModuleStatusOK {
				t.Fatalf("operation %s answered %v", name, status)
			}
		})
	}
}

// Both families together cover the whole declared catalog. The contract's
// operations.json still records catalog_complete: false, which is about the
// families DB2 has yet to declare — not about these two, which are now whole.
func TestBothFamiliesCoverTheDeclaredCatalog(t *testing.T) {
	memory := len(memoryOperations(t))
	lifecycle := len(lifecycleOperations(t))
	if memory+lifecycle != 32 {
		t.Fatalf("served %d operations across both families, want the declared 32",
			memory+lifecycle)
	}
}

// An operation id the family does not define is invalid, not absent: the stage
// is present and telling a caller otherwise would stop it trying the family.
func TestLifecycleHandlerRejectsUnknownOperation(t *testing.T) {
	harness := &resetHarness{recorded: 1536, vectorTables: []string{"kb_embeddings"}}
	handler := NewLifecycleHandler(NewPGLifecycleBackend(fullLifecycleSeams(harness)))

	request, err := db2contract.EncodeRequestHeader(9999, 0, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(lifecycleInvocation(), request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want invalid request", status)
	}
}

// The harness must not be answering by accident: an unscripted query has to
// surface rather than return a plausible zero.
func TestResetHarnessRefusesUnscriptedQueries(t *testing.T) {
	harness := &resetHarness{recorded: 768}
	row := harness.seams().QueryRow(context.Background(), "SELECT something_unexpected")
	var value int64
	err := row.Scan(&value)
	if err == nil || !strings.Contains(err.Error(), "unexpected query") {
		t.Fatalf("err = %v, want an unexpected-query failure", err)
	}
}
