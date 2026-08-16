package db2

import (
	"context"
	"errors"
	"sync"

	protocol "github.com/JBailes/aimee/server-go/db3"
)

var (
	ErrDB3RouterConfig    = errors.New("db3 router: invalid configuration")
	ErrDB3StaleCapability = errors.New("db3 router: stale capability evidence")
	ErrDB3Unavailable     = errors.New("db3 router: selected provider unavailable")
	ErrDB3InvalidResponse = errors.New("db3 router: invalid provider response")
)

type DB3Result uint8

const (
	DB3OK DB3Result = iota
	DB3InvalidRequest
	DB3Unavailable
	DB3ProviderFailure
	DB3InvalidResponse
	DB3Internal
)

type DB3RouteKind uint8

const (
	DB3DefaultPGVector DB3RouteKind = iota
	DB3External
	DB3ExplicitFallback
)

type DB3SearchResponse struct {
	Reply   *protocol.SearchReply
	Failure *protocol.SearchFailure
}

type DB3InternalSearcher func(context.Context, protocol.SearchRequest) (protocol.SearchReply, error)
type DB3ExternalSearcher func(context.Context, uint32, protocol.SearchRequest) (DB3SearchResponse, error)
type DB3CandidateAuthorizer func(context.Context, string, string, int64) (bool, error)

type DB3SearchOutcome struct {
	Result            DB3Result
	Route             DB3RouteKind
	SelectedPrincipal uint32
	ExternalError     DB3Result
	ProviderFailure   protocol.SearchFailureCode
	Reply             protocol.SearchReply
}

type db3Provider struct {
	handle       uint32
	sequence     uint64
	capabilities protocol.Capabilities
}

type db3Selection struct {
	principal  uint32
	generation uint64
	fallback   bool
	explicit   bool
}

// DB3Router is DB2's selection policy between its own pgvector implementation
// and an authenticated external DB3 provider. The external search function is
// a transport seam: production binds it to the event bus, never provider code.
type DB3Router struct {
	mu        sync.RWMutex
	providers map[uint32]db3Provider
	selection db3Selection
	internal  DB3InternalSearcher
	external  DB3ExternalSearcher
	authorize DB3CandidateAuthorizer
}

func NewDB3Router(internal DB3InternalSearcher, external DB3ExternalSearcher,
	authorize DB3CandidateAuthorizer) (*DB3Router, error) {
	if internal == nil || external == nil || authorize == nil {
		return nil, ErrDB3RouterConfig
	}
	return &DB3Router{
		providers: make(map[uint32]db3Provider), internal: internal, external: external,
		authorize: authorize,
	}, nil
}

