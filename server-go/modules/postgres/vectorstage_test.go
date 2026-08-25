package postgres

import (
	"context"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

type recordingSearcher struct {
	asked    int
	lastFor  uint32
	response db3.SearchReply
}

func (s *recordingSearcher) Search(_ context.Context, principal uint32,
	request db3.SearchRequest) (db3.SearchReply, error) {
	s.asked++
	s.lastFor = principal
	reply := s.response
	reply.RequestID = request.RequestID
	reply.Generation = request.RequiredGeneration
	return reply, nil
}

func memorySearchRequest() db3.SearchRequest {
	return db3.SearchRequest{
		RequestID: 7, RequiredGeneration: 3, Workspace: "w1", Project: "alpha",
		RecordType: "memory", TopK: 4, Vector: []float32{1, 0, 0},
	}
}

func TestTheProvidersCollectionIsHandedOffAndEveryOtherIsNot(t *testing.T) {
	// The whole rule: a vector operation a provider can take goes to it, and
	// anything else is answered in-database. A provider serves one collection,
	// so "anything else" is every other collection -- not a degraded path, the
	// ordinary one.
	provider := VectorProvider{Principal: 456, Instance: "qdrant", Collection: "memory"}
	handler, err := NewVectorSearchHandler(provider, &recordingSearcher{})
	if err != nil {
		t.Fatal(err)
	}
	if got := handler.routers["memory"].Provider(); got != 456 {
		t.Errorf("the provider's own collection routes to %d, want 456", got)
	}
	for _, collection := range []string{"kb", "kb_pdf", "code", "curator_entity", "curator_narrative"} {
		if got := handler.routers[collection].Provider(); got != 0 {
			t.Errorf("collection %q routes to provider %d; it serves only %q",
				collection, got, provider.Collection)
		}
	}
}

func TestWithNoVectorDatabaseEveryCollectionIsAnsweredInDatabase(t *testing.T) {
	handler, err := NewVectorSearchHandler(VectorProvider{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	for collection, router := range handler.routers {
		if router.Provider() != 0 {
			t.Errorf("collection %q routed somewhere with nothing provisioned", collection)
		}
	}
}

func TestAHandedOffSearchReachesTheProviderAndComesBackOnTheWire(t *testing.T) {
	searcher := &recordingSearcher{response: db3.SearchReply{
		Candidates: []db3.Candidate{{PointID: 11, Score: 0.9}, {PointID: 12, Score: 0.4}},
	}}
	handler, err := NewVectorSearchHandler(
		VectorProvider{Principal: 456, Collection: "memory"}, searcher)
	if err != nil {
		t.Fatal(err)
	}
	body, err := encodeVectorSearch("memory", memorySearchRequest())
	if err != nil {
		t.Fatal(err)
	}
	response, status := handler.Handle(
		bus.ModuleInvocation{StageID: StageVectorSearch}, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %d", status)
	}
	if searcher.asked != 1 || searcher.lastFor != 456 {
		t.Fatalf("the provider was asked %d times as principal %d", searcher.asked, searcher.lastFor)
	}
	reply, err := db3.DecodeSearchReply(response)
	if err != nil {
		t.Fatal(err)
	}
	if len(reply.Candidates) != 2 || reply.Candidates[0].PointID != 11 {
		t.Fatalf("reply = %+v", reply)
	}
}

func TestASearchForAnotherCollectionNeverReachesTheProvider(t *testing.T) {
	// It is answered in-database instead. Without a database here that fails,
	// which is fine -- the assertion is that the provider was not asked about a
	// corpus it does not hold. Asking it would return another collection's
	// point ids, and DB2 would rehydrate rows the caller never searched for.
	searcher := &recordingSearcher{}
	handler, err := NewVectorSearchHandler(
		VectorProvider{Principal: 456, Collection: "memory"}, searcher)
	if err != nil {
		t.Fatal(err)
	}
	request := memorySearchRequest()
	request.RecordType = "kb"
	body, err := encodeVectorSearch("kb", request)
	if err != nil {
		t.Fatal(err)
	}
	handler.Handle(bus.ModuleInvocation{StageID: StageVectorSearch}, body)
	if searcher.asked != 0 {
		t.Fatalf("a kb search was handed to a provider serving memory")
	}
}

func TestAnUnknownCollectionIsRefusedRatherThanGuessed(t *testing.T) {
	handler, err := NewVectorSearchHandler(VectorProvider{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	body, err := encodeVectorSearch("'; drop table memory_embeddings--", memorySearchRequest())
	if err == nil {
		if _, status := handler.Handle(
			bus.ModuleInvocation{StageID: StageVectorSearch}, body); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("an unknown collection produced status %d", status)
		}
	}
}

func TestTheBodyRoundTripsAndRefusesRubbish(t *testing.T) {
	body, err := encodeVectorSearch("memory", memorySearchRequest())
	if err != nil {
		t.Fatal(err)
	}
	collection, request, err := decodeVectorSearch(body)
	if err != nil || collection != "memory" || request.RequestID != 7 {
		t.Fatalf("round trip gave (%q, %+v, %v)", collection, request, err)
	}
	for _, broken := range [][]byte{nil, {1}, {200, 0}, {6, 0, 'm'}} {
		if _, _, err := decodeVectorSearch(broken); err == nil {
			t.Errorf("decoded rubbish %v", broken)
		}
	}
}

func TestTheScopeSurvivesTheWireExactly(t *testing.T) {
	// Every vector has a workspace, a project, and a record type. They are
	// fields rather than filters precisely so they cannot go missing, and a
	// scope that arrived empty would widen the search to another workspace's
	// rows -- which DB2 refusing to rehydrate still leaks through the timing.
	request := memorySearchRequest()
	request.Filters = []db3.ExactLabel{{Key: "kind", Value: "directive"}}
	body, err := encodeVectorSearch("memory", request)
	if err != nil {
		t.Fatal(err)
	}
	collection, decoded, err := decodeVectorSearch(body)
	if err != nil {
		t.Fatal(err)
	}
	if collection != "memory" || decoded.Workspace != "w1" || decoded.Project != "alpha" ||
		decoded.RecordType != "memory" {
		t.Fatalf("scope did not survive: %q %+v", collection, decoded)
	}
	if decoded.TopK != request.TopK || len(decoded.Vector) != len(request.Vector) ||
		decoded.Vector[0] != 1 {
		t.Fatalf("vector did not survive: %+v", decoded)
	}
	if len(decoded.Filters) != 1 || decoded.Filters[0].Key != "kind" {
		t.Fatalf("filters did not survive: %+v", decoded.Filters)
	}
	if decoded.RequestID != request.RequestID || decoded.RequiredGeneration != request.RequiredGeneration {
		t.Fatalf("identity did not survive: %+v", decoded)
	}
}

func TestAVectorWithNoNearestNeighbourIsRefused(t *testing.T) {
	// NaN and infinity have no nearest neighbour, and stores disagree about
	// what to do with them -- some return nothing, some return everything.
	// Refused here so the answer cannot depend on which store was asked.
	for _, broken := range []float32{
		float32(math.NaN()), float32(math.Inf(1)), float32(math.Inf(-1)),
	} {
		request := memorySearchRequest()
		request.Vector = []float32{broken, 0, 0}
		body, err := encodeVectorSearch("memory", request)
		if err != nil {
			continue
		}
		if _, _, err := decodeVectorSearch(body); err == nil {
			t.Errorf("a vector containing %v was accepted", broken)
		}
	}
}

func TestTrailingBytesAreRefusedRatherThanIgnored(t *testing.T) {
	body, err := encodeVectorSearch("memory", memorySearchRequest())
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := decodeVectorSearch(append(body, 0)); err == nil {
		t.Error("a body with trailing bytes decoded; the disagreement is the bug")
	}
}
