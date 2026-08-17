package db2

import (
	"context"
	"encoding/binary"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	protocol "github.com/JBailes/aimee/server-go/db3"
)

const (
	db3BusPollInterval = 200 * time.Microsecond
	db3BusMaxPending   = 64
)

type db3WireClient interface {
	Poll() (bus.Event, bool, error)
	Publish(uint32, []byte) error
	RequestFragment(uint32, uint64, []byte, bool) error
	ReplyFragment(uint32, uint64, []byte, bool) error
	Cancel(uint32, uint64) error
	HeartbeatNow()
	EpochChanged() bool
	InlineBudget() uint32
}

type db3BusSearchResult struct {
	response DB3SearchResponse
	err      error
}

type db3BusPending struct {
	principal uint32
	reply     chan db3BusSearchResult
}

type db3BusAssembly struct {
	principal uint32
	body      []byte
}

type db3AppliedEvent struct {
	principal uint32
	applied   protocol.Applied
}

type DB3AppliedObserver func(principal uint32, applied protocol.Applied)
type DB3CapabilitiesObserver func(context.Context, uint32, uint32, uint64,
	protocol.Capabilities) error

type DB3BusObservers struct {
	Capabilities DB3CapabilitiesObserver
	Applied      DB3AppliedObserver
}

// DB3BusRouter owns the DB2-side DB3 attachment. One poller multiplexes
// provider notifications, route-control requests, and concurrent search
// replies so no goroutine races another consumer on the shared inbound ring.
type DB3BusRouter struct {
	client       db3WireClient
	router       *DB3Router
	capabilities DB3CapabilitiesObserver
	applied      DB3AppliedObserver
	appliedCh    chan db3AppliedEvent
	next         atomic.Uint64
	sendMu       sync.Mutex
	mu           sync.Mutex
	pending      map[uint64]db3BusPending
	replies      map[uint64]db3BusAssembly
	routes       map[uint64]db3BusAssembly
	closed       chan struct{}
	closeOnce    sync.Once
	errMu        sync.Mutex
	err          error
	pollDelay    time.Duration
}

func NewDB3BusRouter(ctx context.Context, client *bus.Client, internal DB3InternalSearcher,
	authorize DB3CandidateAuthorizer, applied DB3AppliedObserver) (*DB3Router, *DB3BusRouter, error) {
	if client == nil {
		return nil, nil, ErrDB3RouterConfig
	}
	return newDB3BusRouter(ctx, client, internal, authorize, applied)
}

func NewDB3BusRouterWithObservers(ctx context.Context, client *bus.Client,
	internal DB3InternalSearcher, authorize DB3CandidateAuthorizer,
	observers DB3BusObservers) (*DB3Router, *DB3BusRouter, error) {
	if client == nil {
		return nil, nil, ErrDB3RouterConfig
	}
	return newDB3BusRouterWithObservers(ctx, client, internal, authorize, observers)
}

func newDB3BusRouter(ctx context.Context, client db3WireClient, internal DB3InternalSearcher,
	authorize DB3CandidateAuthorizer, applied DB3AppliedObserver) (*DB3Router, *DB3BusRouter, error) {
	return newDB3BusRouterWithObservers(ctx, client, internal, authorize,
		DB3BusObservers{Applied: applied})
}

func newDB3BusRouterWithObservers(ctx context.Context, client db3WireClient,
	internal DB3InternalSearcher, authorize DB3CandidateAuthorizer,
	observers DB3BusObservers) (*DB3Router, *DB3BusRouter, error) {
	if ctx == nil || client == nil || client.InlineBudget() == 0 {
		return nil, nil, ErrDB3RouterConfig
	}
	endpoint := &DB3BusRouter{
		client: client, capabilities: observers.Capabilities, applied: observers.Applied,
		pending: make(map[uint64]db3BusPending),
		replies: make(map[uint64]db3BusAssembly), routes: make(map[uint64]db3BusAssembly),
		closed: make(chan struct{}), pollDelay: db3BusPollInterval,
	}
	if observers.Applied != nil {
		endpoint.appliedCh = make(chan db3AppliedEvent, db3BusMaxPending)
		go endpoint.observeApplied(ctx)
	}
	router, err := NewDB3Router(internal, endpoint.Search, authorize)
	if err != nil {
		return nil, nil, err
	}
	endpoint.router = router
	go endpoint.poll(ctx)
	return router, endpoint, nil
}

