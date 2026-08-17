package db3

import (
	"context"
	"errors"
	"reflect"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

type providerFakeSend struct {
	kind        uint32
	correlation uint64
	payload     []byte
	more        bool
}

type providerFakeBus struct {
	budget    uint32
	events    chan bus.Event
	publishes chan providerFakeSend
	replies   chan providerFakeSend
	mu        sync.Mutex
	heartbeat int
}

func newProviderFakeBus(budget uint32) *providerFakeBus {
	return &providerFakeBus{
		budget: budget, events: make(chan bus.Event, 32), publishes: make(chan providerFakeSend, 32),
		replies: make(chan providerFakeSend, 32),
	}
}

func (f *providerFakeBus) Poll() (bus.Event, bool, error) {
	select {
	case event := <-f.events:
		return event, true, nil
	default:
		return bus.Event{}, false, nil
	}
}

func (f *providerFakeBus) Publish(kind uint32, payload []byte) error {
	f.publishes <- providerFakeSend{kind: kind, payload: append([]byte(nil), payload...)}
	return nil
}

func (f *providerFakeBus) ReplyFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	f.replies <- providerFakeSend{kind: kind, correlation: correlation,
		payload: append([]byte(nil), payload...), more: more}
	return nil
}

func (f *providerFakeBus) HeartbeatNow() {
	f.mu.Lock()
	f.heartbeat++
	f.mu.Unlock()
}
func (f *providerFakeBus) EpochChanged() bool   { return false }
func (f *providerFakeBus) InlineBudget() uint32 { return f.budget }

func providerTestCapabilities() Capabilities {
	return Capabilities{
		Generation: 7, Operations: OperationSearch | OperationApply, Metrics: MetricCosine,
		Filters: FilterExact, MaxDimension: MaxDimension, MaxBatch: 64, MaxTopK: MaxTopK, Ready: true,
	}
}

func providerTestRequest() SearchRequest {
	return SearchRequest{RequestID: 77, RequiredGeneration: 7, Project: "project-a",
		RecordType: "memory", TopK: 2, Vector: []float32{.3, .2, .1}}
}

func providerTestApply() Apply {
	return Apply{OperationID: 1001, Generation: 7, PointID: 41, Kind: ApplyUpsert,
		Collection: "memory", Vector: []float32{.1, .2, .3}, Labels: []ExactLabel{
			{Key: "project", Value: "project-a"},
			{Key: "record_type", Value: "memory"},
		}}
}

func TestProviderConfigFailsClosed(t *testing.T) {
	ctx := context.Background()
	client := newProviderFakeBus(64)
	for name, config := range map[string]ProviderConfig{
		"empty": {},
		"search-missing": {Capabilities: Capabilities{Generation: 1, Operations: OperationSearch,
			Metrics: MetricCosine, MaxDimension: 1, MaxTopK: 1, Ready: true}},
		"apply-missing": {Capabilities: Capabilities{Generation: 1, Operations: OperationApply,
			MaxBatch: 1, Ready: true}},
	} {
		t.Run(name, func(t *testing.T) {
			if err := validateProviderConfig(ctx, client, config); !errors.Is(err, ErrProviderConfig) {
				t.Fatalf("error = %v", err)
			}
		})
	}
	if err := validateProviderConfig(ctx, newProviderFakeBus(capabilitiesHeader-1), ProviderConfig{
		Capabilities: providerTestCapabilities(), Search: func(context.Context, SearchRequest) (SearchReply, SearchFailureCode) {
			return SearchReply{}, SearchFailureInternal
		}, Apply: func(context.Context, Apply) ProviderApplyOutcome {
			return ProviderApplyOutcome{}
		},
	}); !errors.Is(err, ErrProviderConfig) {
		t.Fatalf("undersized inline budget error = %v", err)
	}
}

