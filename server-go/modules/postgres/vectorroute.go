package postgres

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/db3"
)

// Vector routing: send to DB3 what a provider can answer, keep the rest here.
//
// THE DEFAULT IS IN-DATABASE. Vector operations are served by PostgreSQL --
// pgvector, with pgvectorscale's DiskANN where the extension is present and the
// corpus is large enough to want it. That is the whole product for a deployment
// that installs nothing else, and it is not a fallback in the sense of a
// degraded mode: it is the ordinary path.
//
// An external vector database is something a user may OPTIONALLY install for
// PostgreSQL's use. With none installed the registry is empty, every operation
// runs in-database, and nothing about the deployment changes. Installing one
// accelerates the portable subset; it never becomes a dependency. If the
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
	// RoutePostgreSQL means the operation ran in-database, on pgvector or
	// pgvectorscale. This is the default and the ordinary case.
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
// vectorbus.go supplies one over the event bus. A nil searcher stays meaningful
// and is the ordinary case: it is a deployment that installed no DB3 at all.
type ProviderSearcher interface {
	Search(ctx context.Context, principal uint32, request db3.SearchRequest) (db3.SearchReply, error)
}

// PostgreSQLSearch is the in-database path: pgvector, or pgvectorscale's
// DiskANN where it is installed and chosen. It is never optional, because it is
// what a deployment has before it has anything else.
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
// The generation is not negotiable. The caller asks at the generation it
// requires, and a provider that has not reached it would answer from an older
// corpus. So a provider behind the request is not used -- the in-database path
// is, and it is always current because PostgreSQL is the canonical store.
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
	// A provider behind the required generation has not caught up with the
	// canonical store's writes. Ahead is equally refused: the wire requires the
	// reply to carry exactly the requested generation, so a provider ahead of it
	// cannot answer this request at all.
	if generation != request.RequiredGeneration {
		return 0, 0, false
	}
	return principal, generation, true
}
