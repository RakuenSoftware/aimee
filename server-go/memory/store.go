package memory

import (
	"context"
	"encoding/json"
	"errors"
)

var ErrNotFound = errors.New("memory: record not found")
var ErrUnsupported = errors.New("memory: operation unsupported by backend")
var ErrUnavailable = errors.New("memory: backend unavailable")
var ErrCapacity = errors.New("memory: backend candidate capacity exceeded")

// Record is the existing public memory record. Provider-specific or optional
// authority fields retain their JSON representations rather than native types.
type Record struct {
	ID             int64                `json:"id"`
	Scope          Scope                `json:"scope"`
	Tier           string               `json:"tier"`
	Kind           string               `json:"kind"`
	Key            string               `json:"key"`
	Content        string               `json:"content"`
	Confidence     float64              `json:"confidence"`
	Version        *MemoryRecordVersion `json:"version,omitempty"`
	Historical     bool                 `json:"historical,omitempty"`
	Authorship     json.RawMessage      `json:"authorship,omitempty"`
	UtilityHorizon json.RawMessage      `json:"utility_horizon,omitempty"`
}

// Store is extracted from the existing native DataStore. Context carries the
// caller's cancellation and verified admission, never credentials in arguments.
// Implementations preserve scope, record identity, not-found, and deletion
// semantics. Put uses the existing upsert-by-kind/key behavior; IDs are assigned
// by the canonical source. Conditional mutation belongs to the extended API.
// Empty search queries list eligible records in the requested scope.
// The existing extended memory API remains a separate contract. Implementations
// of Store do not implicitly acquire graph, learning, history or authority APIs.
type Store interface {
	Get(context.Context, Scope, int64) (Record, error)
	Search(context.Context, Scope, string, string, string, int) ([]Record, error)
	Put(context.Context, Scope, Record) (Record, error)
	Delete(context.Context, Scope, int64) (bool, error)
}

type Capability struct {
	Name       string   `json:"name"`
	Version    int      `json:"version"`
	Operations []string `json:"operations"`
}

type Backend interface {
	Store
	Capabilities() Capability
}

// NamespaceStore optionally supplies an opaque, stable namespace for the
// admitted source audience. Backends must not derive tenancy from model input.
type NamespaceStore interface{ Namespace(Scope) string }

// DerivedStore removes retrieval state without mutating canonical records. The
// existing lifecycle owner calls this after admission and before retirement or
// destruction. Failure leaves the canonical mutation uncommitted and retryable.
// Forget is idempotent, including when a record is no longer serving-eligible.
type DerivedStore interface {
	Forget(context.Context, Scope, int64) error
}

// DerivedResetter discards every derived copy owned by this Aimee node. The
// existing subject-erasure coordinator invokes it after canonical erasure and
// before acknowledging completion. Memory startup invokes it before serving
// restored state. Canonical records and other nodes' namespaces are untouched.
// Implementations verify cleanup, report unknown outcomes, and allow retries.
type DerivedResetter interface {
	ResetDerived(context.Context) error
}
