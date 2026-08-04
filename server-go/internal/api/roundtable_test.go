package api

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) { return f(request) }

type fakeRoundtableReviewer struct {
	request roundtablecfg.ReviewRequest
	err     error
}

func (f *fakeRoundtableReviewer) Review(_ context.Context, request roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error) {
	f.request = request
	return roundtablecfg.RunResult{Artifact: "approved", Approved: true, ParticipantsTotal: 3, ParticipantsUsed: 3}, f.err
}

func TestRoundtableReviewEndpointRejectsValidationErrors(t *testing.T) {
	server, _, _ := newTestServer(t)
	server.SetRoundtableReviewer(&fakeRoundtableReviewer{err: roundtablecfg.ValidationError{Message: "invalid artifact"}})
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

func TestRoundtableReviewEndpointMaterializesGitHubPR(t *testing.T) {
	server, _, _ := newTestServer(t)
	reviewer := &fakeRoundtableReviewer{}
	server.SetRoundtableReviewer(reviewer)
	server.SetRoundtableArtifactHTTPClient(&http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.URL.String() != "https://github.com/JBailes/aimee/pull/1828.diff" {
			t.Fatalf("unexpected artifact URL %s", request.URL)
		}
		return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(strings.NewReader("diff --git a/a b/a\n")), Header: make(http.Header)}, nil
	})})
	req := httptest.NewRequest(http.MethodPost, "/v1/roundtable/review", strings.NewReader(`{"artifact":"https://github.com/JBailes/aimee/pull/1828","original_request":"review it"}`))
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || !strings.HasPrefix(reviewer.request.Artifact, "diff --git") {
		t.Fatalf("status=%d artifact=%q body=%s", rec.Code, reviewer.request.Artifact, rec.Body.String())
	}
}

func TestRoundtableReviewEndpointReportsArtifactDeliveryFailures(t *testing.T) {
	tests := []struct {
		name       string
		transport  roundTripFunc
		wantStatus int
	}{
		{name: "unavailable", wantStatus: http.StatusServiceUnavailable, transport: func(*http.Request) (*http.Response, error) {
			return nil, errors.New("network unavailable")
		}},
		{name: "oversized", wantStatus: http.StatusBadRequest, transport: func(*http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(io.LimitReader(zeroReader{}, roundtablecfg.MaxArtifactBytes+1)), Header: make(http.Header)}, nil
		}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			server, _, _ := newTestServer(t)
			server.SetRoundtableReviewer(&fakeRoundtableReviewer{})
			server.SetRoundtableArtifactHTTPClient(&http.Client{Transport: tc.transport})
			req := httptest.NewRequest(http.MethodPost, "/v1/roundtable/review", strings.NewReader(`{"artifact":"https://github.com/JBailes/aimee/pull/1828","original_request":"review it"}`))
			rec := httptest.NewRecorder()
			server.ServeHTTP(rec, req)
			if rec.Code != tc.wantStatus {
				t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
			}
		})
	}
}

type zeroReader struct{}

func (zeroReader) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = 'x'
	}
	return len(p), nil
}
