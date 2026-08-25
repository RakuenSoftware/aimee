package postgres

import (
	"context"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

func routableRequest() db3.SearchRequest {
	return db3.SearchRequest{
		RequestID: 1, RequiredGeneration: 7, Workspace: "w1", Project: "p1",
		RecordType: "memory", TopK: 3, Vector: []float32{1, 0, 0},
	}
}

type fakeSearcher struct {
	calls     int
	principal uint32
	reply     db3.SearchReply
	err       error
}

func (f *fakeSearcher) Search(_ context.Context, principal uint32,
	request db3.SearchRequest) (db3.SearchReply, error) {
	f.calls++
	f.principal = principal
	if f.err != nil {
		return db3.SearchReply{}, f.err
	}
	reply := f.reply
	if reply.RequestID == 0 {
		reply.RequestID = request.RequestID
		reply.Generation = request.RequiredGeneration
	}
	return reply, nil
}

func readyProvider(generation uint64) db3.Capabilities {
	return db3.Capabilities{
		Generation: generation, Operations: db3.OperationSearch | db3.OperationApply,
		Metrics: db3.MetricCosine, Filters: db3.FilterExact,
		MaxDimension: db3.MaxDimension, MaxBatch: 8, MaxTopK: db3.MaxTopK, Ready: true,
	}
}

func newRouter(t *testing.T, searcher ProviderSearcher) (*VectorRouter, *int) {
	t.Helper()
	fallbacks := 0
	router, err := NewVectorRouter(db3.NewProviderRegistry(), searcher,
		func(_ context.Context, request db3.SearchRequest) (db3.SearchReply, error) {
			fallbacks++
			return db3.SearchReply{
				RequestID: request.RequestID, Generation: request.RequiredGeneration,
				Candidates: []db3.Candidate{{PointID: 99, Score: 0.1}},
			}, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	return router, &fallbacks
}

func TestWithNoProviderInstalledEverythingRunsOnPostgreSQL(t *testing.T) {
	// The default deployment. A DB3 provider is optional, and its absence is
	// not a degraded mode -- it is the mode.
	router, fallbacks := newRouter(t, nil)
	reply, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RoutePostgreSQL {
		t.Errorf("route = %v, want postgresql", route)
	}
	if *fallbacks != 1 || len(reply.Candidates) != 1 {
		t.Errorf("PostgreSQL did not answer (%d calls, %d candidates)", *fallbacks, len(reply.Candidates))
	}
	if router.Registry().Len() != 0 {
		t.Error("a registry with no provider is not empty")
	}
}

func TestAReadyProviderAtTheRequiredGenerationAnswers(t *testing.T) {
	searcher := &fakeSearcher{}
	router, fallbacks := newRouter(t, searcher)
	if !router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7)) {
		t.Fatal("the registry refused a valid provider")
	}
	_, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteProvider {
		t.Fatalf("route = %v, want provider", route)
	}
	if searcher.calls != 1 || searcher.principal != db3.ProviderRefFirst {
		t.Errorf("provider was asked %d times as principal %d", searcher.calls, searcher.principal)
	}
	if *fallbacks != 0 {
		t.Error("PostgreSQL was queried even though the provider answered")
	}
}

func TestAProviderAtTheWrongGenerationIsNotUsed(t *testing.T) {
	// Behind means it has not caught up with DB2's writes. Ahead cannot answer
	// at all: the wire requires the reply to carry exactly the requested
	// generation. Either way PostgreSQL answers, and it is always current.
	for _, generation := range []uint64{6, 8} {
		searcher := &fakeSearcher{}
		router, fallbacks := newRouter(t, searcher)
		router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(generation))
		_, route, err := router.Search(context.Background(), routableRequest())
		if err != nil {
			t.Fatal(err)
		}
		if route != RoutePostgreSQL || searcher.calls != 0 || *fallbacks != 1 {
			t.Errorf("generation %d: route=%v provider-calls=%d fallbacks=%d",
				generation, route, searcher.calls, *fallbacks)
		}
	}
}

