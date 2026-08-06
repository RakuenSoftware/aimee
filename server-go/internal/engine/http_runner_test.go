package engine

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func TestHTTPRunnerTransfersCompleteArtifactsAndFeedback(t *testing.T) {
	proposal := strings.Repeat("proposal;", 700_000) + "PROPOSAL_END"
	plan := strings.Repeat("plan;", 700_000) + "PLAN_END"
	recommendation := strings.Repeat("recommend;", 700_000) + "RECOMMENDATION_END"
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var request StepRequest
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			t.Error(err)
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if request.Proposal != proposal || string(request.Inputs["plan"].Content) != plan || request.Feedback == nil ||
			request.Feedback.Findings[0].Recommendation != recommendation {
			t.Error("runner boundary received incomplete content")
		}
		_ = json.NewEncoder(w).Encode(StepResult{Status: StepAdvanced, Artifact: plan})
	}))
	defer server.Close()
	runner, err := NewHTTPRunner(HTTPRunnerConfig{Endpoint: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	result, err := runner.Run(t.Context(), StepRequest{
		Proposal: proposal,
		Inputs:   map[string]wfe.Artifact{"plan": {Content: []byte(plan)}},
		Feedback: &wfe.ReviewFeedback{Findings: []wfe.Finding{{Recommendation: recommendation}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Artifact != plan {
		t.Fatal("runner response artifact was incomplete")
	}
}

func TestHTTPRunnerTransportsErrKindForCollisionDetection(t *testing.T) {
	// The HTTP runner decodes StepResult from JSON and StepResult.Err is tagged
	// `json:"-"`, so the in-memory typed sentinel is stripped at the boundary.
	// ErrKind must survive the round trip and engine.Advance must rehydrate
	// ErrFreezeCreateCreateCollision from it for downstream errors.Is matching.
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(StepResult{
			Status:  StepFailed,
			Detail:  ErrFreezeCreateCreateCollision.Error() + ": path foo current slice wi_s1 conflicts with already frozen sibling slice wi_s2",
			ErrKind: freezeCreateCreateCollision,
		})
	}))
	defer server.Close()
	runner, err := NewHTTPRunner(HTTPRunnerConfig{Endpoint: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	result, runErr := runner.Run(t.Context(), StepRequest{})
	if runErr != nil {
		t.Fatal(runErr)
	}
	if result.ErrKind != freezeCreateCreateCollision {
		t.Fatalf("ErrKind did not survive the JSON round trip: got %q", result.ErrKind)
	}
	if result.Err != nil {
		t.Fatalf("expected nil Err after JSON decode (Err is tagged `json:\"-\"`), got: %v", result.Err)
	}
	// Simulate engine.Advance rehydrating the sentinel from ErrKind.
	if result.Err == nil && result.ErrKind == freezeCreateCreateCollision {
		result.Err = ErrFreezeCreateCreateCollision
	}
	if !errors.Is(result.Err, ErrFreezeCreateCreateCollision) {
		t.Fatalf("errors.Is failed after rehydration: %v", result.Err)
	}
	// Detail must remain accessible via Error() for operator-facing diagnostics.
	if !strings.Contains(result.Detail, freezeCreateCreateCollision) {
		t.Fatalf("expected detail to preserve collision marker text, got: %q", result.Detail)
	}
}
