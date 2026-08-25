package postgres

import (
	"context"
	"errors"
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

// VectorBus keeps a module's provider registry current and carries its searches.
//
// It owns the client it is given: it reads every event, so the client must not
// be shared with another reader. The bus delivers each event once, and two
// readers would each see half of them.
type VectorBus struct {
	caller   *db3.SearchCaller
	registry *db3.ProviderRegistry
}

// AttachVectorBus connects this module to the DB3 wire.
//
// It is OPTIONAL in the strongest sense: a deployment with no bus, or no
// provider on it, never calls this, and vector operations run on PostgreSQL
// exactly as they always have. Nothing here may make the absence of a provider
// an error.
func AttachVectorBus(ctx context.Context, client *bus.Client,
	registry *db3.ProviderRegistry) (*VectorBus, error) {
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
	caller, err := db3.NewSearchCallerWithObserver(ctx, client,
		func(principal, handle uint32, sequence uint64, capabilities db3.Capabilities) {
			registry.Observe(principal, handle, sequence, capabilities)
		})
	if err != nil {
		return nil, err
	}
	attachment.caller = caller
	return attachment, nil
}

// Registry is the provider registry this attachment feeds.
func (v *VectorBus) Registry() *db3.ProviderRegistry { return v.registry }

// Searcher is the transport to hand NewVectorRouter.
func (v *VectorBus) Searcher() ProviderSearcher { return busSearcher{caller: v.caller} }

// Close releases the attachment.
func (v *VectorBus) Close() {
	if v != nil && v.caller != nil {
		v.caller.Close()
	}
}

// NewBusVectorRouter builds a router wired to a bus, with PostgreSQL behind it.
//
// This is the constructor a deployment uses. With no client it still builds a
// working router -- one that answers every search from PostgreSQL -- because a
// DB3 provider is optional and its absence is the ordinary case.
func NewBusVectorRouter(ctx context.Context, client *bus.Client,
	fallback PostgreSQLSearch) (*VectorRouter, *VectorBus, error) {
	if fallback == nil {
		return nil, nil, ErrNoVectorFallback
	}
	if client == nil {
		router, err := NewVectorRouter(nil, nil, fallback)
		return router, nil, err
	}
	attachment, err := AttachVectorBus(ctx, client, nil)
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
