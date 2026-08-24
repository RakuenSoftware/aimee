package db2

import (
	"context"
	"errors"
	"math"
	"sync"
	"sync/atomic"
	"testing"

	protocol "github.com/JBailes/aimee/server-go/db3"
)

func db3Request() protocol.SearchRequest {
	return protocol.SearchRequest{
		RequestID: 77, RequiredGeneration: 7, Workspace: "workspace-a", Project: "project-a",
		RecordType: "memory", TopK: 3, Vector: []float32{0.3, 0.2, 0.1},
	}
}

func db3Reply(request protocol.SearchRequest, first int64) protocol.SearchReply {
	reply := protocol.SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
		Candidates: []protocol.Candidate{{PointID: first, Score: .9}, {PointID: first + 1, Score: .8}, {PointID: first + 2, Score: .7}},
	}
	if request.TopK < uint32(len(reply.Candidates)) {
		reply.Candidates = reply.Candidates[:request.TopK]
	}
	return reply
}

func db3Capabilities(generation uint64, ready bool) protocol.Capabilities {
	return protocol.Capabilities{
		Generation: generation, Operations: protocol.OperationSearch | protocol.OperationApply,
		Metrics: protocol.MetricCosine, Filters: protocol.FilterExact,
		MaxDimension: protocol.MaxDimension, MaxBatch: 64, MaxTopK: protocol.MaxTopK, Ready: ready,
	}
}

func newTestDB3Router(t *testing.T, internal DB3InternalSearcher,
	external DB3ExternalSearcher, authorize DB3CandidateAuthorizer) *DB3Router {
	t.Helper()
	router, err := NewDB3Router(internal, external, authorize)
	if err != nil {
		t.Fatal(err)
	}
	return router
}

func allowAll(context.Context, string, string, int64) (bool, error) { return true, nil }

func TestDB3RouterDeploymentMakesExternalTheDefaultAndRevalidates(t *testing.T) {
	var internalCalls, externalCalls atomic.Int32
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			internalCalls.Add(1)
			return db3Reply(request, 10), nil
		},
		func(_ context.Context, principal uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			externalCalls.Add(1)
			if principal != 457 {
				t.Fatalf("principal = %d", principal)
			}
			reply := db3Reply(request, 20)
			return DB3SearchResponse{Reply: &reply}, nil
		},
		func(_ context.Context, workspace, project string, pointID int64) (bool, error) {
			if workspace != "workspace-a" || project != "project-a" {
				t.Fatalf("scope = %q/%q", workspace, project)
			}
			return pointID != 11 && pointID != 21, nil
		})

	outcome := router.Search(context.Background(), db3Request())
	if outcome.Result != DB3OK || outcome.Route != DB3DefaultPGVector ||
		outcome.SelectedPrincipal != 0 || len(outcome.Reply.Candidates) != 2 ||
		outcome.Reply.Candidates[0].PointID != 10 || outcome.Reply.Candidates[1].PointID != 12 {
		t.Fatalf("default outcome = %+v", outcome)
	}
	if err := router.ObserveCapabilities(457, 41, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	reply := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteQuery})
	if reply.Result != protocol.RouteOK || reply.SelectedPrincipal != 457 ||
		reply.ProviderGeneration != 7 {
		t.Fatalf("automatic route reply = %+v", reply)
	}
	outcome = router.Search(context.Background(), db3Request())
	if outcome.Result != DB3OK || outcome.Route != DB3External ||
		outcome.SelectedPrincipal != 457 || len(outcome.Reply.Candidates) != 2 ||
		outcome.Reply.Candidates[0].PointID != 20 || outcome.Reply.Candidates[1].PointID != 22 {
		t.Fatalf("external outcome = %+v", outcome)
	}
	if internalCalls.Load() != 1 || externalCalls.Load() != 1 {
		t.Fatalf("calls = internal %d external %d", internalCalls.Load(), externalCalls.Load())
	}
	router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteClear})
	if got := router.Search(context.Background(), db3Request()); got.Route != DB3External ||
		got.SelectedPrincipal != 457 {
		t.Fatalf("cleared route = %+v", got)
	}
}