// ObserveCapabilities records host-authenticated provider evidence. Principal
// and handle come from the bus frame, not the provider payload. Sequence is
// monotonic within one attachment; a new handle represents a new attachment
// and may restart its sequence.
func (r *DB3Router) ObserveCapabilities(principal, handle uint32, sequence uint64,
	capabilities protocol.Capabilities) error {
	if r == nil || principal == 0 || sequence == 0 || capabilities.Validate() != nil {
		return ErrDB3RouterConfig
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	current, exists := r.providers[principal]
	if exists && current.handle == handle && sequence <= current.sequence {
		return ErrDB3StaleCapability
	}
	r.providers[principal] = db3Provider{
		handle: handle, sequence: sequence, capabilities: capabilities,
	}
	if !r.selection.explicit {
		r.selectDeployedDefaultLocked()
	}
	return nil
}

func eligibleDB3Provider(capabilities protocol.Capabilities) bool {
	return capabilities.Ready &&
		capabilities.Operations&protocol.OperationSearch != 0 &&
		capabilities.Metrics&protocol.MetricCosine != 0 &&
		capabilities.Filters&protocol.FilterExact != 0
}

// selectDeployedDefaultLocked chooses the stable principal ordering used by
// deployment admission. Every ready provider remains an apply observer; the
// lowest eligible principal serves portable reads unless control installed an
// explicit override. Principal identities are deployment-owned and unique, so
// the result does not depend on capability arrival order.
func (r *DB3Router) selectDeployedDefaultLocked() {
	selection := db3Selection{}
	for principal, provider := range r.providers {
		if !eligibleDB3Provider(provider.capabilities) ||
			(selection.principal != 0 && principal >= selection.principal) {
			continue
		}
		selection.principal = principal
		selection.generation = provider.capabilities.Generation
	}
	r.selection = selection
}

// RemoveProvider removes one attachment's live evidence. An automatic route
// deterministically advances to the next deployed provider (or pgvector when
// none remains). An explicit route stays pinned and therefore fails closed if
// its selected provider disappears.
func (r *DB3Router) RemoveProvider(principal, handle uint32) bool {
	if r == nil || principal == 0 {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	current, exists := r.providers[principal]
	if !exists || current.handle != handle {
		return false
	}
	delete(r.providers, principal)
	if !r.selection.explicit {
		r.selectDeployedDefaultLocked()
	}
	return true
}

func (r *DB3Router) routeSnapshot(requestID uint64, result protocol.RouteResult) protocol.RouteReply {
	selection := r.selection
	return protocol.RouteReply{
		RequestID: requestID, Result: result, SelectedPrincipal: selection.principal,
		ProviderGeneration: selection.generation, Fallback: selection.fallback,
	}
}

// Route applies one validated control request atomically. Selection is a CAS
// against the provider generation observed by the controller, preventing a
// stale readiness snapshot from becoming the search route.
func (r *DB3Router) Route(request protocol.RouteRequest) protocol.RouteReply {
	if r == nil || request.Validate() != nil {
		return protocol.RouteReply{RequestID: request.RequestID, Result: protocol.RouteInvalid}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	switch request.Action {
	case protocol.RouteQuery:
		return r.routeSnapshot(request.RequestID, protocol.RouteOK)
	case protocol.RouteClear:
		// Clear removes the operator override, not the deployment policy. A ready
		// deployed provider therefore becomes the portable-read default again.
		r.selection = db3Selection{}
		r.selectDeployedDefaultLocked()
		return r.routeSnapshot(request.RequestID, protocol.RouteOK)
	case protocol.RouteSelect:
		provider, exists := r.providers[request.Principal]
		if !exists {
			return r.routeSnapshot(request.RequestID, protocol.RouteNotFound)
		}
		capabilities := provider.capabilities
		if !eligibleDB3Provider(capabilities) {
			return r.routeSnapshot(request.RequestID, protocol.RouteNotReady)
		}
		if capabilities.Generation != request.CapabilityGeneration {
			return r.routeSnapshot(request.RequestID, protocol.RouteGenerationConflict)
		}
		r.selection = db3Selection{
			principal: request.Principal, generation: capabilities.Generation,
			fallback: request.Fallback, explicit: true,
		}
		return r.routeSnapshot(request.RequestID, protocol.RouteOK)
	default:
		return r.routeSnapshot(request.RequestID, protocol.RouteInvalid)
	}
}

func cloneSearchRequest(request protocol.SearchRequest) protocol.SearchRequest {
	request.Vector = append([]float32(nil), request.Vector...)
	return request
}

func cloneSearchReply(reply protocol.SearchReply) protocol.SearchReply {
	reply.Candidates = append([]protocol.Candidate(nil), reply.Candidates...)
	return reply
}

func (r *DB3Router) callInternal(ctx context.Context,
	request protocol.SearchRequest) (protocol.SearchReply, DB3Result) {
	reply, err := r.internal(ctx, cloneSearchRequest(request))
	if err != nil {
		return protocol.SearchReply{}, DB3Internal
	}
	if protocol.ValidateSearchReply(request, reply) != nil {
		return protocol.SearchReply{}, DB3InvalidResponse
	}
	return cloneSearchReply(reply), DB3OK
}

func (r *DB3Router) callExternal(ctx context.Context, principal uint32,
	request protocol.SearchRequest) (protocol.SearchReply, DB3Result, protocol.SearchFailureCode) {
	response, err := r.external(ctx, principal, cloneSearchRequest(request))
	if err != nil {
		if ctx.Err() != nil {
			return protocol.SearchReply{}, DB3Internal, 0
		}
		if errors.Is(err, ErrDB3Unavailable) {
			return protocol.SearchReply{}, DB3Unavailable, 0
		}
		if errors.Is(err, ErrDB3InvalidResponse) {
			return protocol.SearchReply{}, DB3InvalidResponse, 0
		}
		return protocol.SearchReply{}, DB3ProviderFailure, 0
	}
	if (response.Reply == nil) == (response.Failure == nil) {
		return protocol.SearchReply{}, DB3InvalidResponse, 0
	}
	if response.Failure != nil {
		if response.Failure.Validate() != nil || response.Failure.RequestID != request.RequestID {
			return protocol.SearchReply{}, DB3InvalidResponse, 0
		}
		if response.Failure.Code == protocol.SearchFailureUnavailable {
			return protocol.SearchReply{}, DB3Unavailable, response.Failure.Code
		}
		return protocol.SearchReply{}, DB3ProviderFailure, response.Failure.Code
	}
	if protocol.ValidateSearchReply(request, *response.Reply) != nil {
		return protocol.SearchReply{}, DB3InvalidResponse, 0
	}
	return cloneSearchReply(*response.Reply), DB3OK, 0
}

func (r *DB3Router) authorizeReply(ctx context.Context, request protocol.SearchRequest,
	reply *protocol.SearchReply) DB3Result {
	kept := reply.Candidates[:0]
	for _, candidate := range reply.Candidates {
		allowed, err := r.authorize(ctx, request.Workspace, request.Project, candidate.PointID)
		if err != nil {
			return DB3Internal
		}
		if allowed {
			kept = append(kept, candidate)
		}
	}
	reply.Candidates = kept
	return DB3OK
}

func (r *DB3Router) Search(ctx context.Context, request protocol.SearchRequest) DB3SearchOutcome {
	outcome := DB3SearchOutcome{Result: DB3InvalidRequest}
	if r == nil || request.Validate() != nil {
		return outcome
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if ctx.Err() != nil {
		outcome.Result = DB3Internal
		return outcome
	}

	r.mu.RLock()
	selection := r.selection
	provider, providerExists := r.providers[selection.principal]
	r.mu.RUnlock()
	outcome.SelectedPrincipal = selection.principal

	if selection.principal == 0 {
		outcome.Route = DB3DefaultPGVector
		outcome.Reply, outcome.Result = r.callInternal(ctx, request)
	} else {
		outcome.Route = DB3External
		capabilities := provider.capabilities
		if !providerExists || !eligibleDB3Provider(capabilities) ||
			capabilities.Generation != selection.generation ||
			request.RequiredGeneration != selection.generation ||
			uint32(len(request.Vector)) > capabilities.MaxDimension || request.TopK > capabilities.MaxTopK {
			outcome.Result = DB3Unavailable
		} else {
			outcome.Reply, outcome.Result, outcome.ProviderFailure =
				r.callExternal(ctx, selection.principal, request)
		}
		if outcome.Result != DB3OK {
			outcome.ExternalError = outcome.Result
		}
		if outcome.Result != DB3OK && selection.fallback && ctx.Err() == nil {
			outcome.Route = DB3ExplicitFallback
			outcome.Reply, outcome.Result = r.callInternal(ctx, request)
		}
	}
	if outcome.Result == DB3OK {
		outcome.Result = r.authorizeReply(ctx, request, &outcome.Reply)
	}
	return outcome
}
