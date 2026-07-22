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
}

func (f *fakeRoundtableReviewer) Review(_ context.Context, request roundtable.ReviewRequest) (roundtable.RunResult, error) {
	f.request = request
	return roundtable.RunResult{Artifact: "approved", Approved: true, ParticipantsTotal: 3, ParticipantsUsed: 3}, nil
}

func TestRoundtableReviewEndpointRoutesToGoEngine(t *testing.T) {
	server, _, _ := newTestServer(t)
	reviewer := &fakeRoundtableReviewer{}
	server.SetRoundtableReviewer(reviewer)
	req := httptest.NewRequest(http.MethodPost, "/v1/roundtable/review", strings.NewReader(`{"artifact":"a complete artifact that needs review","original_request":"the original request","roundtable":"default"}`))
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || reviewer.request.Roundtable != "default" || reviewer.request.Artifact == "" {
		t.Fatalf("status=%d request=%+v body=%s", rec.Code, reviewer.request, rec.Body.String())
	}
}
