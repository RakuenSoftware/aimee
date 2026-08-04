package roundtable

import (
	"context"
	"encoding/json"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type stubReviewer struct {
	got    panel.ReviewRequest
	result panel.RunResult
	err    error
	calls  int
}

func (s *stubReviewer) Review(_ context.Context, request panel.ReviewRequest) (panel.RunResult, error) {
	s.calls++
	s.got = request
	return s.result, s.err
}

// The body on the wire is the SAME JSON the HTTP route carried, so moving the
// transport must not change the contract a caller sees.
func TestReviewHandlerRoundTripsTheJSONContract(t *testing.T) {
	stub := &stubReviewer{result: panel.RunResult{RunID: "run-1", Approved: true, Artifact: "reviewed"}}
	handler := NewReviewHandler(stub)
	request, err := json.Marshal(panel.ReviewRequest{
		Artifact: "diff", OriginalRequest: "two bugs", ArtifactStage: "frozen_diff", RunID: "run-1",
	})
	if err != nil {
		t.Fatal(err)
	}
	body, status := handler(bus.ModuleInvocation{StageID: StageReview}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if stub.calls != 1 || stub.got.OriginalRequest != "two bugs" || stub.got.ArtifactStage != "frozen_diff" {
		t.Fatalf("reviewer saw %+v after %d calls", stub.got, stub.calls)
	}
	var decoded panel.RunResult
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.RunID != "run-1" || !decoded.Approved || decoded.Artifact != "reviewed" {
		t.Fatalf("result = %+v", decoded)
	}
}

// A stage id belonging to the deliberate rubric must not reach the reviewer:
// the two stages share a module and only the id distinguishes them.
func TestReviewHandlerRejectsAnotherStage(t *testing.T) {
	stub := &stubReviewer{}
	handler := NewReviewHandler(stub)
	if _, status := handler(bus.ModuleInvocation{StageID: StageDeliberate}, []byte(`{}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest", status)
	}
	if stub.calls != 0 {
		t.Fatalf("reviewer ran %d times for the wrong stage", stub.calls)
	}
}

func TestReviewHandlerRejectsMalformedBody(t *testing.T) {
	stub := &stubReviewer{}
	handler := NewReviewHandler(stub)
	if _, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte("not json")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest", status)
	}
	if stub.calls != 0 {
		t.Fatalf("reviewer ran %d times on a malformed body", stub.calls)
	}
}

// A failing review is reported as a module failure, never as an empty success:
// a caller that reads an empty result as "approved, no findings" would ship
// unreviewed work.
func TestReviewHandlerReportsReviewerFailure(t *testing.T) {
	handler := NewReviewHandler(&stubReviewer{err: errors.New("panel unavailable")})
	body, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte(`{}`))
	if status != bus.ModuleStatusInternal || body != nil {
		t.Fatalf("status = %v body = %q, want Internal and no body", status, body)
	}
}

func TestReviewHandlerWithoutReviewerFailsClosed(t *testing.T) {
	handler := NewReviewHandler(nil)
	if _, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte(`{}`)); status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want Internal", status)
	}
}

// EventReview must not collide with an allocated kind, and must sit at or above
// the module base -- kinds below it belong to the bus itself.
func TestReviewEventKindIsDistinct(t *testing.T) {
	if EventReview == EventDeliberate {
		t.Fatal("review and deliberate share an event kind")
	}
	if EventReview < 256 {
		t.Fatalf("EventReview %d is below BUS_KIND_MODULE_BASE", EventReview)
	}
	if StageReview == StageDeliberate {
		t.Fatal("review and deliberate share a stage id")
	}
}
