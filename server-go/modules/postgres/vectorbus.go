package postgres

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

// The bus side of vector routing: how a search actually reaches a provider, and
// how this module learns a provider exists at all.
//
// There is no module-to-module dependency here and there never needed to be.
// The event bus and the DB3 wire are both shared packages, not modules, so any
// module may speak them -- which is the whole point of peers meeting on the bus
// rather than on each other's headers.

// ErrVectorBusConfig reports a bus attachment that cannot be used.
var ErrVectorBusConfig = errors.New("postgres: invalid DB3 bus attachment")

// busSearcher adapts db3.SearchCaller to the router's ProviderSearcher.
//
// The adapter exists because the two disagree about what a provider's typed
// failure means, and that disagreement is deliberate. On the wire a failure is
// an ANSWER: the provider is saying it cannot serve this. To the router it is
// not an answer at all, because acting on it as one would return no candidates,
// and no candidates is indistinguishable from a corpus with no matches. So a
// failure becomes an error here, and the router falls back to PostgreSQL.
type busSearcher struct {
	caller *db3.SearchCaller
}

// ErrProviderRefused reports a provider that answered with a typed failure.
var ErrProviderRefused = errors.New("postgres: the DB3 provider refused the search")

func (s busSearcher) Search(ctx context.Context, principal uint32,
	request db3.SearchRequest) (db3.SearchReply, error) {
	reply, failure, err := s.caller.Search(ctx, request)
	if err != nil {
		return db3.SearchReply{}, err
	}
	if failure.Code != 0 {
		return db3.SearchReply{}, ErrProviderRefused
	}
	return reply, nil
}

// VectorBus keeps this module's provider registry current and carries its
// searches, on the module's OWN bus attachment.
//
// It is a bus.ModuleSidecar. The module loop owns the only reader and drops
// every event that is not a request to one of its stages -- which is exactly
// the search replies and capability publishes this needs -- so those events are
// handed here instead of discarded. Sending was never the difficulty; a second
// attachment for receiving is not available, because a principal attaches once.
type VectorBus struct {
	caller    *db3.SearchCaller
	publisher *db3.ApplyPublisher
	registry  *db3.ProviderRegistry
}

// AttachVectorBus connects this module to the DB3 wire.
//
// It is OPTIONAL in the strongest sense: a deployment with no bus, or no
// provider on it, never calls this, and vector operations stay in-database on
// pgvector or pgvectorscale exactly as they always have. Nothing here may make
// the absence of a provider an error.
func AttachVectorBus(client *bus.Client, registry *db3.ProviderRegistry) (*VectorBus, error) {
	if client == nil {
		return nil, ErrVectorBusConfig
	}
	if registry == nil {
		registry = db3.NewProviderRegistry()
	}
	attachment := &VectorBus{registry: registry}
	// The caller reads every event on this client, so provider announcements
	// arrive here rather than needing a second subscription. That is what makes
	// the module DISCOVER a provider: anything else means being told a
	// generation by something that guessed it, and the wire refuses a search at
	// a generation the provider is not actually at.
	caller, err := db3.NewSearchCallerWithObserver(client,
		func(principal, handle uint32, sequence uint64, capabilities db3.Capabilities) {
			registry.Observe(principal, handle, sequence, capabilities)
		})
	if err != nil {
		return nil, err
	}
	attachment.caller = caller
	// The write half. A provider that is searched but never written to answers
	// correctly and emptily forever, which reads as a corpus with no matches
	// rather than one nobody filled.
	publisher, err := db3.NewApplyPublisher(client)
	if err != nil {
		return nil, err
	}
	attachment.publisher = publisher
	return attachment, nil
}

// Registry is the provider registry this attachment feeds.
func (v *VectorBus) Registry() *db3.ProviderRegistry { return v.registry }

// Searcher is the transport to hand NewVectorRouter.
func (v *VectorBus) Searcher() ProviderSearcher { return busSearcher{caller: v.caller} }

// Close releases the attachment.
func (v *VectorBus) Close() {
	if v == nil {
		return
	}
	if v.caller != nil {
		v.caller.Close()
	}
	if v.publisher != nil {
		v.publisher.Close()
	}
}

// PublishApply ships one committed operation to every admitted provider.
//
// The postgres module owns this because it owns the canonical rows: an
// operation is published only after it has committed here, so a provider can
// never hold a row PostgreSQL does not.
func (v *VectorBus) PublishApply(ctx context.Context, apply db3.Apply) error {
	if v == nil || v.publisher == nil {
		return ErrVectorBusConfig
	}
	return v.publisher.PublishApply(ctx, apply)
}

// NewBusVectorRouter builds a router wired to a bus, with PostgreSQL behind it.
//
// This is the constructor a deployment uses. With no client it still builds a
// working router -- one that answers every search in-database -- because an
// external vector database is optional and its absence is the ordinary case.
func NewBusVectorRouter(client *bus.Client,
	fallback PostgreSQLSearch) (*VectorRouter, *VectorBus, error) {
	if fallback == nil {
		return nil, nil, ErrNoVectorFallback
	}
	if client == nil {
		router, err := NewVectorRouter(nil, nil, fallback)
		return router, nil, err
	}
	attachment, err := AttachVectorBus(client, nil)
	if err != nil {
		return nil, nil, err
	}
	router, err := NewVectorRouter(attachment.Registry(), attachment.Searcher(), fallback)
	if err != nil {
		attachment.Close()
		return nil, nil, err
	}
	return router, attachment, nil
}