func TestDB3RouterAutomaticDefaultIsDeterministicAcrossMultipleProviders(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, principal uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, int64(principal))
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)

	// Arrival order is intentionally the reverse of principal order.
	if err := router.ObserveCapabilities(460, 52, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	if err := router.ObserveCapabilities(457, 51, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	query := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteQuery})
	if query.SelectedPrincipal != 457 || query.ProviderGeneration != 7 || query.Fallback {
		t.Fatalf("deterministic deployed default = %+v", query)
	}
	if got := router.Search(context.Background(), db3Request()); got.Result != DB3OK ||
		got.SelectedPrincipal != 457 || got.Reply.Candidates[0].PointID != 457 {
		t.Fatalf("automatic external search = %+v", got)
	}

	// Losing the automatic default advances to the next deployed provider.
	if !router.RemoveProvider(457, 51) {
		t.Fatal("automatic provider was not removed")
	}
	query = router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteQuery})
	if query.SelectedPrincipal != 460 || query.ProviderGeneration != 7 {
		t.Fatalf("automatic failover route = %+v", query)
	}
	if got := router.Search(context.Background(), db3Request()); got.Result != DB3OK ||
		got.SelectedPrincipal != 460 || got.Reply.Candidates[0].PointID != 460 {
		t.Fatalf("automatic failover search = %+v", got)
	}

	// Unready evidence remains an observer registration, never a read route.
	if err := router.ObserveCapabilities(462, 53, 1, db3Capabilities(7, false)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 3, Action: protocol.RouteQuery}); got.SelectedPrincipal != 460 {
		t.Fatalf("unready provider displaced default = %+v", got)
	}
	if !router.RemoveProvider(460, 52) {
		t.Fatal("last ready provider was not removed")
	}
	if got := router.Search(context.Background(), db3Request()); got.Result != DB3OK ||
		got.Route != DB3DefaultPGVector || got.SelectedPrincipal != 0 ||
		got.Reply.Candidates[0].PointID != 1 {
		t.Fatalf("no ready deployed provider did not restore pgvector = %+v", got)
	}
	if err := router.ObserveCapabilities(462, 53, 2, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 4, Action: protocol.RouteQuery}); got.SelectedPrincipal != 462 || got.ProviderGeneration != 7 {
		t.Fatalf("provider readiness did not restore external default = %+v", got)
	}
}

func TestDB3RouterExplicitOverrideWinsUntilClear(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, principal uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, int64(principal))
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	for _, principal := range []uint32{457, 460} {
		if err := router.ObserveCapabilities(principal, principal, 1, db3Capabilities(7, true)); err != nil {
			t.Fatal(err)
		}
	}
	selected := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 460, CapabilityGeneration: 7, Fallback: true})
	if selected.Result != protocol.RouteOK || selected.SelectedPrincipal != 460 || !selected.Fallback {
		t.Fatalf("explicit override = %+v", selected)
	}
	// A newly admitted lower principal cannot rewrite an explicit choice.
	if err := router.ObserveCapabilities(456, 456, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteQuery}); got.SelectedPrincipal != 460 || !got.Fallback {
		t.Fatalf("admission rewrote explicit override = %+v", got)
	}
	cleared := router.Route(protocol.RouteRequest{RequestID: 3, Action: protocol.RouteClear})
	if cleared.SelectedPrincipal != 456 || cleared.ProviderGeneration != 7 || cleared.Fallback {
		t.Fatalf("clear did not restore deployed default = %+v", cleared)
	}
}

