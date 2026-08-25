package vectordb

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

// record_type is a LABEL within a collection, not the collection itself — the
// projection catalog stores it beside the vector. A provider that read it as
// the collection would search a namespace the caller never asked for and
// return confident results from the wrong corpus.
func TestRecordTypeNarrowsWithinTheCollectionRatherThanChoosingOne(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")

	// Two points in the one collection this provider serves, differing only by
	// their record_type label.
	if err := index.Upsert("memory", 1, []float32{1, 0, 0},
		labels("workspace", "w1", "record_type", "memory")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	if err := index.Upsert("memory", 2, []float32{1, 0, 0},
		labels("workspace", "w1", "record_type", "summary")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	// A point in a different collection, which must never be reachable.
	if err := index.Upsert("kb", 3, []float32{1, 0, 0},
		labels("workspace", "w1", "record_type", "memory")); err != nil {
		t.Fatalf("upsert: %v", err)
	}

	request := db3.SearchRequest{
		RequestID: 1, Workspace: "w1", RecordType: "summary",
		TopK: 10, Vector: []float32{1, 0, 0},
	}
	reply, failure := provider.Search(context.Background(), request)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 1 || reply.Candidates[0].PointID != 2 {
		t.Fatalf("candidates = %v, want only the matching record_type", reply.Candidates)
	}
}

// A provider serves exactly one collection. Nothing in another collection is
// reachable through it, whatever the request says.
func TestProviderNeverLeavesItsCollection(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")
	if err := index.Upsert("kb", 9, []float32{1, 0, 0},
		labels("workspace", "w1", "record_type", "kb")); err != nil {
		t.Fatalf("upsert: %v", err)
	}
	request := db3.SearchRequest{
		RequestID: 1, Workspace: "w1", RecordType: "kb",
		TopK: 10, Vector: []float32{1, 0, 0},
	}
	reply, failure := provider.Search(context.Background(), request)
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Fatalf("candidates = %v, want none from another collection", reply.Candidates)
	}
}

// An apply names its own collection, and one addressed elsewhere must not land
// in this provider's namespace. The wire's asymmetry — applies carry a
// collection, searches do not — is why this is checked here rather than assumed.
func TestApplyToAnotherCollectionIsNotSearchable(t *testing.T) {
	index := NewIndex(Cosine, 3)
	provider := NewProvider(index, "memory")

	outcome := provider.Apply(context.Background(), db3.Apply{
		OperationID: 1, Generation: 1, PointID: 5,
		Kind: db3.ApplyUpsert, Collection: "kb",
		Vector: []float32{1, 0, 0},
		Labels: labels("workspace", "w1", "record_type", "kb"),
	})
	if outcome.Result != db3.AppliedOK {
		t.Fatalf("result = %v", outcome.Result)
	}
	reply, failure := provider.Search(context.Background(), db3.SearchRequest{
		RequestID: 1, Workspace: "w1", TopK: 10, Vector: []float32{1, 0, 0},
	})
	if failure != 0 {
		t.Fatalf("failure = %v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Fatalf("candidates = %v, want none — the point belongs to kb", reply.Candidates)
	}
}