func (b *DB3BusRouter) observeApplied(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case <-b.closed:
			return
		case event := <-b.appliedCh:
			b.applied(event.principal, event.applied)
		}
	}
}

func (b *DB3BusRouter) finish(err error) {
	b.closeOnce.Do(func() {
		b.errMu.Lock()
		b.err = err
		b.errMu.Unlock()
		b.mu.Lock()
		for id, pending := range b.pending {
			delete(b.pending, id)
			pending.reply <- db3BusSearchResult{err: err}
		}
		clear(b.replies)
		clear(b.routes)
		b.mu.Unlock()
		close(b.closed)
	})
}

func (b *DB3BusRouter) Err() error {
	if b == nil {
		return ErrDB3RouterConfig
	}
	b.errMu.Lock()
	defer b.errMu.Unlock()
	return b.err
}

func (b *DB3BusRouter) poll(ctx context.Context) {
	idle := b.pollDelay
	for {
		select {
		case <-b.closed:
			return
		default:
		}
		if err := ctx.Err(); err != nil {
			b.finish(err)
			return
		}
		if b.client.EpochChanged() {
			b.finish(bus.ErrEpoch)
			return
		}
		b.client.HeartbeatNow()
		event, ok, err := b.client.Poll()
		if err != nil {
			b.finish(err)
			return
		}
		if !ok {
			time.Sleep(idle)
			if idle < 10*time.Millisecond {
				idle *= 2
			}
			continue
		}
		idle = b.pollDelay
		b.handleEvent(ctx, event)
	}
}

func (b *DB3BusRouter) handleEvent(ctx context.Context, event bus.Event) {
	switch event.Frame.EventKind {
	case bus.KindCapabilityAbsent, bus.KindError:
		b.deliver(event.Frame.CorrelationID, db3BusSearchResult{err: ErrDB3Unavailable})
	case protocol.EventCapabilities:
		if event.Frame.HdrFlags&bus.FNotification == 0 {
			return
		}
		capabilities, err := protocol.DecodeCapabilities(event.Payload)
		if err == nil {
			principal, handle := event.Frame.PrincipalRef, event.Frame.SrcHandle
			if b.capabilities != nil &&
				capabilities.Operations&protocol.OperationApply != 0 {
				if err := b.capabilities(ctx, principal, handle, event.Frame.Seq,
					capabilities); err != nil {
					return
				}
			}
			_ = b.router.ObserveCapabilities(principal, handle, event.Frame.Seq, capabilities)
		}
	case protocol.EventApplied:
		if event.Frame.HdrFlags&bus.FNotification == 0 {
			return
		}
		applied, err := protocol.DecodeApplied(event.Payload)
		if err == nil && b.applied != nil {
			select {
			case b.appliedCh <- db3AppliedEvent{principal: event.Frame.PrincipalRef, applied: applied}:
			case <-ctx.Done():
			}
		}
	case protocol.EventSearch:
		if event.Frame.HdrFlags&bus.FReply != 0 {
			b.handleSearchReply(event)
		}
	case protocol.EventRoute:
		if event.Frame.HdrFlags&bus.FRequest != 0 {
			b.handleRouteRequest(ctx, event)
		}
	}
}

