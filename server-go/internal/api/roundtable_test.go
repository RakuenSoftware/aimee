package api

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/roundtable"
)

type fakeRoundtableReviewer struct {
	request roundtable.ReviewRequest
	err     error
}

func (f *fakeRoundtableReviewer) Review(_ context.Context, request roundtable.ReviewRequest) (roundtable.RunResult, error) {
	f.request = request
	return roundtable.RunResult{Artifact: "approved", Approved: true, ParticipantsTotal: 3, ParticipantsUsed: 3}, f.err
}

func TestRoundtableReviewEndpointRejectsValidationErrors(t *testing.T) {
	server, _, _ := newTestServer(t)
	server.SetRoundtableReviewer(&fakeRoundtableReviewer{err: roundtable.ValidationError{Message: "invalid artifact"}})
	req := httptest.NewRequest(http.MethodPost, "/v1/roundtable/review", strings.NewReader(`{"artifact":"invalid"}`))
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestRoundtableReviewEndpointRoutesToGoEngine(t *testing.T) {
	server, _, _ := newTestServer(t)
	reviewer := &fakeRoundtableReviewer{}
	server.SetRoundtableReviewer(reviewer)
	req := httptest.NewRequest(http.MethodPost, "/v1/roundtable/review", strings.NewReader(`{"artifact":"a complete artifact that needs review","original_request":"the original request","roundtable":"default","artifact_stage":"frozen_diff","run_id":"review-pr-1828"}`))
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || reviewer.request.Roundtable != "default" || reviewer.request.Artifact == "" || reviewer.request.RunID != "review-pr-1828" || reviewer.request.ArtifactStage != "frozen_diff" {
		t.Fatalf("status=%d request=%+v body=%s", rec.Code, reviewer.request, rec.Body.String())
	}
}
