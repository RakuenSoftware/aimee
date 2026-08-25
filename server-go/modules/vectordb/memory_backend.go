package vectordb

import (
	"context"

	"github.com/JBailes/aimee/server-go/db3"
)

// MemoryBackend serves a provider out of the in-process Index.
//
// It is the reference implementation of Backend, and it is what the tests and
// the default provider use. Keeping it a thin adapter over Index rather than
// folding Backend into Index directly is deliberate: Index is a data structure
// with a synchronous API and no notion of a context or a remote failure, and a
// remote store is nothing like it. Adapting one to the other HERE means the
// difference shows up in one small file instead of spreading conditionals
// through the provider.
type MemoryBackend struct {
	index *Index
}

// NewMemoryBackend wraps an index as a backend.
func NewMemoryBackend(index *Index) *MemoryBackend {
	return &MemoryBackend{index: index}
}

// Index exposes the underlying store, for tests that seed it directly.
func (b *MemoryBackend) Index() *Index { return b.index }

func (b *MemoryBackend) Generation(context.Context) (uint64, error) {
	return b.index.Generation(), nil
}

func (b *MemoryBackend) Dimension() int { return b.index.dimension }

func (b *MemoryBackend) Metric() Metric { return b.index.metric }

func (b *MemoryBackend) Upsert(_ context.Context, collection string, pointID int64,
	vector []float32, labels []db3.ExactLabel) error {
	return b.index.Upsert(collection, pointID, vector, labels)
}

func (b *MemoryBackend) Delete(_ context.Context, collection string, pointID int64) error {
	b.index.Delete(collection, pointID)
	return nil
}

func (b *MemoryBackend) Tombstone(_ context.Context, collection string, pointID int64) error {
	b.index.Tombstone(collection, pointID)
	return nil
}

func (b *MemoryBackend) Search(_ context.Context, collection string, vector []float32,
	topK int, filters []db3.ExactLabel) ([]db3.Candidate, error) {
	return b.index.Search(collection, vector, topK, filters), nil
}

// Close is a no-op: the index dies with the process that made it.
func (b *MemoryBackend) Close() error { return nil }

// setWatermark records how far DB2's outbox has been applied.
//
// Only the in-process index carries this; a remote store does not need it,
// because the provider's own contiguous counter is the authority and the
// watermark on the index exists for tests to read.
func (b *MemoryBackend) setWatermark(value uint64) {
	b.index.mu.Lock()
	b.index.watermark = value
	b.index.mu.Unlock()
}