// ObserveCapabilities records a provider announcement.
//
// The attachment's own read loop already does this for every announcement on
// its client. This exists for one that reached the module by some other route,
// and it goes through the registry so the band check and the staleness rules
// apply however it arrived.
func (v *VectorBus) ObserveCapabilities(principal, handle uint32, sequence uint64,
	capabilities db3.Capabilities) bool {
	if v == nil {
		return false
	}
	return v.registry.Observe(principal, handle, sequence, capabilities)
}

// ForgetProvider drops a provider that has detached.
func (v *VectorBus) ForgetProvider(principal, handle uint32) bool {
	if v == nil {
		return false
	}
	return v.registry.Remove(principal, handle)
}

// vectorSearchDeadline bounds a routed search.
//
// A provider that hangs must not hold the caller past the point where asking
// PostgreSQL would have answered. The fallback is always available, so waiting
// longer than this can only make the answer later, never better.
const vectorSearchDeadline = 2 * time.Second

// SearchWithDeadline routes a search under the module's own deadline.
func (r *VectorRouter) SearchWithDeadline(ctx context.Context,
	request db3.SearchRequest) (db3.SearchReply, RouteKind, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	// The deadline bounds the PROVIDER, not the fallback: a cancelled provider
	// search must still leave PostgreSQL able to answer, or a slow provider
	// would become a failed search rather than a slow one.
	bounded, cancel := context.WithTimeout(ctx, vectorSearchDeadline)
	reply, route, err := r.Search(bounded, request)
	cancel()
	if err != nil && ctx.Err() == nil && route != RoutePostgreSQL {
		return r.fallbackOnly(ctx, request)
	}
	return reply, route, err
}

func (r *VectorRouter) fallbackOnly(ctx context.Context,
	request db3.SearchRequest) (db3.SearchReply, RouteKind, error) {
	reply, err := r.fallback(ctx, request)
	return reply, RouteFellBack, err
}

// --- bus.ModuleSidecar ---------------------------------------------------
//
// The module loop calls these. They are the whole reason this module can speak
// DB3 without a second attachment.

// VectorSidecar carries DB3 on a module's own attachment.
//
// Built before the client exists, because the module runtime hands the client
// to the sidecar rather than the other way round: the process declares what it
// will carry, then the loop attaches once and passes it in.
type VectorSidecar struct {
	registry *db3.ProviderRegistry
	fallback PostgreSQLSearch

	mu     sync.Mutex
	bus    *VectorBus
	router *VectorRouter
}

// NewVectorSidecar declares that this module will carry DB3.
//
// The fallback is required and the provider is not: with no vector database
// installed the sidecar still attaches, the registry stays empty, and every
// search runs in-database. That is the ordinary deployment.
func NewVectorSidecar(fallback PostgreSQLSearch) (*VectorSidecar, error) {
	if fallback == nil {
		return nil, ErrNoVectorFallback
	}
	return &VectorSidecar{registry: db3.NewProviderRegistry(), fallback: fallback}, nil
}

// NewVectorSidecarForCollection is the constructor a module process uses: it
// binds the in-database search to the same collection a provider would serve,
// so the two answer from the same relation.
func NewVectorSidecarForCollection(collection string) (*VectorSidecar, error) {
	search, err := NewPGVectorSearch(collection)
	if err != nil {
		return nil, err
	}
	return NewVectorSidecar(search)
}

func (s *VectorSidecar) Attached(client *bus.Client) {
	attachment, err := AttachVectorBus(client, s.registry)
	if err != nil {
		// Without the DB3 half this module still serves every vector operation
		// in-database, so a failure here degrades to the default rather than
		// taking the module down.
		return
	}
	router, err := NewVectorRouter(s.registry, attachment.Searcher(), s.fallback)
	if err != nil {
		attachment.Close()
		return
	}
	s.mu.Lock()
	s.bus, s.router = attachment, router
	s.mu.Unlock()
}

func (s *VectorSidecar) Absorb(event bus.Event) {
	s.mu.Lock()
	attachment := s.bus
	s.mu.Unlock()
	if attachment != nil {
		attachment.Absorb(event)
	}
}

func (s *VectorSidecar) Detached() {
	s.mu.Lock()
	attachment := s.bus
	s.bus, s.router = nil, nil
	s.mu.Unlock()
	if attachment != nil {
		attachment.Close()
	}
}

// PublishApply ships a committed operation to every admitted provider, or
// reports that this module has no DB3 attachment.
func (s *VectorSidecar) PublishApply(ctx context.Context, apply db3.Apply) error {
	s.mu.Lock()
	attachment := s.bus
	s.mu.Unlock()
	if attachment == nil {
		return ErrVectorBusConfig
	}
	return attachment.PublishApply(ctx, apply)
}

// Router is the router to route vector searches through, or nil before the
// module has attached.
func (s *VectorSidecar) Router() *VectorRouter {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.router
}

// Absorb hands one event to the DB3 caller.
func (v *VectorBus) Absorb(event bus.Event) {
	if v != nil && v.caller != nil {
		v.caller.Absorb(event)
	}
}