func TestAnUnreadyProviderIsNotUsed(t *testing.T) {
	// Attached but still filling its index. It would answer correctly and
	// emptily, which is worse than not being used at all.
	searcher := &fakeSearcher{}
	router, fallbacks := newRouter(t, searcher)
	unready := readyProvider(7)
	unready.Ready = false
	router.Registry().Observe(db3.ProviderRefFirst, 1, 1, unready)
	_, route, _ := router.Search(context.Background(), routableRequest())
	if route != RoutePostgreSQL || searcher.calls != 0 || *fallbacks != 1 {
		t.Errorf("an unready provider was used (route=%v calls=%d)", route, searcher.calls)
	}
}

func TestAFailingProviderFallsBackRatherThanReturningNothing(t *testing.T) {
	// An empty result is indistinguishable from a corpus with no matches, so a
	// broken provider would become a silent loss of recall.
	searcher := &fakeSearcher{err: errors.New("provider is down")}
	router, fallbacks := newRouter(t, searcher)
	router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7))
	reply, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteFellBack {
		t.Fatalf("route = %v, want fell-back", route)
	}
	if *fallbacks != 1 || len(reply.Candidates) != 1 {
		t.Errorf("PostgreSQL did not answer after the provider failed")
	}
}

func TestAMalformedProviderReplyFallsBack(t *testing.T) {
	// A reply the wire refuses is not an answer. Accepting it would let a
	// provider return candidates for a different request.
	searcher := &fakeSearcher{reply: db3.SearchReply{RequestID: 999, Generation: 7}}
	router, fallbacks := newRouter(t, searcher)
	router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7))
	_, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteFellBack || *fallbacks != 1 {
		t.Errorf("a malformed reply was accepted (route=%v fallbacks=%d)", route, *fallbacks)
	}
}

func TestFellBackIsDistinctFromNeverRouted(t *testing.T) {
	// "No provider installed" and "the provider is failing" look identical in
	// the results and could not be more different operationally.
	searcher := &fakeSearcher{err: errors.New("down")}
	failing, _ := newRouter(t, searcher)
	failing.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7))
	_, failedRoute, _ := failing.Search(context.Background(), routableRequest())

	absent, _ := newRouter(t, nil)
	_, absentRoute, _ := absent.Search(context.Background(), routableRequest())

	if failedRoute == absentRoute {
		t.Fatalf("both reported %v; a deployment cannot tell a broken provider from none", failedRoute)
	}
}

func TestAnOperationTheWireCannotCarryStaysOnPostgreSQL(t *testing.T) {
	// Not every vector operation can leave. A request the contract refuses is
	// not a failure; it is an operation that runs where it always did.
	searcher := &fakeSearcher{}
	router, fallbacks := newRouter(t, searcher)
	router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7))

	unroutable := routableRequest()
	unroutable.TopK = 0
	if RoutableSearch(unroutable) {
		t.Fatal("a request with no top-k was judged routable")
	}
	_, route, _ := router.Search(context.Background(), unroutable)
	if route != RoutePostgreSQL || searcher.calls != 0 || *fallbacks != 1 {
		t.Errorf("an unroutable request reached a provider (route=%v calls=%d)", route, searcher.calls)
	}
}

func TestACancelledContextDoesNotSpendPostgreSQL(t *testing.T) {
	// The caller has gone. Running the query anyway would spend the database on
	// an answer nobody is waiting for.
	searcher := &fakeSearcher{err: errors.New("down")}
	router, fallbacks := newRouter(t, searcher)
	router.Registry().Observe(db3.ProviderRefFirst, 1, 1, readyProvider(7))
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, _, err := router.Search(ctx, routableRequest())
	if err == nil {
		t.Fatal("a cancelled search reported success")
	}
	if *fallbacks != 0 {
		t.Errorf("PostgreSQL was queried for a cancelled caller (%d times)", *fallbacks)
	}
}

func TestARouterWithoutAPostgreSQLPathIsRefused(t *testing.T) {
	// PostgreSQL is never optional; DB3 always is. A router built the other way
	// round would make an optional component load-bearing.
	if _, err := NewVectorRouter(db3.NewProviderRegistry(), &fakeSearcher{}, nil); err == nil {
		t.Fatal("a router was built with no PostgreSQL fallback")
	}
}
