package db3

import "sync"

// ProviderRegistry tracks which vector providers are attached and serving.
//
// It lives in the wire package rather than in a module because "is a provider
// installed, and at what generation" is a fact about the DB3 contract, and more
// than one module needs it: DB2 routes its own portable searches, and the
// postgres module routes the vector operations that reach it. Two copies of
// this would be two answers to the same question, and the one that mattered
// would be whichever the caller happened to hold.
//
// EMPTY IS THE NORMAL STATE. A DB3 provider is optional. A deployment that
// installs none has an empty registry forever, Selected reports nothing, and
// every caller answers from PostgreSQL exactly as it did before. Nothing here
// may treat absence as an error.
type ProviderRegistry struct {
	mu        sync.RWMutex
	providers map[uint32]providerState
}

type providerState struct {
	handle       uint32
	sequence     uint64
	capabilities Capabilities
}

// NewProviderRegistry builds an empty registry.
func NewProviderRegistry() *ProviderRegistry {
	return &ProviderRegistry{providers: map[uint32]providerState{}}
}

// Observe records a provider's latest capabilities.
//
// Returns false when the announcement is stale or the principal is not a valid
// provider. Refusing an out-of-band principal HERE and not only where the
// process starts matters: the registry is fed from the bus, and a principal
// that reached it without going through the provider's own startup checks is
// exactly the case those checks cannot cover.
//
// Sequence is compared per provider, so a message that arrives out of order
// cannot move a provider backwards -- including backwards into "ready" after it
// has said it is not.
func (r *ProviderRegistry) Observe(principal, handle uint32, sequence uint64,
	capabilities Capabilities) bool {
	if r == nil || ValidateProviderRef(principal) != nil {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	existing, known := r.providers[principal]
	if known && existing.handle == handle && sequence <= existing.sequence {
		return false
	}
	r.providers[principal] = providerState{
		handle: handle, sequence: sequence, capabilities: capabilities,
	}
	return true
}

// Remove forgets a provider that has detached.
//
// The handle must match: a detach notice for a PREVIOUS attachment must not
// remove the provider that replaced it, or a restart would look like a
// permanent disappearance.
func (r *ProviderRegistry) Remove(principal, handle uint32) bool {
	if r == nil {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	existing, known := r.providers[principal]
	if !known || existing.handle != handle {
		return false
	}
	delete(r.providers, principal)
	return true
}

// Selected reports the provider to route to, if any.
//
// A provider qualifies only when it says it is Ready, advertises a non-zero
// generation, and offers the search operation. Ready is the provider's own
// statement about its backing store; a provider that is attached but still
// filling its index answers correctly and emptily, which is worse than not
// being used.
//
// When several qualify the highest generation wins, and ties break on the
// lowest principal so the choice is stable rather than map-order.
func (r *ProviderRegistry) Selected() (principal uint32, generation uint64, ok bool) {
	if r == nil {
		return 0, 0, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	for candidate, state := range r.providers {
		if !state.capabilities.Ready || state.capabilities.Generation == 0 {
			continue
		}
		if state.capabilities.Operations&OperationSearch == 0 {
			continue
		}
		if !ok || state.capabilities.Generation > generation ||
			(state.capabilities.Generation == generation && candidate < principal) {
			principal, generation, ok = candidate, state.capabilities.Generation, true
		}
	}
	return principal, generation, ok
}

// Capabilities returns what a provider last advertised.
func (r *ProviderRegistry) Capabilities(principal uint32) (Capabilities, bool) {
	if r == nil {
		return Capabilities{}, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	state, known := r.providers[principal]
	return state.capabilities, known
}

// Len reports how many providers are attached, ready or not.
func (r *ProviderRegistry) Len() int {
	if r == nil {
		return 0
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.providers)
}
