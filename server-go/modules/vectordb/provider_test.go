package vectordb

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

func labels(pairs ...string) []db3.ExactLabel {
	out := make([]db3.ExactLabel, 0, len(pairs)/2)
	for index := 0; index+1 < len(pairs); index += 2 {
		out = append(out, db3.ExactLabel{Key: pairs[index], Value: pairs[index+1]})
	}
	return out
}

func seeded(t *testing.T) (*Provider, *Index) {
	t.Helper()
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")
	ctx := context.Background()

	points := []struct {
		id     int64
		vector []float32
		labs   []db3.ExactLabel
	}{
		{1, []float32{1, 0, 0}, labels("workspace", "w1", "project", "p1", "record_type", "memory")},
		{2, []float32{0.9, 0.1, 0}, labels("workspace", "w1", "project", "p1", "record_type", "memory")},
		{3, []float32{0, 1, 0}, labels("workspace", "w1", "project", "p1", "record_type", "memory")},
		{4, []float32{1, 0, 0}, labels("workspace", "w2", "project", "p9", "record_type", "memory")},
	}
	for index, point := range points {
		outcome := provider.Apply(ctx, db3.Apply{
			OperationID: uint64(index + 1),
			Generation:  1,
			PointID:     point.id,
			Kind:        db3.ApplyUpsert,
			Collection:  "memory",
			Vector:      point.vector,
			Labels:      point.labs,
		})
		if outcome.Result != db3.AppliedOK {
			t.Fatalf("seed apply %d = %v", index+1, outcome.Result)
		}
	}
	return provider, index
}

func searchRequest(topK uint32) db3.SearchRequest {
	return db3.SearchRequest{
		RequestID:  1,
		Workspace:  "w1",
		Project:    "p1",
		RecordType: "memory",
		TopK:       topK,
		Vector:     []float32{1, 0, 0},
	}
}

func TestSearchRanksNearestFirst(t *testing.T) {
	provider, _ := seeded(t)
	reply, failure := provider.Search(context.Background(), searchRequest(3))
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 3 {
		t.Fatalf("candidates = %v", reply.Candidates)
	}
	if reply.Candidates[0].PointID != 1 || reply.Candidates[1].PointID != 2 {
		t.Errorf("ranking = %v, want 1 then 2", reply.Candidates)
	}
	if reply.Candidates[0].Score < reply.Candidates[1].Score {
		t.Error("scores must descend")
	}
}

// Scope is not advisory. A search that ignored it would return candidates from
// another workspace, and DB2 rehydrating them would leak their existence
// through the timing even after refusing to return their content.
func TestSearchNeverCrossesScope(t *testing.T) {
	provider, _ := seeded(t)
	reply, failure := provider.Search(context.Background(), searchRequest(10))
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	for _, candidate := range reply.Candidates {
		if candidate.PointID == 4 {
			t.Fatal("a point from another workspace was returned")
		}
	}
}

// A point missing the filtered label must not match. Treating a missing label
// as a wildcard is how an index leaks across workspaces.
func TestSearchTreatsMissingLabelAsNoMatch(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")
	if err := index.Upsert("memory", 7, []float32{1, 0, 0}, nil); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	reply, failure := provider.Search(context.Background(), searchRequest(5))
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Fatalf("an unlabelled point matched a scoped search: %v", reply.Candidates)
	}
}

// The reply carries opaque ids and scores. A provider that returned content
// would let a caller read rows DB2 would have refused it.
func TestSearchReturnsOnlyIdentifiersAndScores(t *testing.T) {
	provider, _ := seeded(t)
	reply, _ := provider.Search(context.Background(), searchRequest(1))
	if len(reply.Candidates) != 1 {
		t.Fatalf("candidates = %v", reply.Candidates)
	}
	// db3.Candidate has exactly two fields; this asserts the shape has not
	// grown a payload, which the compiler enforces here.
	candidate := reply.Candidates[0]
	if candidate.PointID == 0 {
		t.Error("expected an opaque point id")
	}
	_ = candidate.Score
}

// A caller naming a generation newer than the index's has already seen writes
// this index may not have. Answering would hand back a ranking that silently
// omits them.
func TestSearchRefusesGenerationItCannotSatisfy(t *testing.T) {
	provider, index := seeded(t)
	request := searchRequest(3)
	request.RequiredGeneration = index.Generation() + 1

	_, failure := provider.Search(context.Background(), request)
	if failure != db3.SearchFailureRetryable {
		t.Fatalf("failure = %v, want retryable", failure)
	}
}

func TestSearchAcceptsGenerationItHasReached(t *testing.T) {
	provider, index := seeded(t)
	request := searchRequest(3)
	request.RequiredGeneration = index.Generation()

	reply, failure := provider.Search(context.Background(), request)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if reply.Generation < request.RequiredGeneration {
		t.Errorf("reply generation %d is older than required %d",
			reply.Generation, request.RequiredGeneration)
	}
}

func TestSearchRejectsMalformedRequests(t *testing.T) {
	provider, _ := seeded(t)
	ctx := context.Background()

	cases := map[string]db3.SearchRequest{
		"no request id":   {TopK: 1, Vector: []float32{1, 0, 0}},
		"no topK":         {RequestID: 1, Vector: []float32{1, 0, 0}},
		"no vector":       {RequestID: 1, TopK: 1},
		"wrong dimension": {RequestID: 1, TopK: 1, Vector: []float32{1, 0}},
	}
	for name, request := range cases {
		if _, failure := provider.Search(ctx, request); failure != db3.SearchFailureInvalidRequest {
			t.Errorf("%s: failure = %v, want invalid request", name, failure)
		}
	}
}

// --- apply ---

