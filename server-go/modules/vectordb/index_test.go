package vectordb

import (
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

// L2 is negated so larger is nearer for every metric this package serves.
// Returning a raw distance would silently invert every ranking.
func TestL2RanksNearestFirst(t *testing.T) {
	index := NewIndex(L2, 2)
	mustUpsert(t, index, 1, []float32{0, 0})
	mustUpsert(t, index, 2, []float32{10, 10})

	got := index.Search("c", []float32{0, 0}, 2, nil)
	if len(got) != 2 || got[0].PointID != 1 {
		t.Fatalf("ranking = %v, want the nearer point first", got)
	}
	if got[0].Score < got[1].Score {
		t.Error("scores must descend for L2 as for every other metric")
	}
}

func TestDotRanksLargestProductFirst(t *testing.T) {
	index := NewIndex(Dot, 2)
	mustUpsert(t, index, 1, []float32{1, 0})
	mustUpsert(t, index, 2, []float32{5, 0})

	got := index.Search("c", []float32{1, 0}, 2, nil)
	if got[0].PointID != 2 {
		t.Fatalf("ranking = %v, want the larger product first", got)
	}
}

// A NaN score sorts unpredictably and would scatter a zero vector through the
// ranking instead of placing it last.
func TestCosineOfZeroVectorIsZeroNotNaN(t *testing.T) {
	index := NewIndex(Cosine, 2)
	mustUpsert(t, index, 1, []float32{0, 0})
	mustUpsert(t, index, 2, []float32{1, 0})

	got := index.Search("c", []float32{1, 0}, 2, nil)
	if len(got) != 2 {
		t.Fatalf("candidates = %v", got)
	}
	if got[0].PointID != 2 {
		t.Errorf("the real match must outrank the zero vector, got %v", got)
	}
	for _, candidate := range got {
		if candidate.Score != candidate.Score {
			t.Fatal("a NaN score reached the ranking")
		}
	}
}

// Without a tie-break the order of equal-scoring points is the map's, so a
// caller paging through candidates could see one twice and miss another.
func TestEqualScoresBreakTiesByPointID(t *testing.T) {
	index := NewIndex(Cosine, 2)
	for id := int64(5); id >= 1; id-- {
		mustUpsert(t, index, id, []float32{1, 0})
	}
	for attempt := 0; attempt < 8; attempt++ {
		got := index.Search("c", []float32{1, 0}, 5, nil)
		for position, candidate := range got {
			if candidate.PointID != int64(position+1) {
				t.Fatalf("attempt %d: ranking = %v, want ascending ids", attempt, got)
			}
		}
	}
}

func TestSearchIsScopedToItsCollection(t *testing.T) {
	index := NewIndex(Cosine, 2)
	if err := index.Upsert("a", 1, []float32{1, 0}, nil); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	if err := index.Upsert("b", 2, []float32{1, 0}, nil); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	got := index.Search("a", []float32{1, 0}, 10, nil)
	if len(got) != 1 || got[0].PointID != 1 {
		t.Fatalf("candidates = %v, want only collection a", got)
	}
}

func TestSearchHonoursTopK(t *testing.T) {
	index := NewIndex(Cosine, 2)
	for id := int64(1); id <= 10; id++ {
		mustUpsert(t, index, id, []float32{1, 0})
	}
	if got := index.Search("c", []float32{1, 0}, 3, nil); len(got) != 3 {
		t.Fatalf("candidates = %d, want 3", len(got))
	}
}

func TestUpsertRefusesWrongDimension(t *testing.T) {
	index := NewIndex(Cosine, 3)
	if err := index.Upsert("c", 1, []float32{1, 0}, nil); err != ErrDimension {
		t.Fatalf("err = %v, want ErrDimension", err)
	}
}

// Generation advances on every mutation, which is what lets a caller tell a
// stale index from a current one.
func TestGenerationAdvancesOnEveryMutation(t *testing.T) {
	index := NewIndex(Cosine, 2)
	start := index.Generation()

	mustUpsert(t, index, 1, []float32{1, 0})
	afterUpsert := index.Generation()
	if afterUpsert <= start {
		t.Fatal("an upsert must advance the generation")
	}
	index.Delete("c", 1)
	afterDelete := index.Generation()
	if afterDelete <= afterUpsert {
		t.Fatal("a delete must advance the generation")
	}
	index.Tombstone("c", 2)
	if index.Generation() <= afterDelete {
		t.Fatal("a tombstone must advance the generation")
	}
}

// A tombstone records that a point existed and is gone, which is what lets a
// later replay tell "never seen" from "deleted". A delete simply drops it.
func TestTombstoneIsRememberedWhereDeleteIsNot(t *testing.T) {
	index := NewIndex(Cosine, 2)
	mustUpsert(t, index, 1, []float32{1, 0})
	mustUpsert(t, index, 2, []float32{1, 0})

	index.Delete("c", 1)
	index.Tombstone("c", 2)

	index.mu.RLock()
	_, deletedPresent := index.points[collectionKey{"c", 1}]
	tombstoned, tombstonePresent := index.points[collectionKey{"c", 2}]
	index.mu.RUnlock()

	if deletedPresent {
		t.Error("a deleted point must leave no record")
	}
	if !tombstonePresent || !tombstoned.tombstoned {
		t.Error("a tombstoned point must be remembered as gone")
	}
	if index.Len() != 0 {
		t.Errorf("live count = %d, want 0", index.Len())
	}
}

func TestSearchRejectsWrongDimensionAndTopK(t *testing.T) {
	index := NewIndex(Cosine, 2)
	mustUpsert(t, index, 1, []float32{1, 0})

	if got := index.Search("c", []float32{1, 0, 0}, 1, nil); got != nil {
		t.Errorf("wrong dimension returned %v", got)
	}
	if got := index.Search("c", []float32{1, 0}, 0, nil); got != nil {
		t.Errorf("zero topK returned %v", got)
	}
}

func TestFiltersMatchExactly(t *testing.T) {
	index := NewIndex(Cosine, 2)
	if err := index.Upsert("c", 1, []float32{1, 0},
		[]db3.ExactLabel{{Key: "workspace", Value: "w1"}}); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	if got := index.Search("c", []float32{1, 0}, 5,
		[]db3.ExactLabel{{Key: "workspace", Value: "w1"}}); len(got) != 1 {
		t.Errorf("matching filter returned %v", got)
	}
	if got := index.Search("c", []float32{1, 0}, 5,
		[]db3.ExactLabel{{Key: "workspace", Value: "w2"}}); len(got) != 0 {
		t.Errorf("a different value must not match: %v", got)
	}
}

func mustUpsert(t *testing.T, index *Index, id int64, vector []float32) {
	t.Helper()
	if err := index.Upsert("c", id, vector, nil); err != nil {
		t.Fatalf("upsert %d: %v", id, err)
	}
}
