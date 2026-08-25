package postgres

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/db3"
)

// Vector routing: send to a provisioned vector database what it can answer,
// keep everything else in-database.
//
// THE DEFAULT IS IN-DATABASE. Vector operations are served by PostgreSQL --
// pgvector, with pgvectorscale's DiskANN where the extension is present. That
// is the whole product for a deployment that installs nothing else, and it is
// not a degraded mode: it is the ordinary path.
//
// DECIDED ONCE, AT BOOT. An external vector database is something a user may
// OPTIONALLY install, and whether one is installed is a FACT ON DISK: the
// operator provisions a provider, which writes a grant the bus host reads once
// at start. This module reads the same directory at start, and the answer is
// then fixed for the life of the process.
//
// It is fixed deliberately. A vector database that came and went at runtime
// would mean the same query answered from different stores minutes apart, with
// no way for a caller to know which -- and recall that changes without anything
// changing is worse than recall that is merely lower. If a grant was not made,
// that is the deployment, and it stays that way until the service restarts.
//
// This replaces a registry fed by announcements over the bus. That machinery
// answered a question nobody asked: a provider cannot serve without a grant,
// grants load once at boot, and a module that discovers its own deployment can
// be wrong about it. One directory, read by the host and by this module, cannot
// disagree with itself.
//
// NOT EVERY VECTOR OPERATION CAN LEAVE. A provider takes a vector, a top-k and
// exact-match filters, and returns opaque ids and scores. Anything that needs a
// join, a payload, an ordering PostgreSQL computes, or authority over what the
// caller may see stays here -- not as a limitation to work around but because
// those are operations PostgreSQL must answer itself. RoutableSearch is the
// whole test, and a request that fails it is not a failure: it is an operation
// that runs where it always did.
type RouteKind uint8

const (
	// RoutePostgreSQL means the operation ran in-database, on pgvector or
	// pgvectorscale. This is the default and the ordinary case.
	RoutePostgreSQL RouteKind = iota
	// RouteProvider means the provisioned vector database answered it.
	RouteProvider
	// RouteFellBack means the provisioned provider was asked and could not
	// answer, so PostgreSQL did.
	//
	// Distinct from RoutePostgreSQL so a deployment can tell "no vector
	// database installed" from "the one installed is failing". Those look
	// identical in the results and could not be more different operationally:
	// the first is a choice, the second is an outage nobody was told about.
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

// ErrNoVectorFallback reports a router built without an in-database path.
var ErrNoVectorFallback = errors.New("postgres: vector routing needs an in-database fallback")

// ProviderSearcher sends one search to the provisioned provider.
//
// An interface so the routing POLICY is testable without a bus. A nil searcher
// is a deployment that provisioned no vector database, which is the default.
type ProviderSearcher interface {
	Search(ctx context.Context, principal uint32, request db3.SearchRequest) (db3.SearchReply, error)
}

// PostgreSQLSearch is the in-database path: pgvector, or pgvectorscale's
// DiskANN where it is installed. It is never optional, because it is what a
// deployment has before it has anything else.
type PostgreSQLSearch func(ctx context.Context, request db3.SearchRequest) (db3.SearchReply, error)

// VectorRouter decides where a vector search runs.
//
// Every field is set at construction and never written again, so there is no
// state for a search to race against and no way for the routing decision to
// change under a caller.
type VectorRouter struct {
	// principal is the provisioned provider's ref, or zero when none is.
	principal uint32
	searcher  ProviderSearcher
	fallback  PostgreSQLSearch
}

// NewVectorRouter builds a router for the deployment as provisioned.
//
// principal zero, or a nil searcher, means no vector database was provisioned
// and every search runs in-database -- for the life of this process.
func NewVectorRouter(principal uint32, searcher ProviderSearcher,
	fallback PostgreSQLSearch) (*VectorRouter, error) {
	if fallback == nil {
		return nil, ErrNoVectorFallback
	}
	if searcher == nil {
		principal = 0
	}
	return &VectorRouter{principal: principal, searcher: searcher, fallback: fallback}, nil
}

// Provider reports the provisioned provider's principal, or zero for none.
func (r *VectorRouter) Provider() uint32 {
	if r == nil {
		return 0
	}
	return r.principal
}

// RoutableSearch reports whether this request is one a provider could answer.
//
// The rule is the wire's own: if the request does not validate against the DB3
// contract, no provider can be asked for it. That keeps the definition of
// "portable" in ONE place -- the contract -- rather than as a second opinion
// here that could come to disagree with it.
func RoutableSearch(request db3.SearchRequest) bool {
	return request.Validate() == nil
}

// Search answers a vector search, routing it when a vector database was
// provisioned and the request is one a provider can serve.
func (r *VectorRouter) Search(ctx context.Context,
	request db3.SearchRequest) (db3.SearchReply, RouteKind, error) {
	if r == nil || r.fallback == nil {
		return db3.SearchReply{}, RoutePostgreSQL, ErrNoVectorFallback
	}
	if ctx == nil {
		ctx = context.Background()
	}

	if r.principal == 0 || !RoutableSearch(request) {
		reply, err := r.fallback(ctx, request)
		return reply, RoutePostgreSQL, err
	}

	reply, err := r.searcher.Search(ctx, r.principal, request)
	if err == nil && db3.ValidateSearchReply(request, reply) == nil {
		return reply, RouteProvider, nil
	}

	// A provider that failed, or answered something the wire refuses, must not
	// become an empty result. An empty result is indistinguishable from a
	// corpus with no matches, and that is how a broken provider becomes a
	// silent loss of recall rather than a visible failure.
	//
	// It does NOT stop being the route. A provider that is failing is a
	// deployment problem to fix, and quietly demoting it would hide exactly the
	// thing an operator needs to see -- as well as making the store a query
	// answers from depend on when it was asked.
	//
	// The context ending is the one case that is not a fallback: the caller has
	// gone, and running the query here would spend PostgreSQL on an answer
	// nobody is waiting for.
	if ctx.Err() != nil {
		return db3.SearchReply{}, RouteFellBack, ctx.Err()
	}
	fallbackReply, fallbackErr := r.fallback(ctx, request)
	return fallbackReply, RouteFellBack, fallbackErr
}
