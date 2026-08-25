package vectordb

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

// A filter the request carries must narrow the answer exactly as the fixed
// scope fields do. This is what lets a search whose meaning depends on a
// generation or a lifecycle state route at all.
func TestRequestFiltersNarrowTheAnswer(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "kb")

	if err := index.Upsert("kb", 1, []float32{1, 0, 0},
		labels("generation", "7", "lifecycle_state", "current", "project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	// Same project, retired generation. Without the filter this outranks
	// nothing but would still be returned.
	if err := index.Upsert("kb", 2, []float32{1, 0, 0},
		labels("generation", "6", "lifecycle_state", "current", "project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}

	request := db3.SearchRequest{
		RequestID: 1, Project: "p1", TopK: 10, Vector: []float32{1, 0, 0},
		Filters: []db3.ExactLabel{
			{Key: "generation", Value: "7"},
			{Key: "lifecycle_state", Value: "current"},
		},
	}
	reply, failure := provider.Search(context.Background(), request)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 1 || reply.Candidates[0].PointID != 1 {
		t.Fatalf("candidates = %v, want only the current generation", reply.Candidates)
	}
}

// A point missing a filtered label must not match, or a search for the current
// generation would return rows that never recorded one.
func TestRequestFilterOnAbsentLabelMatchesNothing(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "kb")
	if err := index.Upsert("kb", 1, []float32{1, 0, 0}, labels("project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}

	reply, failure := provider.Search(context.Background(), db3.SearchRequest{
		RequestID: 1, Project: "p1", TopK: 10, Vector: []float32{1, 0, 0},
		Filters: []db3.ExactLabel{{Key: "generation", Value: "7"}},
	})
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Fatalf("candidates = %v, want none", reply.Candidates)
	}
}

// Filters are ANDed with the scope rather than merged into it, so one that
// contradicts the scope matches nothing. The answer narrows; it never widens.
func TestContradictoryFilterMatchesNothing(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "kb")
	if err := index.Upsert("kb", 1, []float32{1, 0, 0}, labels("project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}

	reply, failure := provider.Search(context.Background(), db3.SearchRequest{
		RequestID: 1, Project: "p1", TopK: 10, Vector: []float32{1, 0, 0},
		Filters: []db3.ExactLabel{{Key: "project", Value: "p2"}},
	})
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Fatalf("candidates = %v, want none for a contradictory filter", reply.Candidates)
	}
}

// The provider advertises exact filtering, which is what lets DB2 route a
// filtered search to it at all. A provider without this capability must not be
// selected for one.
func TestProviderAdvertisesExactFiltering(t *testing.T) {
	provider := NewProvider(NewIndex(Cosine, 3), "kb")
	if provider.Capabilities().Filters&db3.FilterExact == 0 {
		t.Fatal("exact filtering backs every scoped and filtered search")
	}
}

// A filtered request must survive the wire and still narrow correctly, so the
// codec and the provider agree about what was asked.
func TestFilteredRequestSurvivesTheWire(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "kb")
	if err := index.Upsert("kb", 1, []float32{1, 0, 0},
		labels("generation", "7", "project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	if err := index.Upsert("kb", 2, []float32{1, 0, 0},
		labels("generation", "6", "project", "p1")); err != nil {
		t.Fatalf("upsert: %v", err)
	}

	sent := db3.SearchRequest{
		RequestID: 1, RequiredGeneration: 1, Project: "p1", RecordType: "kb",
		TopK: 10, Vector: []float32{1, 0, 0},
		Filters: []db3.ExactLabel{{Key: "generation", Value: "7"}},
	}
	encoded, err := db3.EncodeSearchRequest(sent)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	received, err := db3.DecodeSearchRequest(encoded)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}

	reply, failure := provider.Search(context.Background(), received)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	// record_type is also a filter, and these points carry none, so the search
	// correctly matches nothing — which is itself the point: an unlabelled
	// corpus does not silently satisfy a filtered request.
	if len(reply.Candidates) != 0 {
		t.Fatalf("candidates = %v", reply.Candidates)
	}

	// Label the points with the record type and the filtered search finds the
	// one current generation.
	if err := index.Upsert("kb", 3, []float32{1, 0, 0},
		labels("generation", "7", "project", "p1", "record_type", "kb")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	reply, failure = provider.Search(context.Background(), received)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 1 || reply.Candidates[0].PointID != 3 {
		t.Fatalf("candidates = %v, want the labelled current-generation point", reply.Candidates)
	}
}