func (b *DB3BusRouter) handleSearchReply(event bus.Event) {
	id := event.Frame.CorrelationID
	b.mu.Lock()
	pending, exists := b.pending[id]
	if !exists {
		b.mu.Unlock()
		return
	}
	assembly := b.replies[id]
	if event.Frame.PrincipalRef != pending.principal ||
		(assembly.principal != 0 && assembly.principal != event.Frame.PrincipalRef) ||
		uint64(len(assembly.body))+uint64(len(event.Payload)) > uint64(bus.MaxPayload) {
		b.mu.Unlock()
		b.deliver(id, db3BusSearchResult{err: ErrDB3InvalidResponse})
		return
	}
	assembly.principal = event.Frame.PrincipalRef
	assembly.body = append(assembly.body, event.Payload...)
	if event.Frame.HdrFlags&bus.FMore != 0 {
		b.replies[id] = assembly
		b.mu.Unlock()
		return
	}
	delete(b.replies, id)
	b.mu.Unlock()

	if reply, err := protocol.DecodeSearchReply(assembly.body); err == nil {
		b.deliver(id, db3BusSearchResult{response: DB3SearchResponse{Reply: &reply}})
		return
	}
	if failure, err := protocol.DecodeSearchFailure(assembly.body); err == nil {
		b.deliver(id, db3BusSearchResult{response: DB3SearchResponse{Failure: &failure}})
		return
	}
	b.deliver(id, db3BusSearchResult{err: ErrDB3InvalidResponse})
}

func (b *DB3BusRouter) handleRouteRequest(ctx context.Context, event bus.Event) {
	id := event.Frame.CorrelationID
	b.mu.Lock()
	assembly := b.routes[id]
	if assembly.principal != 0 && assembly.principal != event.Frame.PrincipalRef {
		delete(b.routes, id)
		b.mu.Unlock()
		return
	}
	if uint64(len(assembly.body))+uint64(len(event.Payload)) > uint64(bus.MaxPayload) {
		delete(b.routes, id)
		b.mu.Unlock()
		return
	}
	assembly.principal = event.Frame.PrincipalRef
	assembly.body = append(assembly.body, event.Payload...)
	if event.Frame.HdrFlags&bus.FMore != 0 {
		if _, exists := b.routes[id]; !exists && len(b.routes) >= db3BusMaxPending {
			b.mu.Unlock()
			return
		}
		b.routes[id] = assembly
		b.mu.Unlock()
		return
	}
	delete(b.routes, id)
	b.mu.Unlock()

	request, err := protocol.DecodeRouteRequest(assembly.body)
	var reply protocol.RouteReply
	if err == nil {
		reply = b.router.Route(request)
	} else if len(assembly.body) >= 16 {
		requestID := binary.LittleEndian.Uint64(assembly.body[8:16])
		if requestID == 0 {
			return
		}
		reply = b.router.Route(protocol.RouteRequest{RequestID: requestID, Action: protocol.RouteQuery})
		reply.Result = protocol.RouteInvalid
	} else {
		return
	}
	wire, err := protocol.EncodeRouteReply(reply)
	if err != nil {
		return
	}
	b.sendMu.Lock()
	err = b.sendFragments(ctx, true, protocol.EventRoute, id, wire)
	b.sendMu.Unlock()
	if err != nil && !errors.Is(err, context.Canceled) {
		b.finish(err)
	}
}

func (b *DB3BusRouter) deliver(id uint64, result db3BusSearchResult) {
	b.mu.Lock()
	pending, exists := b.pending[id]
	delete(b.pending, id)
	delete(b.replies, id)
	b.mu.Unlock()
	if exists {
		pending.reply <- result
	}
}

func (b *DB3BusRouter) sendFragments(ctx context.Context, reply bool, kind uint32,
	correlation uint64, payload []byte) error {
	budget := int(b.client.InlineBudget())
	if budget > int(bus.MaxPayload) {
		budget = int(bus.MaxPayload)
	}
	if budget <= 0 {
		return ErrDB3RouterConfig
	}
	first := true
	for offset := 0; first || offset < len(payload); {
		first = false
		part := len(payload) - offset
		if part > budget {
			part = budget
		}
		more := offset+part < len(payload)
		var err error
		for {
			if reply {
				err = b.client.ReplyFragment(kind, correlation, payload[offset:offset+part], more)
			} else {
				err = b.client.RequestFragment(kind, correlation, payload[offset:offset+part], more)
			}
			if !errors.Is(err, bus.ErrWouldBlock) {
				break
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-b.closed:
				return ErrDB3Unavailable
			case <-time.After(b.pollDelay):
			}
		}
		if err != nil {
			return err
		}
		offset += part
	}
	return nil
}

