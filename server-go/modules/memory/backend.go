package memory

import (
	"context"
	"errors"
	memorycontract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/memory/cognee"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
	"os"
	"strings"
	"time"
)

// BackendFactory receives the request-bound source contract, so adapters do not
// import native SQL types, caller admission or module implementation details.
type BackendFactory func(memorycontract.Store) (memorycontract.Backend, error)

func configuredMemoryBackend(executor egress.Executor) (BackendFactory, error) {
	name := strings.TrimSpace(os.Getenv("AIMEE_MEMORY_BACKEND"))
	switch name {
	case "", "native", "aimee-native":
		return nil, nil
	case "cognee":
		endpoint := os.Getenv("AIMEE_MEMORY_BACKEND_URL")
		if endpoint == "" {
			endpoint = os.Getenv("AIMEE_COGNEE_URL")
		}
		node, err := identity.Read(os.Getenv("AIMEE_HOME"))
		if err != nil {
			return nil, err
		}
		namespace := node.ID
		auth := os.Getenv("AIMEE_MEMORY_BACKEND_AUTH")
		if auth == "" {
			auth = "bearer"
		}
		if auth != "bearer" && auth != "none" {
			return nil, errors.New("memory: unsupported backend authentication")
		}
		credential := auth == "bearer"
		transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
			if executor == nil {
				return 0, nil, memorycontract.ErrUnavailable
			}
			timeout := 2 * time.Minute
			if deadline, ok := ctx.Deadline(); ok {
				timeout = min(timeout, time.Until(deadline))
			}
			if timeout <= 0 {
				return 0, nil, context.DeadlineExceeded
			}
			headers := map[string]string{"Accept": "application/json"}
			if contentType != "" {
				headers["Content-Type"] = contentType
			}
			handle := ""
			if credential {
				handle = "memory-backend"
			}
			response, err := executor.Do(ctx, 0, egress.HTTPRequest{CredentialHandle: handle, Request: egress.Request{TargetURL: target, Purpose: "memory-backend", Method: method, CredentialPresent: credential, RequestSHA256: egress.RequestDigest(method, target, body, credential)}, Headers: headers, Body: body, MaxResponseBytes: cognee.MaxBody, TimeoutMS: max(1, timeout.Milliseconds())})
			return response.Status, response.Body, err
		}
		// Validate configuration before serving; no silent native fallback.
		if _, err := cognee.New(NativeStore{}, transport, endpoint, namespace); err != nil {
			return nil, err
		}
		return func(source memorycontract.Store) (memorycontract.Backend, error) {
			return cognee.New(source, transport, endpoint, namespace)
		}, nil
	default:
		return nil, errors.New("memory: unknown configured backend")
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
	basis.Limit = cognee.MaxRecords + 1
	records, err := source.SearchVisible(ctx, basis)
	if err != nil {
		return nil, err
	}
	if len(records) > cognee.MaxRecords {
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
