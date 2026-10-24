package memory

import (
	"context"
	"errors"
	memorycontract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"os"
	"strings"
)

// BackendFactory receives the request-bound source contract, so adapters do not
// import native SQL types, caller admission or module implementation details.
// Both external retrieval profiles explicitly bound the admitted snapshot.
const backendCandidateLimit = 256

type BackendFactory func(memorycontract.Store) (memorycontract.Backend, error)

// The legacy in-process providers are retired. Aims-db storage is selected by
// installing its independent module, never by silently falling back in this owner.
func configuredMemoryBackend(_ egress.Executor) (BackendFactory, error) {
	switch strings.TrimSpace(os.Getenv("AIMEE_MEMORY_BACKEND")) {
	case "", "native", "aimee-native":
		return nil, nil
	default:
		return nil, errors.New("memory: in-process backend retired or unknown; configure an independent memory module and export legacy data offline")
	}
}

func (s *postgresDataStore) backend(ctx context.Context) (memorycontract.Backend, error) {
	if s.backendFactory == nil {
		return NativeStore{Data: s}, nil
	}
	source := *s
	source.backendFactory = nil
	backend, err := s.backendFactory(NativeStore{Data: &source})
	if err != nil {
		return nil, err
	}
	if backend == nil || backend.Capabilities().Version != 1 {
		return nil, memorycontract.ErrUnsupported
	}
	return backend, nil
}

func (s *postgresDataStore) searchBackend(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]Record, error) {
	backend, err := s.backend(ctx)
	if err != nil {
		return nil, err
	}
	records, err := backend.Search(ctx, scope, query, kind, tier, limit)
	if err != nil {
		return nil, err
	}
	source := *s
	source.backendFactory = nil
	result := make([]Record, 0, len(records))
	seen := map[int64]bool{}
	for _, record := range records {
		if record.ID <= 0 || record.Scope != scope || seen[record.ID] {
			return nil, memorycontract.ErrUnavailable
		}
		seen[record.ID] = true
		current, err := source.Get(ctx, scope, record.ID)
		if err != nil {
			return nil, err
		}
		if current.Key != record.Key || current.Content != record.Content || current.Kind != record.Kind || current.Tier != record.Tier {
			return nil, memorycontract.ErrUnavailable
		}
		result = append(result, current)
	}
	if len(result) > limit {
		return nil, memorycontract.ErrUnavailable
	}
	return result, nil
}

// WithMemoryBackend selects a contract implementation inside the existing
// memory service. Module registration, caller admission and restarts are unchanged.
func WithMemoryBackend(factory BackendFactory) HandlerOption {
	return func(options *handlerOptions) {
		native, ok := options.data.(*postgresDataStore)
		if !ok {
			return
		}
		bound := *native
		bound.backendFactory = factory
		options.data = &bound
	}
}

func (s *postgresDataStore) backendGet(ctx context.Context, scope Scope, id int64) (Record, error) {
	backend, err := s.backend(ctx)
	if err != nil {
		return Record{}, err
	}
	return (ContractDataStore{Store: backend}).Get(ctx, scope, id)
}
func (s *postgresDataStore) backendPut(ctx context.Context, scope Scope, record Record) (Record, error) {
	backend, err := s.backend(ctx)
	if err != nil {
		return Record{}, err
	}
	return (ContractDataStore{Store: backend}).Put(ctx, scope, record)
}
func (s *postgresDataStore) backendDelete(ctx context.Context, scope Scope, id int64) (bool, error) {
	backend, err := s.backend(ctx)
	if err != nil {
		return false, err
	}
	return backend.Delete(ctx, scope, id)
}

func (s *postgresDataStore) searchVisibleBackend(ctx context.Context, request DataRequest) ([]Record, error) {
	source := *s
	source.backendFactory = nil
	if request.Query == "" {
		return source.SearchVisible(ctx, request)
	}
	basis := request
	basis.Query = ""
	basis.Kind = ""
	basis.Tier = ""
	basis.Limit = backendCandidateLimit + 1
	records, err := source.SearchVisible(ctx, basis)
	if err != nil {
		return nil, err
	}
	if len(records) > backendCandidateLimit {
		return nil, memorycontract.ErrCapacity
	}
	scopes := []Scope{}
	seenScopes := map[Scope]bool{}
	for _, record := range records {
		if !seenScopes[record.Scope] {
			seenScopes[record.Scope] = true
			scopes = append(scopes, record.Scope)
		}
	}
	// Preserve the existing visible-scope priority. The selected backend ranks
	// records inside each eligible scope; raw backend scores never cross scopes.
	out := make([]Record, 0, request.Limit)
	for _, scope := range scopes {
		remaining := request.Limit - len(out)
		if remaining <= 0 {
			break
		}
		found, err := s.searchBackend(ctx, scope, request.Query, request.Kind, request.Tier, remaining)
		if err != nil {
			return nil, err
		}
		out = append(out, found...)
	}
	return out, nil
}

// Forget is an optional extension of the same provider contract, used by the
// existing native lifecycle rather than another deletion coordinator.
func (s *postgresDataStore) forgetBackend(ctx context.Context, scope Scope, id int64) error {
	if s.backendFactory == nil {
		return nil
	}
	backend, err := s.backend(ctx)
	if err != nil {
		return err
	}
	if derived, ok := backend.(memorycontract.DerivedStore); ok {
		return derived.Forget(ctx, scope, id)
	}
	return nil
}

func resetDerivedBackend(ctx context.Context, data DataStore) error {
	var backend memorycontract.Store
	switch source := data.(type) {
	case *postgresDataStore:
		if source.backendFactory == nil {
			return nil
		}
		selected, err := source.backend(ctx)
		if err != nil {
			return err
		}
		backend = selected
	case ContractDataStore:
		backend = source.Store
	default:
		return memorycontract.ErrUnsupported
	}
	if resetter, ok := backend.(memorycontract.DerivedResetter); ok {
		return resetter.ResetDerived(ctx)
	}
	if _, derived := backend.(memorycontract.DerivedStore); derived {
		return memorycontract.ErrUnsupported
	}
	return nil // A provider without derived storage has no remote copies.
}

func hasSelectedBackend(data DataStore) bool {
	switch source := data.(type) {
	case *postgresDataStore:
		return source.backendFactory != nil
	case ContractDataStore:
		return true
	default:
		return false
	}
}