func TestProviderRejectsCapabilityUpdateItCannotServe(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newProviderFakeBus(64)
	updates := make(chan Capabilities, 1)
	config := ProviderConfig{
		Capabilities: Capabilities{Generation: 1, Operations: OperationApply,
			MaxBatch: 1, Ready: true},
		CapabilityUpdates: updates,
		Apply: func(context.Context, Apply) ProviderApplyOutcome {
			return ProviderApplyOutcome{Result: AppliedOK}
		},
	}
	done := make(chan error, 1)
	go func() { done <- runProvider(ctx, client, config) }()
	<-client.publishes
	updates <- Capabilities{Generation: 2, Operations: OperationSearch,
		Metrics: MetricCosine, MaxDimension: 1, MaxTopK: 1, Ready: true}
	select {
	case err := <-done:
		if !errors.Is(err, ErrProviderConfig) {
			t.Fatalf("error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("provider did not reject unsupported update")
	}
}

func TestProviderApplyReassemblyRejectsGapsAndAllowsRestart(t *testing.T) {
	apply := providerTestApply()
	wire, _ := EncodeApply(apply)
	assemblies := make(map[applyAssemblyKey]applyAssembly)
	first, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply.OperationID,
		Total: uint32(len(wire)), Data: wire[:20]})
	second, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply.OperationID,
		Total: uint32(len(wire)), Offset: 20, Data: wire[20:]})
	if _, complete := decodeProviderApply(29, first, assemblies); complete {
		t.Fatal("partial chunk completed")
	}
	gap, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply.OperationID,
		Total: uint32(len(wire)), Offset: 21, Data: wire[21:]})
	if _, complete := decodeProviderApply(29, gap, assemblies); complete || len(assemblies) != 0 {
		t.Fatal("gap was accepted or retained")
	}
	if _, complete := decodeProviderApply(29, first, assemblies); complete {
		t.Fatal("restart first chunk completed")
	}
	got, complete := decodeProviderApply(29, second, assemblies)
	if !complete || got.OperationID != apply.OperationID || len(got.Vector) != len(apply.Vector) {
		t.Fatalf("reassembled = (%+v, %v)", got, complete)
	}
	if direct, complete := decodeProviderApply(29, wire, assemblies); !complete || direct.OperationID != apply.OperationID {
		t.Fatalf("direct apply = (%+v, %v)", direct, complete)
	}
}

func TestProviderAssembliesProgressAtCapacity(t *testing.T) {
	searches := make(map[uint64]providerAssembly, maxProviderAssemblies)
	for id := uint64(1); id <= maxProviderAssemblies; id++ {
		searches[id] = providerAssembly{principal: 29, body: []byte{byte(id)}}
	}
	handleProviderSearch(context.Background(), bus.Event{Frame: bus.Frame{
		HdrFlags: bus.FRequest | bus.FMore, EventKind: EventSearch, CorrelationID: 1,
		PrincipalRef: 29,
	}, Payload: []byte{99}}, providerTestCapabilities(), nil, searches,
		make(map[uint64]context.CancelFunc),
		make(chan providerSearchDone, 1))
	if got := searches[1].body; len(got) != 2 || got[1] != 99 {
		t.Fatalf("existing search did not progress at capacity: %v", got)
	}
	handleProviderSearch(context.Background(), bus.Event{Frame: bus.Frame{
		HdrFlags: bus.FRequest | bus.FMore, EventKind: EventSearch,
		CorrelationID: maxProviderAssemblies + 1, PrincipalRef: 29,
	}, Payload: []byte{1}}, providerTestCapabilities(), nil, searches,
		make(map[uint64]context.CancelFunc),
		make(chan providerSearchDone, 1))
	if len(searches) != maxProviderAssemblies {
		t.Fatalf("new search exceeded capacity: %d", len(searches))
	}

	apply := providerTestApply()
	wire, _ := EncodeApply(apply)
	applyChunks := make(map[applyAssemblyKey]applyAssembly, maxProviderAssemblies)
	key := applyAssemblyKey{principal: 29, operation: apply.OperationID}
	for operation := uint64(1); operation <= maxProviderAssemblies; operation++ {
		applyChunks[applyAssemblyKey{principal: 29, operation: operation}] = applyAssembly{
			total: 2, next: 1, body: []byte{1},
		}
	}
	delete(applyChunks, applyAssemblyKey{principal: 29, operation: 1})
	applyChunks[key] = applyAssembly{total: 2, next: 1, body: []byte{1}}
	first, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply.OperationID,
		Total: uint32(len(wire)), Data: wire[:20]})
	second, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply.OperationID,
		Total: uint32(len(wire)), Offset: 20, Data: wire[20:]})
	if _, complete := decodeProviderApply(29, first, applyChunks); complete {
		t.Fatal("restart completed on first chunk")
	}
	if got, complete := decodeProviderApply(29, second, applyChunks); !complete || got.OperationID != apply.OperationID {
		t.Fatalf("existing apply did not progress at capacity: (%+v, %v)", got, complete)
	}
}