func TestDB3RouterSelectionRequiresFreshReadyCompatibleEvidence(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, 1)
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	selectRequest := protocol.RouteRequest{
		RequestID: 1, Action: protocol.RouteSelect, Principal: 457, CapabilityGeneration: 7,
	}
	if got := router.Route(selectRequest); got.Result != protocol.RouteNotFound {
		t.Fatalf("missing provider = %+v", got)
	}
	if err := router.ObserveCapabilities(457, 41, 4, db3Capabilities(7, false)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(selectRequest); got.Result != protocol.RouteNotReady {
		t.Fatalf("unready provider = %+v", got)
	}
	if err := router.ObserveCapabilities(457, 41, 4, db3Capabilities(7, true)); !errors.Is(err, ErrDB3StaleCapability) {
		t.Fatalf("duplicate evidence error = %v", err)
	}
	if err := router.ObserveCapabilities(457, 41, 5, db3Capabilities(8, true)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(selectRequest); got.Result != protocol.RouteGenerationConflict {
		t.Fatalf("stale generation = %+v", got)
	}
	selectRequest.CapabilityGeneration = 8
	if got := router.Route(selectRequest); got.Result != protocol.RouteOK {
		t.Fatalf("fresh generation = %+v", got)
	}
	// A new authenticated attachment may restart its bus sequence.
	if err := router.ObserveCapabilities(457, 42, 1, db3Capabilities(8, true)); err != nil {
		t.Fatalf("new attachment: %v", err)
	}
	if router.RemoveProvider(457, 41) {
		t.Fatal("stale handle removed the replacement provider")
	}
	if !router.RemoveProvider(457, 42) {
		t.Fatal("current handle was not removed")
	}
	query := router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteQuery})
	if query.SelectedPrincipal != 457 || query.ProviderGeneration != 8 {
		t.Fatalf("removal silently cleared selection: %+v", query)
	}
	if got := router.Search(context.Background(), db3Request()); got.Result != DB3Unavailable || got.Route != DB3External {
		t.Fatalf("removed selected provider = %+v", got)
	}
}

func TestDB3RouterAcceptsAuthenticatedSlotZero(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, 1)
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	if err := router.ObserveCapabilities(457, 0, 1, db3Capabilities(7, true)); err != nil {
		t.Fatalf("slot zero evidence: %v", err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 7}); got.Result != protocol.RouteOK {
		t.Fatalf("slot zero selection = %+v", got)
	}
	if !router.RemoveProvider(457, 0) {
		t.Fatal("slot zero provider was not removed")
	}
}

func TestDB3RouterBindsSelectionGenerationAndRequiresExactFiltering(t *testing.T) {
	var externalCalls atomic.Int32
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			externalCalls.Add(1)
			reply := db3Reply(request, 1)
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	if err := router.ObserveCapabilities(457, 41, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 7}); got.Result != protocol.RouteOK {
		t.Fatalf("initial selection = %+v", got)
	}
	if err := router.ObserveCapabilities(457, 41, 2, db3Capabilities(8, true)); err != nil {
		t.Fatal(err)
	}
	query := router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteQuery})
	if query.ProviderGeneration != 7 {
		t.Fatalf("capability update rewrote selected generation: %+v", query)
	}
	for _, generation := range []uint64{7, 8} {
		request := db3Request()
		request.RequiredGeneration = generation
		if got := router.Search(context.Background(), request); got.Result != DB3Unavailable {
			t.Fatalf("generation %d used stale selection: %+v", generation, got)
		}
	}
	if externalCalls.Load() != 0 {
		t.Fatalf("stale selection reached provider: %d", externalCalls.Load())
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 3, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 8}); got.Result != protocol.RouteOK {
		t.Fatalf("refreshed selection = %+v", got)
	}
	request := db3Request()
	request.RequiredGeneration = 8
	if got := router.Search(context.Background(), request); got.Result != DB3OK {
		t.Fatalf("refreshed search = %+v", got)
	}

	capabilities := db3Capabilities(8, true)
	capabilities.Filters = 0
	if err := router.ObserveCapabilities(458, 42, 1, capabilities); err != nil {
		t.Fatal(err)
	}
	if got := router.Route(protocol.RouteRequest{RequestID: 4, Action: protocol.RouteSelect,
		Principal: 458, CapabilityGeneration: 8}); got.Result != protocol.RouteNotReady {
		t.Fatalf("provider without exact filtering = %+v", got)
	}
}

