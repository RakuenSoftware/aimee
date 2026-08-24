package db2

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	protocol "github.com/JBailes/aimee/server-go/vector"
)

type db3FakeFrame struct {
	kind        uint32
	correlation uint64
	payload     []byte
	more        bool
}

type db3FakeBus struct {
	budget   uint32
	events   chan bus.Event
	pubs     chan db3FakeFrame
	repls    chan db3FakeFrame
	cancels  chan db3FakeFrame
	mu       sync.Mutex
	requests map[uint64][]byte
	respond  func(uint64, []byte)
}

func newDB3FakeBus(budget uint32) *db3FakeBus {
	return &db3FakeBus{
		budget: budget, events: make(chan bus.Event, 64), pubs: make(chan db3FakeFrame, 64),
		repls: make(chan db3FakeFrame, 64), cancels: make(chan db3FakeFrame, 16),
		requests: make(map[uint64][]byte),
	}
}

func (f *db3FakeBus) Poll() (bus.Event, bool, error) {
	select {
	case event := <-f.events:
		return event, true, nil
	default:
		return bus.Event{}, false, nil
	}
}
func (f *db3FakeBus) Publish(kind uint32, payload []byte) error {
	f.pubs <- db3FakeFrame{kind: kind, payload: append([]byte(nil), payload...)}
	return nil
}
func (f *db3FakeBus) RequestFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	f.mu.Lock()
	f.requests[correlation] = append(f.requests[correlation], payload...)
	body := append([]byte(nil), f.requests[correlation]...)
	respond := f.respond
	if !more {
		delete(f.requests, correlation)
	}
	f.mu.Unlock()
	if !more && respond != nil {
		respond(correlation, body)
	}
	return nil
}
func (f *db3FakeBus) ReplyFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	f.repls <- db3FakeFrame{kind: kind, correlation: correlation,
		payload: append([]byte(nil), payload...), more: more}
	return nil
}
func (f *db3FakeBus) Cancel(kind uint32, correlation uint64) error {
	f.cancels <- db3FakeFrame{kind: kind, correlation: correlation}
	return nil
}
func (f *db3FakeBus) HeartbeatNow()        {}
func (f *db3FakeBus) EpochChanged() bool   { return false }
func (f *db3FakeBus) InlineBudget() uint32 { return f.budget }

func waitDB3(t *testing.T, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if check() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("timed out waiting for DB3 bus state")
}

func enqueueCapabilities(t *testing.T, client *db3FakeBus, principal, handle uint32,
	sequence uint64, capabilities protocol.Capabilities) {
	t.Helper()
	wire, err := protocol.EncodeCapabilities(capabilities)
	if err != nil {
		t.Fatal(err)
	}
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
		EventKind: protocol.EventCapabilities, PrincipalRef: principal, SrcHandle: handle,
		Seq: sequence}, Payload: wire}
}

