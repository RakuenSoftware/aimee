package db2

import (
	"context"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// textRow answers one text column.
type textRow struct {
	value string
	err   error
}

func (r textRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) != 1 {
		return errors.New("scan arity mismatch")
	}
	target, ok := dest[0].(*string)
	if !ok {
		return errors.New("unsupported scan target")
	}
	*target = r.value
	return nil
}

// A marker that is present but unparsable reads as absent. The kb consults this
// to decide whether to serve vector search, and a garbled marker must not wedge
// search off permanently.
func TestReembedStatusTreatsGarbledMarkerAsAbsent(t *testing.T) {
	for _, marker := range []string{"", "not-a-marker", ":", "0:0", "1536:", ":123", "abc:123", "1536:abc"} {
		backend := NewPGLifecycleBackend(LifecycleSeams{
			QueryRow: func(context.Context, string, ...any) HealthRow {
				return textRow{value: marker}
			},
		})
		running, _, err := backend.ReembedStatus(context.Background())
		if err != nil {
			t.Errorf("marker %q errored: %v", marker, err)
		}
		if running {
			t.Errorf("marker %q must read as absent", marker)
		}
	}
}

func TestReembedStatusParsesMarker(t *testing.T) {
	backend := NewPGLifecycleBackend(LifecycleSeams{
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return textRow{value: "1536:1750000000"}
		},
	})
	running, status, err := backend.ReembedStatus(context.Background())
	if err != nil || !running {
		t.Fatalf("running = %v, err = %v", running, err)
	}
	if status.TargetDimension != 1536 || status.StartedEpoch != 1750000000 {
		t.Errorf("status = %+v", status)
	}
}

// No row means no re-embed, which is NotFound carrying no payload — a different
// answer from a running re-embed whose fields happen to be zero.
func TestLifecycleHandlerReportsNoReembedAsNotFound(t *testing.T) {
	backend := NewPGLifecycleBackend(LifecycleSeams{
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return fakeRow{err: errors.New("no rows in result set")}
		},
	})
	// The scripted error is not the no-rows sentinel, so drive the absent path
	// through an empty marker instead.
	backend = NewPGLifecycleBackend(LifecycleSeams{
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return textRow{value: ""}
		},
	})
	reply, status := NewLifecycleHandler(backend)(lifecycleInvocation(),
		db2contract.EncodeReembedStatusRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, decoded, err := db2contract.DecodeReembedStatusReply(reply)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if result != db2contract.ResultNotFound {
		t.Errorf("result = %d, want NotFound", result)
	}
	if decoded != (db2contract.ReembedStatus{}) {
		t.Errorf("decoded = %+v, want empty", decoded)
	}
}

// --- clear maintenance ---

func maintenanceSeams(marker string, recorded, running uint32) LifecycleSeams {
	return LifecycleSeams{
		QueryRow: func(_ context.Context, query string, _ ...any) HealthRow {
			switch query {
			case sqlReembedMarker:
				return textRow{value: marker}
			case sqlKBMetaExists:
				return fakeRow{values: []any{int64(1)}}
			case sqlRecordedDimension:
				return textRow{value: itoaU32(recorded)}
			}
			return fakeRow{err: errors.New("unexpected query")}
		},
		Exec: func(context.Context, string, ...any) (int64, error) { return 1, nil },
		Runtime: RuntimeState{
			Dimension: func(context.Context) (uint32, error) { return running, nil },
		},
	}
}