func TestDB3RouterFailClosedAndExplicitFallback(t *testing.T) {
	tests := []struct {
		name        string
		external    DB3ExternalSearcher
		want        DB3Result
		wantFailure protocol.SearchFailureCode
	}{
		{"transport-unavailable", func(context.Context, uint32, protocol.SearchRequest) (DB3SearchResponse, error) {
			return DB3SearchResponse{}, ErrDB3Unavailable
		}, DB3Unavailable, 0},
		{"transport-failure", func(context.Context, uint32, protocol.SearchRequest) (DB3SearchResponse, error) {
			return DB3SearchResponse{}, errors.New("provider failed")
		}, DB3ProviderFailure, 0},
		{"typed-unavailable", func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			failure := protocol.SearchFailure{RequestID: request.RequestID, Code: protocol.SearchFailureUnavailable}
			return DB3SearchResponse{Failure: &failure}, nil
		}, DB3Unavailable, protocol.SearchFailureUnavailable},
		{"typed-retryable", func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			failure := protocol.SearchFailure{RequestID: request.RequestID, Code: protocol.SearchFailureRetryable}
			return DB3SearchResponse{Failure: &failure}, nil
		}, DB3ProviderFailure, protocol.SearchFailureRetryable},
		{"malformed-choice", func(context.Context, uint32, protocol.SearchRequest) (DB3SearchResponse, error) {
			return DB3SearchResponse{}, nil
		}, DB3InvalidResponse, 0},
		{"malformed-reply", func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, 1)
			reply.Candidates[0].Score = math.NaN()
			return DB3SearchResponse{Reply: &reply}, nil
		}, DB3InvalidResponse, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var internalCalls atomic.Int32
			router := newTestDB3Router(t,
				func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
					internalCalls.Add(1)
					return db3Reply(request, 50), nil
				}, test.external, allowAll)
			if err := router.ObserveCapabilities(457, 41, 1, db3Capabilities(7, true)); err != nil {
				t.Fatal(err)
			}
			// Ready deployment is already the external default; automatic routes
			// fail closed and never silently opt into pgvector fallback.
			outcome := router.Search(context.Background(), db3Request())
			if outcome.Result != test.want || outcome.Route != DB3External || internalCalls.Load() != 0 ||
				outcome.ExternalError != test.want || outcome.ProviderFailure != test.wantFailure {
				t.Fatalf("fail closed = %+v, internal calls %d", outcome, internalCalls.Load())
			}
			router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteSelect,
				Principal: 457, CapabilityGeneration: 7, Fallback: true})
			outcome = router.Search(context.Background(), db3Request())
			if outcome.Result != DB3OK || outcome.Route != DB3ExplicitFallback ||
				outcome.ExternalError != test.want || outcome.ProviderFailure != test.wantFailure ||
				internalCalls.Load() != 1 || outcome.Reply.Candidates[0].PointID != 50 {
				t.Fatalf("fallback = %+v, internal calls %d", outcome, internalCalls.Load())
			}
		})
	}
}

func TestDB3RouterGenerationLimitsAndAuthorizationErrors(t *testing.T) {
	var externalCalls atomic.Int32
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			externalCalls.Add(1)
			reply := db3Reply(request, 1)
			return DB3SearchResponse{Reply: &reply}, nil
		},
		func(context.Context, string, string, int64) (bool, error) {
			return false, errors.New("db2 lookup failed")
		})
	capabilities := db3Capabilities(8, true)
	capabilities.MaxDimension, capabilities.MaxTopK = 2, 2
	if err := router.ObserveCapabilities(457, 41, 1, capabilities); err != nil {
		t.Fatal(err)
	}
	router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 8, Fallback: false})
	for name, mutate := range map[string]func(*protocol.SearchRequest){
		"generation": func(request *protocol.SearchRequest) { request.RequiredGeneration = 7 },
		"dimension":  func(request *protocol.SearchRequest) { request.RequiredGeneration = 8 },
		"top-k": func(request *protocol.SearchRequest) {
			request.RequiredGeneration, request.Vector = 8, request.Vector[:2]
		},
	} {
		t.Run(name, func(t *testing.T) {
			request := db3Request()
			mutate(&request)
			if got := router.Search(context.Background(), request); got.Result != DB3Unavailable {
				t.Fatalf("outcome = %+v", got)
			}
		})
	}
	if externalCalls.Load() != 0 {
		t.Fatalf("incompatible requests reached provider: %d", externalCalls.Load())
	}
	// A compatible request reaches the provider, then DB2 authorization fails closed.
	request := db3Request()
	request.RequiredGeneration, request.TopK, request.Vector = 8, 2, request.Vector[:2]
	if got := router.Search(context.Background(), request); got.Result != DB3Internal {
		t.Fatalf("authorization failure = %+v", got)
	}
}

func TestDB3RouterDoesNotFallbackAfterCancellation(t *testing.T) {
	var internalCalls atomic.Int32
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			internalCalls.Add(1)
			return db3Reply(request, 1), nil
		},
		func(ctx context.Context, _ uint32, _ protocol.SearchRequest) (DB3SearchResponse, error) {
			<-ctx.Done()
			return DB3SearchResponse{}, ctx.Err()
		}, allowAll)
	if err := router.ObserveCapabilities(457, 41, 1, db3Capabilities(7, true)); err != nil {
		t.Fatal(err)
	}
	router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 7, Fallback: true})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if got := router.Search(ctx, db3Request()); got.Result != DB3Internal || internalCalls.Load() != 0 {
		t.Fatalf("cancelled outcome = %+v, internal calls %d", got, internalCalls.Load())
	}
}

