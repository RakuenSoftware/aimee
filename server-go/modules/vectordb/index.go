// Package vectordb is an external DB3 vector provider.
//
// It is the other end of the portability contract in
// src/modules/db2/eventcontract/vector-portability.json: DB2 keeps the
// canonical rows and every authority decision, and this serves the portable
// half — candidate search over vectors, and the upsert/delete/tombstone stream
// fanned out from DB2's committed outbox.
//
// What it deliberately does NOT do is as important as what it does. It stores
// no payload beyond the labels a filter needs, returns nothing but opaque point
// ids and scores, and makes no authorization decision. A provider that returned
// content would let a caller read rows DB2 would have refused it, and the whole
// point of the split is that an external index cannot become a way around DB2's
// authority.
package vectordb

import (
	"math"
	"sort"
	"sync"

	"github.com/JBailes/aimee/server-go/db3"
)

// Metric is the similarity function an index scores with.
type Metric uint8

const (
	// Cosine scores by angle, ignoring magnitude.
	Cosine Metric = iota
	// L2 scores by negated euclidean distance, so larger is nearer for every
	// metric this package serves.
	L2
	// Dot scores by inner product.
	Dot
)

// point is one stored vector and the labels a filter may match on.
type point struct {
	vector []float32
	labels map[string]string
	// tombstoned marks a point removed by a tombstone rather than a delete.
	// The distinction is kept because a tombstone records that a point existed
	// and is gone, which is what lets a later replay tell "never seen" from
	// "deleted"; a delete simply drops it.
	tombstoned bool
}

// Index is a collection of vectors searchable by similarity.
//
// It is exact rather than approximate. An external index would normally be
// approximate, and the interface is the same either way — but exactness here
// means a test can assert the ranking is right rather than merely plausible,
// which is what makes this usable as the reference the contract is checked
// against.
type Index struct {
	mu         sync.RWMutex
	metric     Metric
	dimension  int
	points     map[collectionKey]*point
	generation uint64
	// watermark is the highest contiguously applied operation id. It is what
	// DB2 reads to know how far behind this provider is.
	watermark uint64
}

type collectionKey struct {
	collection string
	pointID    int64
}

// NewIndex builds an empty index over one metric and dimension.
func NewIndex(metric Metric, dimension int) *Index {
	return &Index{
		metric:     metric,
		dimension:  dimension,
		points:     map[collectionKey]*point{},
		generation: 1,
	}
}

// Generation reports the index's current generation.
//
// It advances on every mutation. A search carrying a RequiredGeneration newer
// than this one is refused rather than answered from a stale index, because a
// caller that asked for a generation is telling the provider it has already
// seen writes this index may not have.
func (i *Index) Generation() uint64 {
	i.mu.RLock()
	defer i.mu.RUnlock()
	return i.generation
}

// Watermark reports the highest applied operation id.
func (i *Index) Watermark() uint64 {
	i.mu.RLock()
	defer i.mu.RUnlock()
	return i.watermark
}

// Upsert stores or replaces a point.
func (i *Index) Upsert(collection string, pointID int64, vector []float32, labels []db3.ExactLabel) error {
	if len(vector) != i.dimension {
		return ErrDimension
	}
	i.mu.Lock()
	defer i.mu.Unlock()

	stored := make([]float32, len(vector))
	copy(stored, vector)
	labelMap := make(map[string]string, len(labels))
	for _, label := range labels {
		labelMap[label.Key] = label.Value
	}
	i.points[collectionKey{collection, pointID}] = &point{vector: stored, labels: labelMap}
	i.generation++
	return nil
}

// Delete removes a point outright.
func (i *Index) Delete(collection string, pointID int64) {
	i.mu.Lock()
	defer i.mu.Unlock()
	delete(i.points, collectionKey{collection, pointID})
	i.generation++
}

// Tombstone marks a point gone while remembering that it existed.
func (i *Index) Tombstone(collection string, pointID int64) {
	i.mu.Lock()
	defer i.mu.Unlock()
	key := collectionKey{collection, pointID}
	if existing, present := i.points[key]; present {
		existing.tombstoned = true
		existing.vector = nil
	} else {
		i.points[key] = &point{tombstoned: true}
	}
	i.generation++
}

// Len reports how many live points the index holds.
func (i *Index) Len() int {
	i.mu.RLock()
	defer i.mu.RUnlock()
	live := 0
	for _, stored := range i.points {
		if !stored.tombstoned {
			live++
		}
	}
	return live
}

// Search returns the topK nearest points, best first.
//
// Ties are broken by point id so the ranking is total. Without that, two points
// at the same distance could swap places between calls, and a caller paging
// through candidates would see one twice and miss another.
func (i *Index) Search(collection string, vector []float32, topK int, filters []db3.ExactLabel) []db3.Candidate {
	i.mu.RLock()
	defer i.mu.RUnlock()

	if topK <= 0 || len(vector) != i.dimension {
		return nil
	}

	candidates := make([]db3.Candidate, 0, len(i.points))
	for key, stored := range i.points {
		if key.collection != collection || stored.tombstoned || len(stored.vector) == 0 {
			continue
		}
		if !matchesAll(stored.labels, filters) {
			continue
		}
		candidates = append(candidates, db3.Candidate{
			PointID: key.pointID,
			Score:   score(i.metric, vector, stored.vector),
		})
	}

	sort.Slice(candidates, func(a, b int) bool {
		if candidates[a].Score != candidates[b].Score {
			return candidates[a].Score > candidates[b].Score
		}
		return candidates[a].PointID < candidates[b].PointID
	})
	if len(candidates) > topK {
		candidates = candidates[:topK]
	}
	return candidates
}

// matchesAll reports whether a point carries every filter label exactly.
//
// An absent label never matches. Treating a missing label as a wildcard would
// widen a scoped search into an unscoped one, which is how an index leaks
// across workspaces.
func matchesAll(labels map[string]string, filters []db3.ExactLabel) bool {
	for _, filter := range filters {
		value, present := labels[filter.Key]
		if !present || value != filter.Value {
			return false
		}
	}
	return true
}

// score answers the similarity, always oriented so larger is nearer.
func score(metric Metric, query, stored []float32) float64 {
	switch metric {
	case L2:
		// Negated so the ordering matches the other metrics. Returning a raw
		// distance here would silently invert every ranking.
		return -euclidean(query, stored)
	case Dot:
		return dot(query, stored)
	default:
		return cosine(query, stored)
	}
}

func dot(a, b []float32) float64 {
	var sum float64
	for index := range a {
		sum += float64(a[index]) * float64(b[index])
	}
	return sum
}

func euclidean(a, b []float32) float64 {
	var sum float64
	for index := range a {
		diff := float64(a[index]) - float64(b[index])
		sum += diff * diff
	}
	return math.Sqrt(sum)
}

// cosine answers zero for a zero-magnitude vector rather than NaN.
//
// A NaN score sorts unpredictably and would scatter a zero vector through the
// ranking instead of placing it last, which is a ranking bug that only appears
// with real data.
func cosine(a, b []float32) float64 {
	product := dot(a, b)
	magnitude := math.Sqrt(dot(a, a)) * math.Sqrt(dot(b, b))
	if magnitude == 0 {
		return 0
	}
	return product / magnitude
}