func (b *DB3BusRouter) Search(ctx context.Context, principal uint32,
	request protocol.SearchRequest) (DB3SearchResponse, error) {
	if b == nil || principal == 0 || request.Validate() != nil {
		return DB3SearchResponse{}, ErrDB3RouterConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	wire, err := protocol.EncodeSearchRequest(request)
	if err != nil {
		return DB3SearchResponse{}, err
	}
	id := b.next.Add(1)
	result := make(chan db3BusSearchResult, 1)
	b.mu.Lock()
	select {
	case <-b.closed:
		b.mu.Unlock()
		return DB3SearchResponse{}, ErrDB3Unavailable
	default:
	}
	if len(b.pending) >= db3BusMaxPending {
		b.mu.Unlock()
		return DB3SearchResponse{}, ErrDB3Unavailable
	}
	b.pending[id] = db3BusPending{principal: principal, reply: result}
	b.mu.Unlock()

	b.sendMu.Lock()
	err = b.sendFragments(ctx, false, protocol.EventSearch, id, wire)
	b.sendMu.Unlock()
	if err != nil {
		b.deliver(id, db3BusSearchResult{err: err})
	}
	select {
	case got := <-result:
		return got.response, got.err
	case <-ctx.Done():
		b.mu.Lock()
		delete(b.pending, id)
		delete(b.replies, id)
		b.mu.Unlock()
		b.sendMu.Lock()
		_ = b.client.Cancel(protocol.EventSearch, id)
		b.sendMu.Unlock()
		return DB3SearchResponse{}, ctx.Err()
	case <-b.closed:
		return DB3SearchResponse{}, ErrDB3Unavailable
	}
}

func (b *DB3BusRouter) publish(ctx context.Context, payload []byte) error {
	for {
		err := b.client.Publish(protocol.EventApply, payload)
		if !errors.Is(err, bus.ErrWouldBlock) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-b.closed:
			return ErrDB3Unavailable
		case <-time.After(b.pollDelay):
		}
	}
}

// PublishApply fans one committed DB2 operation out to every admitted provider.
// Small operations retain the v1 direct frame. Larger operations use the DB3
// notification chunk envelope because F_MORE is intentionally request/reply-only.
func (b *DB3BusRouter) PublishApply(ctx context.Context, apply protocol.Apply) error {
	if b == nil {
		return ErrDB3RouterConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	wire, err := protocol.EncodeApply(apply)
	if err != nil {
		return err
	}
	budget := int(b.client.InlineBudget())
	if budget > int(bus.MaxPayload) {
		budget = int(bus.MaxPayload)
	}
	if budget <= 0 {
		return ErrDB3RouterConfig
	}
	b.sendMu.Lock()
	defer b.sendMu.Unlock()
	if len(wire) <= budget {
		return b.publish(ctx, wire)
	}
	capacity := budget - protocol.ApplyChunkHeader
	if capacity <= 0 {
		return bus.ErrPayload
	}
	for offset := 0; offset < len(wire); offset += capacity {
		end := offset + capacity
		if end > len(wire) {
			end = len(wire)
		}
		chunk, err := protocol.EncodeApplyChunk(protocol.ApplyChunk{
			OperationID: apply.OperationID, Total: uint32(len(wire)), Offset: uint32(offset),
			Data: wire[offset:end],
		})
		if err != nil {
			return err
		}
		if err := b.publish(ctx, chunk); err != nil {
			return err
		}
	}
	return nil
}