func TestVectorBusRouterSearchRouteAndAuthenticatedEvidence(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newDB3FakeBus(31) // force request and reply fragmentation
	client.respond = func(correlation uint64, body []byte) {
		request, err := protocol.DecodeSearchRequest(body)
		if err != nil {
			t.Errorf("decode search request: %v", err)
			return
		}
		reply := db3Reply(request, 40)
		wire, _ := protocol.EncodeSearchReply(reply)
		for offset := 0; offset < len(wire); offset += 17 {
			end := offset + 17
			if end > len(wire) {
				end = len(wire)
			}
			flags := uint16(bus.FReply)
			if end < len(wire) {
				flags |= bus.FMore
			}
			client.events <- bus.Event{Frame: bus.Frame{HdrFlags: flags,
				EventKind: protocol.EventSearch, CorrelationID: correlation, PrincipalRef: 1001,
				SrcHandle: 41}, Payload: wire[offset:end]}
		}
	}
	var observed protocol.Applied
	var observedPrincipal uint32
	var observedMu sync.Mutex
	router, endpoint, err := newVectorBusRouter(ctx, client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 10), nil
		}, allowAll, func(principal uint32, applied protocol.Applied) {
			observedMu.Lock()
			observedPrincipal, observed = principal, applied
			observedMu.Unlock()
		})
	if err != nil {
		t.Fatal(err)
	}
	enqueueCapabilities(t, client, 1001, 41, 1, db3Capabilities(7, true))
	waitDB3(t, func() bool {
		return router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
			Principal: 1001, CapabilityGeneration: 7}).Result == protocol.RouteOK
	})
	outcome := router.Search(context.Background(), db3Request())
	if outcome.Result != DB3OK || outcome.Route != DB3External ||
		outcome.Reply.Candidates[0].PointID != 40 {
		t.Fatalf("search outcome = %+v", outcome)
	}

	appliedWire, _ := protocol.EncodeApplied(protocol.Applied{
		OperationID: 1001, Generation: 7, Watermark: 1001, Result: protocol.AppliedOK,
	})
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
		EventKind: protocol.EventApplied, PrincipalRef: 1001, SrcHandle: 41}, Payload: appliedWire}
	waitDB3(t, func() bool {
		observedMu.Lock()
		defer observedMu.Unlock()
		return observed.OperationID == 1001
	})
	observedMu.Lock()
	if observedPrincipal != 1001 || observed.Watermark != 1001 {
		t.Fatalf("applied observation = principal %d, %+v", observedPrincipal, observed)
	}
	observedMu.Unlock()

	routeWire, _ := protocol.EncodeRouteRequest(protocol.RouteRequest{RequestID: 91,
		Action: protocol.RouteQuery})
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FRequest,
		EventKind: protocol.EventRoute, CorrelationID: 55, PrincipalRef: 500}, Payload: routeWire}
	var replyWire []byte
	for {
		part := <-client.repls
		if part.kind != protocol.EventRoute || part.correlation != 55 {
			t.Fatalf("route reply metadata = %+v", part)
		}
		replyWire = append(replyWire, part.payload...)
		if !part.more {
			break
		}
	}
	routeReply, err := protocol.DecodeRouteReply(replyWire)
	if err != nil || routeReply.SelectedPrincipal != 1001 || routeReply.ProviderGeneration != 7 {
		t.Fatalf("route reply = (%+v, %v)", routeReply, err)
	}
	if endpoint.Err() != nil {
		t.Fatalf("endpoint error = %v", endpoint.Err())
	}
}

func TestVectorBusRouterRejectsWrongProviderAndSupportsExplicitFallback(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newDB3FakeBus(256)
	client.respond = func(correlation uint64, body []byte) {
		request, _ := protocol.DecodeSearchRequest(body)
		reply := db3Reply(request, 20)
		wire, _ := protocol.EncodeSearchReply(reply)
		client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FReply,
			EventKind: protocol.EventSearch, CorrelationID: correlation,
			PrincipalRef: 1002, SrcHandle: 42}, Payload: wire}
	}
	router, _, err := newVectorBusRouter(ctx, client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 50), nil
		}, allowAll, nil)
	if err != nil {
		t.Fatal(err)
	}
	enqueueCapabilities(t, client, 1001, 41, 1, db3Capabilities(7, true))
	waitDB3(t, func() bool {
		return router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
			Principal: 1001, CapabilityGeneration: 7, Fallback: true}).Result == protocol.RouteOK
	})
	outcome := router.Search(context.Background(), db3Request())
	if outcome.Result != DB3OK || outcome.Route != DB3ExplicitFallback ||
		outcome.ExternalError != DB3InvalidResponse || outcome.Reply.Candidates[0].PointID != 50 {
		t.Fatalf("wrong-principal fallback = %+v", outcome)
	}
}

