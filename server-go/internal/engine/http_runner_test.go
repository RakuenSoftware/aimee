package engine

import (
	"encoding/json"
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
		if request.Proposal != proposal || request.Plan != plan || request.Feedback == nil ||
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
		Plan:     plan,
		Feedback: &wfe.ReviewFeedback{Findings: []wfe.Finding{{Recommendation: recommendation}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Artifact != plan {
		t.Fatal("runner response artifact was incomplete")
	}
}