func TestDB3RouterSnapshotsSelectionAndCopiesTrustBoundaries(t *testing.T) {
	entered, release := make(chan struct{}), make(chan struct{})
	providerVector := make(chan []float32, 1)
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, principal uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			providerVector <- request.Vector
			close(entered)
			<-release
			reply := db3Reply(request, int64(principal))
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	for principal, handle := range map[uint32]uint32{457: 41, 458: 42} {
		if err := router.ObserveCapabilities(principal, handle, 1, db3Capabilities(7, true)); err != nil {
			t.Fatal(err)
		}
	}
	router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
		Principal: 457, CapabilityGeneration: 7})
	request := db3Request()
	done := make(chan DB3SearchOutcome, 1)
	go func() { done <- router.Search(context.Background(), request) }()
	<-entered
	request.Vector[0] = 99
	router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteSelect,
		Principal: 458, CapabilityGeneration: 7})
	close(release)
	outcome := <-done
	if outcome.SelectedPrincipal != 457 || outcome.Reply.Candidates[0].PointID != 457 {
		t.Fatalf("in-flight selection changed: %+v", outcome)
	}
	if got := <-providerVector; got[0] != .3 {
		t.Fatalf("provider request aliased caller vector: %v", got)
	}
}

func TestDB3RouterConcurrentEvidenceRouteAndSearch(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, _ uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, 1)
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)
	var wg sync.WaitGroup
	for worker := 0; worker < 8; worker++ {
		worker := worker
		wg.Add(1)
		go func() {
			defer wg.Done()
			principal := uint32(457 + worker%2)
			handle := uint32(41 + worker%2)
			for sequence := uint64(1); sequence <= 100; sequence++ {
				_ = router.ObserveCapabilities(principal, handle, sequence, db3Capabilities(7, true))
				router.Route(protocol.RouteRequest{RequestID: sequence, Action: protocol.RouteSelect,
					Principal: principal, CapabilityGeneration: 7, Fallback: sequence%2 == 0})
				_ = router.Search(context.Background(), db3Request())
			}
		}()
	}
	wg.Wait()
}

// A provider may only attach with a ref from the band reserved for DB3
// providers. Without this the router accepted ANY non-zero principal, so a
// misprovisioned provider could attach as ref 28 and derive postgres's event
// kinds -- and bus_host_serve_kind() binds one kind to exactly one serving slot,
// so the CORE module would be the one denied at attach, with nothing in its own
// log to say why.
//
// The principal is host-authenticated, so this is not defence against a forged
// frame. It is defence against misprovisioning, which is the likely case and the
// one that fails silently.
func TestDB3RouterRefusesOutOfBandProviderPrincipals(t *testing.T) {
	router := newTestDB3Router(t,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		},
		func(_ context.Context, principal uint32, request protocol.SearchRequest) (DB3SearchResponse, error) {
			reply := db3Reply(request, int64(principal))
			return DB3SearchResponse{Reply: &reply}, nil
		}, allowAll)

	// 7, 28, 29, 30 are memory, postgres, db2 and db1. 200 and 455 belong to the
	// plugin band. All must be refused.
	for _, principal := range []uint32{1, 7, 28, 29, 30, 200, 455, 512, 1001} {
		err := router.ObserveCapabilities(principal, 1, 1, db3Capabilities(7, true))
		if err == nil {
			t.Errorf("ObserveCapabilities accepted out-of-band principal %d", principal)
			continue
		}
		if !errors.Is(err, protocol.ErrProviderRef) {
			t.Errorf("principal %d: err = %v, want ErrProviderRef", principal, err)
		}
	}

	// Both boundaries of the band itself are accepted.
	for _, principal := range []uint32{456, 511} {
		if err := router.ObserveCapabilities(principal, 1, 1, db3Capabilities(7, true)); err != nil {
			t.Errorf("ObserveCapabilities refused in-band principal %d: %v", principal, err)
		}
	}
}
