package vectordb

import (
	"context"

	"github.com/JBailes/aimee/server-go/db3"
)

// Backend is the store a provider serves out of.
//
// WHY THIS IS AN INTERFACE. A DB3 provider is one module implementing one
// contract, and which vector store sits behind it is a deployment choice:
// Qdrant, Milvus, a remote pgvector, or the in-process index used by tests. One
// module with a pluggable backend is the difference between adding a store and
// forking the provider -- and a forked provider is how two of them come to
// disagree about scope filtering, which is the part that must never differ,
// because it is the only thing keeping one workspace's vectors out of another's
// answers.
//
// The interface is deliberately narrow. Everything a provider must NOT do --
// authorize, rehydrate, return payloads -- is absent by construction rather
// than by convention, so a backend has no way to offer it.
//
// EVERY METHOD MAY BE CALLED CONCURRENTLY. The provider serialises applies
// under its own lock but not searches, and a remote backend will have several
// in flight.
type Backend interface {
	// Generation reports the store's current version.
	//
	// DB2 searches AT a generation and the wire requires the reply to carry
	// exactly that generation back, so this is not bookkeeping: it is the value
	// the whole routed path agrees on. It MUST advance on every mutation, and a
	// backend whose store cannot report one must count its own.
	Generation(ctx context.Context) (uint64, error)

	// Dimension is the vector width the store was built for. A mismatch is
	// rejected rather than padded: a search at the wrong width returns
	// confident nonsense.
	Dimension() int

	// Metric is how the store scores. It is reported in the provider's
	// capabilities so DB2 never sends a query the store would answer by a
	// different measure than it asked for.
	Metric() Metric

	// Upsert stores one vector with the labels a filter may match on.
	Upsert(ctx context.Context, collection string, pointID int64,
		vector []float32, labels []db3.ExactLabel) error

	// Delete removes a point entirely.
	Delete(ctx context.Context, collection string, pointID int64) error

	// Tombstone marks a point unreachable while keeping its identity.
	//
	// Distinct from Delete because DB2 tombstones a row it may still be asked
	// about: a backend that deleted instead would let the id be reused.
	Tombstone(ctx context.Context, collection string, pointID int64) error

	// Search returns at most topK candidates matching every filter, nearest
	// first.
	//
	// The filters are AND, and a backend that cannot express them MUST return
	// an error rather than a wider answer. Widening here returns another
	// workspace's point ids, and DB2 refusing to rehydrate them still leaks
	// their existence through the timing.
	Search(ctx context.Context, collection string, vector []float32, topK int,
		filters []db3.ExactLabel) ([]db3.Candidate, error)

	// Close releases whatever the backend holds. Called once, on shutdown.
	Close() error
}
