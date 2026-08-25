package qdrant

import (
	"context"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/db3"
	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// Against a real Qdrant.
//
// The fake in qdrant_test.go pins the REST shapes this code sends, which is
// what catches a field rename here. It cannot catch the other half: whether
// Qdrant agrees that those shapes mean what this code assumes. Filter
// semantics, tombstone exclusion, and the direction Euclid scores in are all
// facts about the server, and a fake will happily confirm whatever the client
// believes.
//
// Reads AIMEE_TEST_QDRANT_URL and SKIPS CLEANLY when it is unset, like the
// *-pg fixtures, so the ordinary suite stays green without a Qdrant.
func liveBackend(t *testing.T, metric vectordb.Metric, collection string) (*Backend, string) {
	t.Helper()
	url := os.Getenv("AIMEE_TEST_QDRANT_URL")
	if url == "" {
		t.Skip("AIMEE_TEST_QDRANT_URL is unset; a live Qdrant is required")
	}
	// A per-run prefix so a rerun never inherits the previous run's points, and
	// so two runs can share one Qdrant.
	prefix := fmt.Sprintf("aimeetest%d", time.Now().UnixNano())
	backend, err := New(Config{
		URL: url, Dimension: 3, Metric: metric, CollectionPrefix: prefix,
		Timeout: 20 * time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	// Dropped after the run. Each run uses a fresh prefix so it cannot inherit
	// the previous one's points, which is right for isolation and accumulates
	// collections without bound. Qdrant holds file descriptors per collection,
	// and a container that has run this suite enough times starts failing every
	// write with "Too many open files" -- which presents as a broken client and
	// is not one.
	t.Cleanup(func() {
		_ = backend.do(context.Background(), "DELETE",
			"/collections/"+backend.collectionName(collection), nil, nil)
	})
	return backend, collection
}

func seedLive(t *testing.T, backend *Backend, collection string) {
	t.Helper()
	ctx := context.Background()
	points := []struct {
		id      int64
		vector  []float32
		project string
	}{
		{1, []float32{1, 0, 0}, "alpha"},
		{2, []float32{0.9, 0.1, 0}, "alpha"},
		{3, []float32{0, 1, 0}, "beta"},
	}
	for _, point := range points {
		if err := backend.Upsert(ctx, collection, point.id, point.vector,
			[]db3.ExactLabel{{Key: "project", Value: point.project}}); err != nil {
			t.Fatalf("seeding point %d: %v", point.id, err)
		}
	}
}

func TestLiveQdrantFiltersByScopeAndOrdersNearestFirst(t *testing.T) {
	backend, collection := liveBackend(t, vectordb.Cosine, "memory")
	seedLive(t, backend, collection)
	ctx := context.Background()

	candidates, err := backend.Search(ctx, collection, []float32{1, 0, 0}, 10,
		[]db3.ExactLabel{{Key: "project", Value: "alpha"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 2 {
		t.Fatalf("scope filter returned %d candidates, want the 2 in project alpha: %+v",
			len(candidates), candidates)
	}
	if candidates[0].PointID != 1 || candidates[1].PointID != 2 {
		t.Fatalf("candidates = %+v, want point 1 then 2", candidates)
	}
	if !(candidates[0].Score > candidates[1].Score) {
		t.Errorf("scores are not nearest-first: %+v", candidates)
	}
	// The scope is not advisory. A point in another project must not appear.
	for _, candidate := range candidates {
		if candidate.PointID == 3 {
			t.Fatal("a point from project beta crossed the scope filter")
		}
	}
}

func TestLiveQdrantTombstoneHidesThePointButKeepsTheID(t *testing.T) {
	backend, collection := liveBackend(t, vectordb.Cosine, "memory")
	seedLive(t, backend, collection)
	ctx := context.Background()
	filters := []db3.ExactLabel{{Key: "project", Value: "alpha"}}

	if err := backend.Tombstone(ctx, collection, 1); err != nil {
		t.Fatal(err)
	}
	candidates, err := backend.Search(ctx, collection, []float32{1, 0, 0}, 10, filters)
	if err != nil {
		t.Fatal(err)
	}
	for _, candidate := range candidates {
		if candidate.PointID == 1 {
			t.Fatal("a tombstoned point was returned")
		}
	}
	if len(candidates) != 1 || candidates[0].PointID != 2 {
		t.Fatalf("after the tombstone, candidates = %+v, want only point 2", candidates)
	}

	// Re-upserting is DB2 saying the point is reachable again. If the flag
	// survived, the point would be hidden forever.
	if err := backend.Upsert(ctx, collection, 1, []float32{1, 0, 0}, filters); err != nil {
		t.Fatal(err)
	}
	candidates, err = backend.Search(ctx, collection, []float32{1, 0, 0}, 10, filters)
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 2 {
		t.Fatalf("an upsert did not clear the tombstone: %+v", candidates)
	}
}

func TestLiveQdrantDeleteRemovesThePoint(t *testing.T) {
	backend, collection := liveBackend(t, vectordb.Cosine, "memory")
	seedLive(t, backend, collection)
	ctx := context.Background()
	filters := []db3.ExactLabel{{Key: "project", Value: "alpha"}}

	if err := backend.Delete(ctx, collection, 2); err != nil {
		t.Fatal(err)
	}
	candidates, err := backend.Search(ctx, collection, []float32{1, 0, 0}, 10, filters)
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 1 || candidates[0].PointID != 1 {
		t.Fatalf("after the delete, candidates = %+v, want only point 1", candidates)
	}
}

func TestLiveQdrantEuclidRanksNearestHighest(t *testing.T) {
	// Qdrant returns a DISTANCE for Euclid. If the negation were wrong, this is
	// the test that fails: the ids would still be plausible and the order
	// exactly inverted.
	backend, collection := liveBackend(t, vectordb.L2, "memory")
	seedLive(t, backend, collection)

	candidates, err := backend.Search(context.Background(), collection, []float32{1, 0, 0}, 10,
		[]db3.ExactLabel{{Key: "project", Value: "alpha"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 2 {
		t.Fatalf("euclid search returned %+v", candidates)
	}
	if candidates[0].PointID != 1 {
		t.Fatalf("euclid ranked %d first; the exact point should win", candidates[0].PointID)
	}
	if !(candidates[0].Score > candidates[1].Score) {
		t.Errorf("euclid scores are not nearest-first: %+v", candidates)
	}
}

func TestLiveQdrantTopKBoundsTheAnswer(t *testing.T) {
	backend, collection := liveBackend(t, vectordb.Cosine, "memory")
	seedLive(t, backend, collection)
	candidates, err := backend.Search(context.Background(), collection, []float32{1, 0, 0}, 1, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 1 {
		t.Fatalf("top-k of 1 returned %d candidates", len(candidates))
	}
}