func TestProviderServesFragmentedSearchAndChunkedApply(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newProviderFakeBus(capabilitiesHeader)
	updates := make(chan Capabilities, 1)
	var applyCalls int
	var searchCalls int
	var applyMu sync.Mutex
	var searchMu sync.Mutex
	config := ProviderConfig{
		Capabilities: providerTestCapabilities(), CapabilityUpdates: updates,
		Search: func(_ context.Context, request SearchRequest) (SearchReply, SearchFailureCode) {
			searchMu.Lock()
			searchCalls++
			searchMu.Unlock()
			return SearchReply{RequestID: request.RequestID, Generation: request.RequiredGeneration,
				Candidates: []Candidate{{PointID: 41, Score: .9}, {PointID: 42, Score: .8}}}, 0
		},
		Apply: func(_ context.Context, apply Apply) ProviderApplyOutcome {
			if !reflect.DeepEqual(apply.Labels, providerTestApply().Labels) {
				t.Errorf("apply labels = %+v", apply.Labels)
			}
			applyMu.Lock()
			applyCalls++
			applyMu.Unlock()
			return ProviderApplyOutcome{Result: AppliedOK, Watermark: apply.OperationID}
		},
	}
	done := make(chan error, 1)
	go func() { done <- runProvider(ctx, client, config) }()
	initial := <-client.publishes
	if initial.kind != EventCapabilities {
		t.Fatalf("initial event = %#x", initial.kind)
	}
	if got, err := DecodeCapabilities(initial.payload); err != nil || !got.Ready {
		t.Fatalf("initial capabilities = (%+v, %v)", got, err)
	}

	requestWire, _ := EncodeSearchRequest(providerTestRequest())
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FRequest | bus.FMore,
		EventKind: EventSearch, CorrelationID: 9, PrincipalRef: 29}, Payload: requestWire[:25]}
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FRequest,
		EventKind: EventSearch, CorrelationID: 9, PrincipalRef: 29}, Payload: requestWire[25:]}

	applyWire, _ := EncodeApply(providerTestApply())
	for offset := 0; offset < len(applyWire); offset += 10 {
		end := offset + 10
		if end > len(applyWire) {
			end = len(applyWire)
		}
		chunk, _ := EncodeApplyChunk(ApplyChunk{OperationID: 1001, Total: uint32(len(applyWire)),
			Offset: uint32(offset), Data: applyWire[offset:end]})
		client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
			EventKind: EventApply, PrincipalRef: 29}, Payload: chunk}
	}

	var replyWire []byte
	for {
		part := <-client.replies
		if part.kind != EventSearch || part.correlation != 9 {
			t.Fatalf("reply metadata = %+v", part)
		}
		replyWire = append(replyWire, part.payload...)
		if !part.more {
			break
		}
	}
	reply, err := DecodeSearchReply(replyWire)
	if err != nil || len(reply.Candidates) != 2 || reply.Candidates[0].PointID != 41 {
		t.Fatalf("search reply = (%+v, %v)", reply, err)
	}

	var applied Applied
	deadline := time.After(2 * time.Second)
	for applied.OperationID == 0 {
		select {
		case event := <-client.publishes:
			if event.kind == EventApplied {
				applied, err = DecodeApplied(event.payload)
				if err != nil {
					t.Fatal(err)
				}
			}
		case <-deadline:
			t.Fatal("timed out waiting for applied acknowledgement")
		}
	}
	if applied.OperationID != 1001 || applied.Watermark != 1001 {
		t.Fatalf("applied = %+v", applied)
	}
	applyMu.Lock()
	if applyCalls != 1 {
		t.Fatalf("apply calls = %d", applyCalls)
	}
	applyMu.Unlock()

	apply2 := providerTestApply()
	apply2.OperationID = 2002
	apply2Wire, _ := EncodeApply(apply2)
	first, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply2.OperationID,
		Total: uint32(len(apply2Wire)), Data: apply2Wire[:10]})
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
		EventKind: EventApply, PrincipalRef: 29}, Payload: first}
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification | bus.FControl,
		EventKind: bus.KindOverflow}, Payload: []byte{1}}
	for offset := 10; offset < len(apply2Wire); offset += 10 {
		end := offset + 10
		if end > len(apply2Wire) {
			end = len(apply2Wire)
		}
		chunk, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply2.OperationID,
			Total: uint32(len(apply2Wire)), Offset: uint32(offset), Data: apply2Wire[offset:end]})
		client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
			EventKind: EventApply, PrincipalRef: 29}, Payload: chunk}
	}
	time.Sleep(20 * time.Millisecond)
	applyMu.Lock()
	if applyCalls != 1 {
		t.Fatalf("overflowed partial apply calls = %d", applyCalls)
	}
	applyMu.Unlock()
	for offset := 0; offset < len(apply2Wire); offset += 10 {
		end := offset + 10
		if end > len(apply2Wire) {
			end = len(apply2Wire)
		}
		chunk, _ := EncodeApplyChunk(ApplyChunk{OperationID: apply2.OperationID,
			Total: uint32(len(apply2Wire)), Offset: uint32(offset), Data: apply2Wire[offset:end]})
		client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FNotification,
			EventKind: EventApply, PrincipalRef: 29}, Payload: chunk}
	}
	for applied.OperationID != apply2.OperationID {
		event := <-client.publishes
		if event.kind == EventApplied {
			applied, err = DecodeApplied(event.payload)
			if err != nil {
				t.Fatal(err)
			}
		}
	}
	applyMu.Lock()
	if applyCalls != 2 {
		t.Fatalf("retried apply calls = %d", applyCalls)
	}
	applyMu.Unlock()

	update := providerTestCapabilities()
	update.Generation = 8
	updates <- update
	for {
		event := <-client.publishes
		if event.kind != EventCapabilities {
			continue
		}
		got, decodeErr := DecodeCapabilities(event.payload)
		if decodeErr != nil || got.Generation != 8 {
			t.Fatalf("updated capabilities = (%+v, %v)", got, decodeErr)
		}
		break
	}
	staleRequest := providerTestRequest()
	staleWire, _ := EncodeSearchRequest(staleRequest)
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FRequest, EventKind: EventSearch,
		CorrelationID: 10, PrincipalRef: 29}, Payload: staleWire}
	staleReply := <-client.replies
	failure, decodeErr := DecodeSearchFailure(staleReply.payload)
	if decodeErr != nil || failure.RequestID != staleRequest.RequestID ||
		failure.Code != SearchFailureUnavailable {
		t.Fatalf("stale-generation response = (%+v, %v)", failure, decodeErr)
	}
	searchMu.Lock()
	if searchCalls != 1 {
		t.Fatalf("stale generation reached handler: %d calls", searchCalls)
	}
	searchMu.Unlock()
	cancel()
	if err := <-done; err != nil {
		t.Fatalf("provider exit = %v", err)
	}
}