func TestVectorBusRouterPublishesDirectAndChunkedApply(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newDB3FakeBus(64)
	_, endpoint, err := newVectorBusRouter(ctx, client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		}, allowAll, nil)
	if err != nil {
		t.Fatal(err)
	}
	small := protocol.Apply{OperationID: 1, Generation: 1, PointID: 1,
		Kind: protocol.ApplyDelete, Collection: "memory"}
	if err := endpoint.PublishApply(context.Background(), small); err != nil {
		t.Fatal(err)
	}
	direct := <-client.pubs
	if direct.kind != protocol.EventApply {
		t.Fatalf("direct kind = %#x", direct.kind)
	}
	if got, err := protocol.DecodeApply(direct.payload); err != nil || got.OperationID != 1 {
		t.Fatalf("direct apply = (%+v, %v)", got, err)
	}
	large := protocol.Apply{OperationID: 2, Generation: 2, PointID: 2,
		Kind: protocol.ApplyUpsert, Collection: "memory", Vector: make([]float32, 40)}
	if err := endpoint.PublishApply(context.Background(), large); err != nil {
		t.Fatal(err)
	}
	// Read and validate chunks without relying on private framing constants.
	wantWire, _ := protocol.EncodeApply(large)
	var reconstructed []byte
	for len(reconstructed) < len(wantWire) {
		published := <-client.pubs
		chunk, err := protocol.DecodeApplyChunk(published.payload)
		if err != nil || chunk.OperationID != 2 || int(chunk.Offset) != len(reconstructed) ||
			int(chunk.Total) != len(wantWire) {
			t.Fatalf("chunk = (%+v, %v), reconstructed=%d", chunk, err, len(reconstructed))
		}
		reconstructed = append(reconstructed, chunk.Data...)
	}
	if got, err := protocol.DecodeApply(reconstructed); err != nil || got.OperationID != 2 || len(got.Vector) != 40 {
		t.Fatalf("chunked apply = (%+v, %v)", got, err)
	}
}

func TestVectorBusRouterCancelsOutstandingSearch(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newDB3FakeBus(256)
	router, _, err := newVectorBusRouter(ctx, client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		}, allowAll, nil)
	if err != nil {
		t.Fatal(err)
	}
	enqueueCapabilities(t, client, 1001, 41, 1, db3Capabilities(7, true))
	waitDB3(t, func() bool {
		return router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteSelect,
			Principal: 1001, CapabilityGeneration: 7}).Result == protocol.RouteOK
	})
	callCtx, callCancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer callCancel()
	outcome := router.Search(callCtx, db3Request())
	if outcome.Result != DB3Internal {
		t.Fatalf("cancelled search = %+v", outcome)
	}
	select {
	case cancellation := <-client.cancels:
		if cancellation.kind != protocol.EventSearch || cancellation.correlation == 0 {
			t.Fatalf("cancel frame = %+v", cancellation)
		}
	case <-time.After(time.Second):
		t.Fatal("search cancellation was not emitted")
	}
}

func TestVectorBusRouterConstructionFailsClosed(t *testing.T) {
	client := newDB3FakeBus(0)
	_, _, err := newVectorBusRouter(context.Background(), client,
		func(context.Context, protocol.SearchRequest) (protocol.SearchReply, error) {
			return protocol.SearchReply{}, nil
		},
		allowAll, nil)
	if !errors.Is(err, ErrVectorRouterConfig) {
		t.Fatalf("error = %v", err)
	}
}

func TestVectorBusRouterPersistsApplyProviderBeforeRouting(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newDB3FakeBus(128)
	var observed atomic.Int32
	router, _, err := newVectorBusRouterWithObservers(ctx, client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			return db3Reply(request, 1), nil
		}, allowAll, DB3BusObservers{
			Capabilities: func(_ context.Context, principal, handle uint32, sequence uint64,
				capabilities protocol.Capabilities) error {
				if principal != 1001 || handle != 41 || sequence != 1 ||
					capabilities.Generation != 7 {
					t.Fatalf("capability evidence = %d/%d/%d %+v",
						principal, handle, sequence, capabilities)
				}
				observed.Add(1)
				return errors.New("database unavailable")
			},
		})
	if err != nil {
		t.Fatal(err)
	}
	enqueueCapabilities(t, client, 1001, 41, 1, db3Capabilities(7, true))
	waitDB3(t, func() bool { return observed.Load() == 1 })
	if got := router.Route(protocol.RouteRequest{RequestID: 1, Action: protocol.RouteQuery}); got.SelectedPrincipal != 0 {
		t.Fatalf("unpersisted provider became route = %+v", got)
	}

	// Search-only providers do not create apply-delivery obligations.
	searchOnly := db3Capabilities(7, true)
	searchOnly.Operations = protocol.OperationSearch
	searchOnly.MaxBatch = 0
	enqueueCapabilities(t, client, 2002, 42, 1, searchOnly)
	waitDB3(t, func() bool {
		return router.Route(protocol.RouteRequest{RequestID: 2, Action: protocol.RouteQuery}).
			SelectedPrincipal == 2002
	})
	if observed.Load() != 1 {
		t.Fatalf("search-only provider was persisted: %d", observed.Load())
	}
}
