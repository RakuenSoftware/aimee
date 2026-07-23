package engine

import (
	"context"
	"errors"
	"strings"
	"testing"

	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
)

func TestRoundtableReviewRejectsOversizedArtifact(t *testing.T) {
	runner := &NativeRunner{agents: &recordingAgents{}}
	_, err := runner.Review(context.Background(), roundtablecfg.ReviewRequest{
		Artifact: strings.Repeat("x", (16<<20)+1),
	})
	var validation roundtablecfg.ValidationError
	if !errors.As(err, &validation) || !strings.Contains(err.Error(), "16 MiB") {
		t.Fatalf("err=%v", err)
	}
}

func TestSharedRoundtableReviewUsesGoEngine(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`}
	runner := &NativeRunner{agents: agents, roundtables: configuredTestRoundtable(t)}
	result, err := runner.Review(context.Background(), roundtablecfg.ReviewRequest{
		Artifact: "a complete implementation artifact for review", OriginalRequest: "implement the requested behavior",
	})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Approved || !result.Converged || len(result.Items) != 0 || result.Degraded || result.ParticipantsTotal != 2 || result.ParticipantsFailed != 0 || result.ParticipantsUsed != 2 {
		t.Fatalf("result=%+v", result)
	}
	if len(agents.requests) != 2 || !agents.requests[0].ProvidedTarget || !agents.requests[1].ProvidedTarget {
		t.Fatalf("requests=%+v", agents.requests)
	}
}