func TestProviderCancellationStopsSearchWithoutReply(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newProviderFakeBus(256)
	entered, cancelled := make(chan struct{}), make(chan struct{})
	capabilities := providerTestCapabilities()
	capabilities.Operations = OperationSearch
	capabilities.MaxBatch = 0
	config := ProviderConfig{Capabilities: capabilities,
		Search: func(ctx context.Context, _ SearchRequest) (SearchReply, SearchFailureCode) {
			close(entered)
			<-ctx.Done()
			close(cancelled)
			return SearchReply{}, SearchFailureInternal
		}}
	done := make(chan error, 1)
	go func() { done <- runProvider(ctx, client, config) }()
	<-client.publishes
	wire, _ := EncodeSearchRequest(providerTestRequest())
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FRequest, EventKind: EventSearch,
		CorrelationID: 11, PrincipalRef: 29}, Payload: wire}
	<-entered
	client.events <- bus.Event{Frame: bus.Frame{HdrFlags: bus.FCancel, EventKind: EventSearch,
		CorrelationID: 11, PrincipalRef: 29}}
	select {
	case <-cancelled:
	case <-time.After(2 * time.Second):
		t.Fatal("provider search was not cancelled")
	}
	select {
	case reply := <-client.replies:
		t.Fatalf("cancelled search replied: %+v", reply)
	case <-time.After(20 * time.Millisecond):
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
