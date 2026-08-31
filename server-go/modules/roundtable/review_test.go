package roundtable

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
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
//
// The reason rides back with the failure. The runtime drops it before replying,
// so nothing reaches the caller but the status -- but it logs it on the way
// past, and without that every distinct failure here is the same bare number to
// whoever has to work out why reviews stopped.
func TestReviewHandlerReportsReviewerFailure(t *testing.T) {
	handler := NewReviewHandler(&stubReviewer{err: errors.New("panel unavailable")})
	body, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte(`{}`))
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want Internal", status)
	}
	if string(body) != "panel unavailable" {
		t.Fatalf("body = %q, want the reviewer's reason", body)
	}
}

func TestReviewHandlerWithoutReviewerFailsClosed(t *testing.T) {
	handler := NewReviewHandler(nil)
	if _, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte(`{}`)); status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want Internal", status)
	}
}

// The kind is not a free choice: the process contract computes it as
// 4096 + ordinal*256 + stage, and a mismatch means the daemon routes this kind
// to a module that does not serve it. Pinning it here catches a drift between
// the contract and this constant at build time rather than at attach time.
func TestReviewEventKindMatchesTheProcessContract(t *testing.T) {
	const roundtableOrdinal = 21
	if want := uint32(4096 + roundtableOrdinal*256 + StageReview); EventReview != want {
		t.Fatalf("EventReview = %d, want %d", EventReview, want)
	}
	if EventReview == EventDeliberate || StageReview == StageDeliberate {
		t.Fatal("review and deliberate must be distinguishable")
	}
	// Deliberate is stage 1 of the same module, so review has to be its
	// successor rather than a kind taken from elsewhere in the range.
	if EventReview != EventDeliberate+1 {
		t.Fatalf("EventReview %d does not follow EventDeliberate %d", EventReview, EventDeliberate)
	}
}

// One process serves both stages, so the dispatcher must route by stage id
// alone. A review body reaching the deliberate rubric, or the reverse, would be
// rejected as malformed and look like a broken caller.
func TestHandlerRoutesEachStageToItsOwnContract(t *testing.T) {
	stub := &stubReviewer{result: panel.RunResult{RunID: "run-1", Approved: true}}
	handler := NewHandler(stub)

	request, err := json.Marshal(panel.ReviewRequest{Artifact: "diff", RunID: "run-1"})
	if err != nil {
		t.Fatal(err)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageReview}, request); status != bus.ModuleStatusOK {
		t.Fatalf("review stage status = %v", status)
	}
	if stub.calls != 1 {
		t.Fatalf("reviewer ran %d times", stub.calls)
	}
	// The deliberate rubric has a fixed 40-byte contract; a review body is not
	// one, and must be rejected rather than reaching the reviewer.
	if _, status := handler(bus.ModuleInvocation{StageID: StageDeliberate}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("deliberate stage accepted a review body: %v", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: 99}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown stage was served: %v", status)
	}
	if stub.calls != 1 {
		t.Fatalf("reviewer ran %d times across all stages", stub.calls)
	}
}

// A stage no reviewer covers will be refused identically on every attempt. It
// has to be reported as an invalid request, not an internal fault: the caller
// uses that distinction to stop, and reporting it as internal made a workflow
// park and resubmit the same rejected review every few seconds forever.
func TestReviewHandlerReportsAPermanentlyBadRequestAsInvalid(t *testing.T) {
	handler := NewReviewHandler(&stubReviewer{
		err: panel.ValidationError{Message: `roundtable unsupported artifact stage "proposal"`}})
	body, status := handler(bus.ModuleInvocation{StageID: StageReview}, []byte(`{}`))
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest so the caller stops retrying", status)
	}
	if !strings.Contains(string(body), "unsupported artifact stage") {
		t.Fatalf("body = %q, want the reason for the log", body)
	}
}