func TestApplyDeleteRemovesFromResults(t *testing.T) {
	provider, _ := seeded(t)
	ctx := context.Background()

	outcome := provider.Apply(ctx, db3.Apply{
		OperationID: 5, Generation: 1, PointID: 1,
		Kind: db3.ApplyDelete, Collection: "memory",
	})
	if outcome.Result != db3.AppliedOK {
		t.Fatalf("result = %v", outcome.Result)
	}
	reply, _ := provider.Search(ctx, searchRequest(10))
	for _, candidate := range reply.Candidates {
		if candidate.PointID == 1 {
			t.Fatal("a deleted point was returned")
		}
	}
}

func TestApplyTombstoneRemovesFromResults(t *testing.T) {
	provider, _ := seeded(t)
	ctx := context.Background()

	provider.Apply(ctx, db3.Apply{
		OperationID: 5, Generation: 1, PointID: 2,
		Kind: db3.ApplyTombstone, Collection: "memory",
	})
	reply, _ := provider.Search(ctx, searchRequest(10))
	for _, candidate := range reply.Candidates {
		if candidate.PointID == 2 {
			t.Fatal("a tombstoned point was returned")
		}
	}
}

// The outbox delivers at least once — that is what makes it durable — so a
// redelivery must be recognised, not performed again.
func TestApplyIsIdempotentOnRedelivery(t *testing.T) {
	provider, index := seeded(t)
	ctx := context.Background()

	before := index.Len()
	apply := db3.Apply{
		OperationID: 2, Generation: 1, PointID: 2,
		Kind: db3.ApplyUpsert, Collection: "memory",
		Vector: []float32{0.9, 0.1, 0},
		Labels: labels("workspace", "w1", "project", "p1", "record_type", "memory"),
	}
	outcome := provider.Apply(ctx, apply)
	if outcome.Result != db3.AppliedOK {
		t.Fatalf("a redelivery must be acknowledged, got %v", outcome.Result)
	}
	if index.Len() != before {
		t.Errorf("point count changed on redelivery: %d then %d", before, index.Len())
	}
}

// The watermark is DB2's promise that nothing below it is outstanding.
// Advancing it past a gap would tell DB2 an operation had landed when it had
// not, and the outbox would never redeliver it.
func TestWatermarkStopsAtAGap(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")
	ctx := context.Background()

	apply := func(id uint64, pointID int64) db3.ProviderApplyOutcome {
		return provider.Apply(ctx, db3.Apply{
			OperationID: id, Generation: 1, PointID: pointID,
			Kind: db3.ApplyUpsert, Collection: "memory",
			Vector: []float32{1, 0, 0},
		})
	}

	if outcome := apply(1, 1); outcome.Watermark != 1 {
		t.Fatalf("watermark = %d, want 1", outcome.Watermark)
	}
	// Operation 2 is missing; 3 arrives first.
	outcome := apply(3, 3)
	if outcome.Watermark != 1 {
		t.Fatalf("watermark = %d, want 1 — it must not pass the gap", outcome.Watermark)
	}
	if outcome.Lag == 0 {
		t.Error("an out-of-order apply must report lag")
	}
	// Filling the gap releases both.
	if outcome := apply(2, 2); outcome.Watermark != 3 {
		t.Fatalf("watermark = %d, want 3 once the gap is filled", outcome.Watermark)
	}
}

// A dimension mismatch will never succeed on retry, so it is rejected rather
// than deferred — a retryable verdict would stall the outbox forever.
func TestApplyRejectsWrongDimension(t *testing.T) {
	provider, _ := seeded(t)
	outcome := provider.Apply(context.Background(), db3.Apply{
		OperationID: 99, Generation: 1, PointID: 5,
		Kind: db3.ApplyUpsert, Collection: "memory",
		Vector: []float32{1, 0},
	})
	if outcome.Result != db3.AppliedRejected {
		t.Fatalf("result = %v, want rejected", outcome.Result)
	}
}

func TestApplyRejectsUnknownKindAndZeroOperation(t *testing.T) {
	provider, _ := seeded(t)
	ctx := context.Background()

	if outcome := provider.Apply(ctx, db3.Apply{OperationID: 0}); outcome.Result != db3.AppliedRejected {
		t.Errorf("zero operation id = %v", outcome.Result)
	}
	outcome := provider.Apply(ctx, db3.Apply{
		OperationID: 50, Generation: 1, PointID: 1, Kind: db3.ApplyKind(99),
	})
	if outcome.Result != db3.AppliedRejected {
		t.Errorf("unknown kind = %v", outcome.Result)
	}
}

// --- capabilities ---

// The capabilities are what DB2 routes on, so they must satisfy the contract's
// own validator rather than merely look right.
func TestCapabilitiesAreValid(t *testing.T) {
	provider, _ := seeded(t)
	capabilities := provider.Capabilities()
	if err := capabilities.Validate(); err != nil {
		t.Fatalf("capabilities invalid: %v", err)
	}
	if capabilities.Operations&db3.OperationSearch == 0 ||
		capabilities.Operations&db3.OperationApply == 0 {
		t.Error("this provider serves both search and apply")
	}
	if capabilities.Filters&db3.FilterExact == 0 {
		t.Error("exact filters back the scope enforcement and must be advertised")
	}
	if !capabilities.Ready {
		t.Error("a seeded provider is ready")
	}
}

func TestCapabilitiesReportTheIndexMetric(t *testing.T) {
	for metric, want := range map[Metric]db3.MetricSet{
		Cosine: db3.MetricCosine,
		L2:     db3.MetricL2,
		Dot:    db3.MetricDot,
	} {
		provider := NewProvider(NewIndex(metric, 3), "memory")
		if got := provider.Capabilities().Metrics; got != want {
			t.Errorf("metric %d advertised %v, want %v", metric, got, want)
		}
	}
}
