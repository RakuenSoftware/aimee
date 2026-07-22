package engine

import (
	"context"
	"testing"

	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
)

func TestSharedRoundtableReviewUsesGoEngine(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`}
	runner := &NativeRunner{agents: agents, roundtables: configuredTestRoundtable(t)}
	result, err := runner.Review(context.Background(), roundtablecfg.ReviewRequest{
		Artifact: "a complete implementation artifact for review", OriginalRequest: "implement the requested behavior",
	})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Approved || result.Degraded || result.ParticipantsTotal != 2 || result.ParticipantsFailed != 0 || result.ParticipantsUsed != 2 {
		t.Fatalf("result=%+v", result)
	}
	if len(agents.requests) != 2 || !agents.requests[0].ProvidedTarget || !agents.requests[1].ProvidedTarget {
		t.Fatalf("requests=%+v", agents.requests)
	}
}
