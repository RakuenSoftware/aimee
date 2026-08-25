package vectordb_test

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
	db2module "github.com/JBailes/aimee/server-go/modules/db2"
	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// This is the end-to-end proof of the externalization path: DB2's real router,
// routing a real search to a real external provider, with pgvector as the
// fallback. Until it existed, both halves were tested only against fakes of
// each other.

const providerPrincipal = db3.ProviderRefFirst

func buildProvider(t *testing.T) *vectordb.Provider {
	t.Helper()
	index := vectordb.NewIndex(vectordb.Cosine, 3)
	provider := vectordb.NewProvider(index, "memory")

	seeds := []struct {
		id     int64
		vector []float32
	}{
		{1, []float32{1, 0, 0}},
		{2, []float32{0.8, 0.2, 0}},
		{3, []float32{0, 0, 1}},
	}
	for offset, seed := range seeds {
		outcome := provider.Apply(context.Background(), db3.Apply{
			OperationID: uint64(offset + 1),
			Generation:  1,
			PointID:     seed.id,
			Kind:        db3.ApplyUpsert,
			Collection:  "memory",
			Vector:      seed.vector,
			Labels: []db3.ExactLabel{
				{Key: "workspace", Value: "w1"},
				{Key: "project", Value: "p1"},
				{Key: "record_type", Value: "memory"},
			},
		})
		if outcome.Result != db3.AppliedOK {
			t.Fatalf("seed %d: %v", seed.id, outcome.Result)
		}
	}
	return provider
}

// routerFor wires the real router to the provider, recording whether the
// internal pgvector path was taken.
func routerFor(t *testing.T, provider *vectordb.Provider, internalCalled *bool) *db2module.DB3Router {
	t.Helper()

	internal := func(context.Context, db3.SearchRequest) (db3.SearchReply, error) {
		*internalCalled = true
		return db3.SearchReply{RequestID: 1, Generation: 1}, nil
	}
	external := func(ctx context.Context, _ uint32, request db3.SearchRequest) (db2module.DB3SearchResponse, error) {
		reply, failure := provider.Search(ctx, request)
		if failure != 0 {
			return db2module.DB3SearchResponse{
				Failure: &db3.SearchFailure{RequestID: request.RequestID, Code: failure},
			}, nil
		}
		return db2module.DB3SearchResponse{Reply: &reply}, nil
	}
	// DB2 authorizes every candidate the provider returns. The provider's
	// answer is a shortlist, never a permission.
	authorize := func(context.Context, string, string, int64) (bool, error) { return true, nil }

	router, err := db2module.NewDB3Router(internal, external, authorize)
	if err != nil {
		t.Fatalf("router: %v", err)
	}
	return router
}

func TestRouterServesSearchFromTheExternalProvider(t *testing.T) {
	provider := buildProvider(t)
	internalCalled := false
	router := routerFor(t, provider, &internalCalled)

	capabilities := provider.Capabilities()
	if err := router.ObserveCapabilities(providerPrincipal, 1, 1, capabilities); err != nil {
		t.Fatalf("observe: %v", err)
	}

	outcome := router.Search(context.Background(), db3.SearchRequest{
		RequestID:          1,
		RequiredGeneration: capabilities.Generation,
		Workspace:          "w1",
		Project:            "p1",
		RecordType:         "memory",
		TopK:               2,
		Vector:             []float32{1, 0, 0},
	})

	if outcome.Result != db2module.DB3OK {
		t.Fatalf("result = %v", outcome.Result)
	}
	if outcome.Route != db2module.DB3External {
		t.Fatalf("route = %v, want external", outcome.Route)
	}
	if internalCalled {
		t.Error("pgvector must not be consulted once a provider is serving")
	}
	if len(outcome.Reply.Candidates) != 2 {
		t.Fatalf("candidates = %v", outcome.Reply.Candidates)
	}
	if outcome.Reply.Candidates[0].PointID != 1 {
		t.Errorf("ranking = %v, want the nearest point first", outcome.Reply.Candidates)
	}
}

// With no provider attached, the same search must still be answered — by
// pgvector. The externalization is an optimisation, not a dependency.
func TestRouterFallsBackToPGVectorWithoutAProvider(t *testing.T) {
	provider := buildProvider(t)
	internalCalled := false
	router := routerFor(t, provider, &internalCalled)

	outcome := router.Search(context.Background(), db3.SearchRequest{
		RequestID:          1,
		RequiredGeneration: 1,
		Workspace:          "w1",
		Project:            "p1",
		RecordType:         "memory",
		TopK:               2,
		Vector:             []float32{1, 0, 0},
	})

	// Result is asserted before Route: DB3DefaultPGVector is the zero value, so
	// a rejected request would otherwise read as a successful fallback.
	if outcome.Result != db2module.DB3OK {
		t.Fatalf("result = %v", outcome.Result)
	}
	if outcome.Route != db2module.DB3DefaultPGVector {
		t.Fatalf("route = %v, want pgvector", outcome.Route)
	}
	if !internalCalled {
		t.Error("pgvector must answer when no provider is serving")
	}
}

// A provider that withdraws must not strand the search. This is the property
// that makes an external index safe to deploy: losing it costs latency, not
// availability.
func TestRouterReturnsToPGVectorWhenTheProviderWithdraws(t *testing.T) {
	provider := buildProvider(t)
	internalCalled := false
	router := routerFor(t, provider, &internalCalled)

	capabilities := provider.Capabilities()
	if err := router.ObserveCapabilities(providerPrincipal, 1, 1, capabilities); err != nil {
		t.Fatalf("observe: %v", err)
	}
	if !router.RemoveProvider(providerPrincipal, 1) {
		t.Fatal("expected the provider to be removed")
	}

	outcome := router.Search(context.Background(), db3.SearchRequest{
		RequestID:          1,
		RequiredGeneration: 1,
		Workspace:          "w1",
		Project:            "p1",
		RecordType:         "memory",
		TopK:               2,
		Vector:             []float32{1, 0, 0},
	})
	if outcome.Result != db2module.DB3OK {
		t.Fatalf("result = %v", outcome.Result)
	}
	if outcome.Route != db2module.DB3DefaultPGVector {
		t.Fatalf("route = %v, want pgvector after withdrawal", outcome.Route)
	}
	if !internalCalled {
		t.Error("pgvector must answer once the provider is gone")
	}
}

// The provider band is reserved. A ref outside it derives another module's
// event kinds, and whichever attaches second is denied — possibly the core
// module, with nothing in its own log to explain why.
func TestRouterRefusesAProviderOutsideTheReservedBand(t *testing.T) {
	provider := buildProvider(t)
	internalCalled := false
	router := routerFor(t, provider, &internalCalled)

	for _, ref := range []uint32{1, 28, db3.ProviderRefFirst - 1, db3.ProviderRefLimit} {
		if err := router.ObserveCapabilities(ref, 1, 1, provider.Capabilities()); err == nil {
			t.Errorf("ref %d was accepted; it is outside the reserved band", ref)
		}
	}
}
