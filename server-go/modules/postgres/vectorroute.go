package postgres

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/db3"
)

// Vector routing: send to DB3 what a provider can answer, keep the rest here.
//
// A DB3 provider is OPTIONAL, and that is the property this file exists to
// preserve. With none installed the registry is empty, every operation runs
// against PostgreSQL, and nothing about the deployment changes. Installing one
// is an accelerator for the portable subset, never a dependency: if the
// provider is absent, unready, slow, or wrong, the answer still comes from
// PostgreSQL.
//
// NOT EVERY VECTOR OPERATION CAN LEAVE. A provider takes a vector, a top-k and
// exact-match filters, and returns opaque ids and scores. Anything that needs a
// join, a payload, an ordering PostgreSQL computes, or authority over what the
// caller may see stays here -- not as a limitation to work around but because
// those are the operations DB2 must answer itself. RoutableSearch is the whole
// test, and a request that fails it is not a failure: it is an operation that
// runs where it always did.
type RouteKind uint8

const (
	// RoutePostgreSQL means the operation ran here.
	RoutePostgreSQL RouteKind = iota
	// RouteProvider means a DB3 provider answered it.
	RouteProvider
	// RouteFellBack means a provider was asked and could not answer, so
	// PostgreSQL did. Distinct from RoutePostgreSQL so a deployment can tell
	// "no provider installed" from "the provider is failing", which look
	// identical in the results and could not be more different operationally.
	RouteFellBack
)

func (k RouteKind) String() string {
	switch k {
	case RouteProvider:
		return "provider"
	case RouteFellBack:
		return "fell-back"
	default:
		return "postgresql"
	}
}

// ErrNoVectorFallback reports a router built without a PostgreSQL path.
var ErrNoVectorFallback = errors.New("postgres: vector routing needs a PostgreSQL fallback")

// ProviderSearcher sends one search to one provider.
//
// An interface rather than the bus client itself so the routing POLICY is
// testable without a bus, and so the transport can be supplied by whatever
// already owns a connection. A nil searcher is a deployment with no DB3 at all,
// which is the default.
//
// NOTHING SUPPLIES ONE YET. The bus transport that would carry a search to a
// provider lives in modules/db2, and the module boundary forbids importing it;
// making it shared means lifting the fragment and correlation machinery into
// the db3 package, which has not been done. So today every caller of this
// router passes nil and every search runs on PostgreSQL -- correct, and the
// same answer as before, but the routed half is policy without a wire.
//
// This is written down because the rest of this work was found by noticing
// exactly this shape: a component that is complete, tested, and connected to
// nothing reads as a working feature until someone checks.
type ProviderSearcher interface {
	Search(ctx context.Context, principal uint32, request db3.SearchRequest) (db3.SearchReply, error)
}

// PostgreSQLSearch is the pgvector path. It is never optional.
type PostgreSQLSearch func(ctx context.Context, request db3.SearchRequest) (db3.SearchReply, error)

// VectorRouter decides where a vector search runs.
type VectorRouter struct {
	registry *db3.ProviderRegistry
	searcher ProviderSearcher
	fallback PostgreSQLSearch
}

// NewVectorRouter builds a router. searcher may be nil, meaning no DB3.
func NewVectorRouter(registry *db3.ProviderRegistry, searcher ProviderSearcher,
	fallback PostgreSQLSearch) (*VectorRouter, error) {
	if fallback == nil {
		return nil, ErrNoVectorFallback
	}
	if registry == nil {
		registry = db3.NewProviderRegistry()
	}
	return &VectorRouter{registry: registry, searcher: searcher, fallback: fallback}, nil
}

// Registry exposes the provider registry so the caller can feed it from the bus.
func (r *VectorRouter) Registry() *db3.ProviderRegistry { return r.registry }

// RoutableSearch reports whether this request is one a provider could answer.
//
// The rule is the wire's own: if the request does not validate against the DB3
// contract, no provider can be asked for it. That keeps the definition of
// "portable" in ONE place -- the contract -- rather than as a second opinion
// here that could come to disagree with it.
func RoutableSearch(request db3.SearchRequest) bool {
	return request.Validate() == nil
}

// Search answers a vector search, routing it when it can and can be answered.
//
// The generation is not negotiable. DB2 asks at the generation it requires, and
// a provider that has not reached it would answer from an older corpus. So a
// provider behind the request is not used -- PostgreSQL is, which is always
// current because it is the canonical store.
func (r *VectorRouter) Search(ctx context.Context,
	request db3.SearchRequest) (db3.SearchReply, RouteKind, error) {
	if r == nil || r.fallback == nil {
		return db3.SearchReply{}, RoutePostgreSQL, ErrNoVectorFallback
	}
	if ctx == nil {
		ctx = context.Background()
	}

	principal, generation, ok := r.selectProvider(request)
	if !ok {
		reply, err := r.fallback(ctx, request)
		return reply, RoutePostgreSQL, err
	}
	_ = generation

	reply, err := r.searcher.Search(ctx, principal, request)
	if err == nil && db3.ValidateSearchReply(request, reply) == nil {
		return reply, RouteProvider, nil
	}

	// A provider that failed, or answered something the wire refuses, must not
	// become an empty result. An empty result is indistinguishable from a
	// corpus with no matches, and that is how a broken provider becomes a
	// silent loss of recall rather than a visible failure.
	//
	// The context ending is the one case that is NOT a fallback: the caller has
	// gone, and running the query here would spend PostgreSQL on an answer
	// nobody is waiting for.
	if ctx.Err() != nil {
		return db3.SearchReply{}, RouteFellBack, ctx.Err()
	}
	fallbackReply, fallbackErr := r.fallback(ctx, request)
	return fallbackReply, RouteFellBack, fallbackErr
}

// selectProvider picks a provider for this request, or reports none.
func (r *VectorRouter) selectProvider(request db3.SearchRequest) (uint32, uint64, bool) {
	if r.searcher == nil || !RoutableSearch(request) {
		return 0, 0, false
	}
	principal, generation, ok := r.registry.Selected()
	if !ok {
		return 0, 0, false
	}
	// A provider behind the required generation has not caught up with DB2's
	// writes. Ahead is equally refused: the wire requires the reply to carry
	// exactly the requested generation, so a provider ahead of it cannot answer
	// this request at all.
	if generation != request.RequiredGeneration {
		return 0, 0, false
	}
	return principal, generation, true
}