// The dangerous case is a recorded width that disagrees with the running one:
// clearing there resumes vector search against a store still mid-transition.
func TestClearMaintenanceRefusesMismatchWithoutForce(t *testing.T) {
	backend := NewPGLifecycleBackend(maintenanceSeams("1536:1750000000", 768, 1536))
	cleared, status, err := backend.ReembedClearMaintenance(context.Background(), false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if cleared {
		t.Fatal("a mismatched clear must be refused without force")
	}
	if status.RecordedDimension != 768 || status.RunningDimension != 1536 {
		t.Errorf("status = %+v", status)
	}
	if status.WasInProgress != 1 {
		t.Error("the marker was set and must be reported so")
	}
}

// The common stuck case — the re-embed finished and the marker outlived a crash
// — has matching widths and clears without force. Requiring force everywhere
// would train operators to pass it reflexively.
func TestClearMaintenanceClearsMatchingWidthsWithoutForce(t *testing.T) {
	backend := NewPGLifecycleBackend(maintenanceSeams("1536:1750000000", 1536, 1536))
	cleared, _, err := backend.ReembedClearMaintenance(context.Background(), false)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if !cleared {
		t.Fatal("matching widths must clear without force")
	}
}

func TestClearMaintenanceForceOverridesMismatch(t *testing.T) {
	backend := NewPGLifecycleBackend(maintenanceSeams("1536:1750000000", 768, 1536))
	cleared, _, err := backend.ReembedClearMaintenance(context.Background(), true)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if !cleared {
		t.Fatal("force must clear a mismatch")
	}
}

// A refusal is Conflict carrying the two disagreeing widths, because the
// operator's next decision depends on seeing them.
func TestLifecycleHandlerReportsRefusedClearAsConflict(t *testing.T) {
	handler := NewLifecycleHandler(NewPGLifecycleBackend(maintenanceSeams("1536:1750000000", 768, 1536)))
	request, err := db2contract.EncodeReembedClearMaintenanceRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := handler(lifecycleInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, decoded, err := db2contract.DecodeReembedClearMaintenanceReply(reply)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if result != db2contract.ResultConflict {
		t.Errorf("result = %d, want Conflict", result)
	}
	if decoded.RecordedDimension != 768 || decoded.RunningDimension != 1536 {
		t.Errorf("the operator must see both widths, got %+v", decoded)
	}
}

// --- recorded dimension ---

// A fresh database has no kb_meta at all. Reading a missing table as a fault
// would fail the operation on exactly the databases where the answer is simply
// "nothing recorded yet".
func TestRecordedDimensionIsZeroOnAFreshDatabase(t *testing.T) {
	backend := &pgLifecycleBackend{queryRow: func(_ context.Context, query string, _ ...any) HealthRow {
		if query == sqlKBMetaExists {
			return fakeRow{values: []any{int64(0)}}
		}
		return fakeRow{err: errors.New("relation kb_meta does not exist")}
	}}
	recorded, err := backend.recordedDimension(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if recorded != 0 {
		t.Errorf("recorded = %d, want 0", recorded)
	}
}

// A garbage or out-of-range row is not evidence of a width.
func TestRecordedDimensionIgnoresUnusableValues(t *testing.T) {
	for _, value := range []string{"", "abc", "0", "-1", "999999"} {
		backend := &pgLifecycleBackend{queryRow: func(_ context.Context, query string, _ ...any) HealthRow {
			if query == sqlKBMetaExists {
				return fakeRow{values: []any{int64(1)}}
			}
			return textRow{value: value}
		}}
		recorded, err := backend.recordedDimension(context.Background())
		if err != nil {
			t.Errorf("value %q errored: %v", value, err)
		}
		if recorded != 0 {
			t.Errorf("value %q read as width %d", value, recorded)
		}
	}
}

// --- runtime state ---

// The contract ties the two refusal fields together: a count implies a width
// was offered, and an offered width implies something refused it.
func TestEmbeddingRefusalsRejectsInconsistentPair(t *testing.T) {
	for _, refusals := range []db2contract.EmbeddingRefusals{
		{RefusedCount: 5, LastOffered: 0},
		{RefusedCount: 0, LastOffered: 768},
	} {
		backend := NewPGLifecycleBackend(LifecycleSeams{
			Runtime: RuntimeState{
				Refusals: func(context.Context) (db2contract.EmbeddingRefusals, error) {
					return refusals, nil
				},
			},
		})
		if _, err := backend.EmbeddingRefusals(context.Background()); err == nil {
			t.Errorf("%+v was accepted; it describes a state that cannot happen", refusals)
		}
	}
}

func TestEmbeddingRefusalsAcceptsConsistentPairs(t *testing.T) {
	for _, refusals := range []db2contract.EmbeddingRefusals{
		{RefusedCount: 0, LastOffered: 0},
		{RefusedCount: 5, LastOffered: 768},
	} {
		backend := NewPGLifecycleBackend(LifecycleSeams{
			Runtime: RuntimeState{
				Refusals: func(context.Context) (db2contract.EmbeddingRefusals, error) {
					return refusals, nil
				},
			},
		})
		if _, err := backend.EmbeddingRefusals(context.Background()); err != nil {
			t.Errorf("%+v was refused: %v", refusals, err)
		}
	}
}

// A seam this module was not given is a capability answer, not a sick database.
// Collapsing the two made an unconfigured module look like a failing one.
func TestAbsentRuntimeSeamReportsCapabilityAbsent(t *testing.T) {
	handler := NewLifecycleHandler(NewPGLifecycleBackend(LifecycleSeams{}))
	for name, request := range map[string][]byte{
		"embedding_dimension": db2contract.EncodeEmbeddingDimensionRequest(),
		"embedding_refusals":  db2contract.EncodeEmbeddingRefusalsRequest(),
		"embedder_serving_id": db2contract.EncodeEmbedderServingIDRequest(),
		"reembed_clear":       db2contract.EncodeReembedClearRequest(),
	} {
		if _, status := handler(lifecycleInvocation(), request); status != bus.ModuleStatusCapabilityAbsent {
			t.Errorf("%s answered %v, want capability absent", name, status)
		}
	}
}

func itoaU32(v uint32) string {
	if v == 0 {
		return "0"
	}
	var digits [10]byte
	index := len(digits)
	for v > 0 {
		index--
		digits[index] = byte('0' + v%10)
		v /= 10
	}
	return string(digits[index:])
}
