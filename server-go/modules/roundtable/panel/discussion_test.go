package panel

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"
)

func TestDiscussionPromptUsesJSONIdentityEscaping(t *testing.T) {
	prompt := buildDiscussionPrompt("run\x01\"\\", "hash\x02", 1, nil, nil)
	if strings.Contains(prompt, `\x01`) || strings.Contains(prompt, `\x02`) {
		t.Fatalf("discussion identity used Go quoting instead of JSON escaping: %q", prompt)
	}
	if !strings.Contains(prompt, `"run_id":"run\u0001\"\\"`) ||
		!strings.Contains(prompt, `"artifact_hash":"hash\u0002"`) {
		t.Fatalf("discussion response contract lacks JSON-escaped identity: %q", prompt)
	}
}

// discussionTestAgents is a resource plane that answers with whatever the test
// dictates, recording every seat it was asked to open.
type discussionTestAgents struct {
	mu       sync.Mutex
	requests []SeatRequest
	respond  func(SeatRequest) (string, error)
}

func (a *discussionTestAgents) One(_ context.Context, _ Run, request SeatRequest) SeatResult {
	a.mu.Lock()
	a.requests = append(a.requests, request)
	a.mu.Unlock()
	response, err := a.respond(request)
	return SeatResult{Response: response, Err: err}
}

// Seats run concurrently in production, so the double does too: a data race
// here is a real one, and running them serially would hide it.
//
// It also supplies the two things a real plane does and the tests would
// otherwise have to restate on every response: the run/artifact identity a seat
// echoes back, and a distinct participant per seat. Without the participant a
// seat is never eligible for its one repair attempt, and without the identity
// every ballot is discarded as belonging to another run.
func (a *discussionTestAgents) Group(ctx context.Context, run Run, requests []SeatRequest) []SeatResult {
	out := make([]SeatResult, len(requests))
	var wg sync.WaitGroup
	wg.Add(len(requests))
	for i := range requests {
		go func(i int) {
			defer wg.Done()
			result := a.One(ctx, run, requests[i])
			if result.Err == nil {
				result.Response = withTestIdentity(result.Response, run, requests[i])
				result.Participant = fmt.Sprintf("test-participant:%d", i)
			}
			out[i] = result
		}(i)
	}
	wg.Wait()
	return out
}

// withTestIdentity fills in the run and artifact identity a seat is required to
// echo, so a test only has to state the part of the response it is about.
func withTestIdentity(response string, run Run, request SeatRequest) string {
	var object map[string]any
	if json.Unmarshal([]byte(response), &object) != nil {
		return response
	}
	if _, ok := object["run_id"]; !ok {
		object["run_id"] = run.ID
	}
	if _, ok := object["artifact_hash"]; !ok {
		object["artifact_hash"] = request.ArtifactHash
	}
	encoded, _ := json.Marshal(object)
	return string(encoded)
}

func discussionAnalysis(severity string) Analysis {
	return Analysis{
		Feedback: ReviewFeedback{SchemaVersion: 1, ArtifactHash: "hash", Findings: []Finding{{ID: "direction", Persona: "architecture", Severity: severity, Summary: "the direction is wrong"}}},
		Reports: []SeatReport{
			{Seat: Seat{Persona: "architecture", Participant: "participant-a", Ordinal: 0}, Response: panelResponse{ArtifactStage: "plan", Verdict: "changes"}},
			{Seat: Seat{Persona: "reviewer", Participant: "participant-b", Ordinal: 1}, Response: panelResponse{ArtifactStage: "plan", Verdict: "changes"}},
		},
		Voters: 2,
	}
}

func TestDiscussionNitsHaveExactlyOneCycle(t *testing.T) {
	analysis := discussionAnalysis("nit")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	feedback, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 {
		t.Fatalf("discussion failed: err=%q feedback=%+v", errText, feedback)
	}
	if feedback.Findings[0].ID != issueID {
		t.Fatalf("final feedback ID %q does not match discussion ID %q", feedback.Findings[0].ID, issueID)
	}
	if len(agents.requests) != 2 {
		t.Fatalf("nit caused %d calls, want exactly one two-seat cycle", len(agents.requests))
	}
	for _, request := range agents.requests {
		if !strings.Contains(request.DurableSlot, ":discussion:1:") {
			t.Fatalf("unexpected extra discussion cycle: %q", request.DurableSlot)
		}
	}
}

func TestDiscussionAbstentionAloneDoesNotExtend(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"abstain"}]}`, issueID), nil
	}}
	feedback, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 2 {
		t.Fatalf("abstention extended or erased issue: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionRejectsIncompleteBallots(t *testing.T) {
	analysis := discussionAnalysis("blocking")
	analysis.Feedback.Findings = append(analysis.Feedback.Findings, Finding{ID: "second", Severity: "blocking", Summary: "another defect"})
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	_, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if !strings.Contains(errText, "discussion quorum 0/2") {
		t.Fatalf("incomplete ballots counted as successful: %q", errText)
	}
}

func TestDiscussionRejectsStaleRunAndArtifactIdentity(t *testing.T) {
	analysis := discussionAnalysis("blocking")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"run_id":"another-run","artifact_hash":"stale","positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	_, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if !strings.Contains(errText, "discussion quorum 0/2") {
		t.Fatalf("stale discussion ballots counted as successful: %q", errText)
	}
}

func TestDiscussionOrdinaryDisagreementDoesNotExtend(t *testing.T) {
	analysis := discussionAnalysis("blocking")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(request SeatRequest) (string, error) {
		position := "agree"
		if strings.HasSuffix(request.DurableSlot, "seat:1") {
			position = "disagree"
		}
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":%q}]}`, issueID, position), nil
	}}
	feedback, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 2 {
		t.Fatalf("ordinary disagreement extended: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionFoundationalTieExtendsOnlyUntilMajority(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(request SeatRequest) (string, error) {
		position := "agree"
		if strings.Contains(request.DurableSlot, ":discussion:1:") && strings.HasSuffix(request.DurableSlot, "seat:1") {
			position = "disagree"
		}
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":%q}]}`, issueID, position), nil
	}}
	feedback, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 4 {
		t.Fatalf("foundational consensus: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionFailureRemainsDegradedAfterSeatRecovers(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	analysis.Reports = append(analysis.Reports, SeatReport{
		Seat:     Seat{Persona: "qa", Participant: "participant-c", Ordinal: 2},
		Response: panelResponse{ArtifactStage: "plan", Verdict: "changes"},
	})
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(request SeatRequest) (string, error) {
		if strings.Contains(request.DurableSlot, ":discussion:1:") && strings.HasSuffix(request.DurableSlot, "seat:2") {
			return "", fmt.Errorf("temporary seat failure")
		}
		position := "agree"
		if strings.Contains(request.DurableSlot, ":discussion:1:") && strings.HasSuffix(request.DurableSlot, "seat:1") {
			position = "disagree"
		}
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":%q}]}`, issueID, position), nil
	}}
	feedback, _, _, _, failed, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 6 {
		t.Fatalf("seat recovery did not complete consensus: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
	if failed != 1 {
		t.Fatalf("recovered seat erased degradation: failed=%d, want 1", failed)
	}
}

func TestDiscussionFoundationalAgreementHasOneCycle(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	_, _, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(agents.requests) != 2 {
		t.Fatalf("unanimous foundational issue extended: calls=%d err=%q", len(agents.requests), errText)
	}
}

func TestDiscussionRejectMajorityDropsIssueDeterministically(t *testing.T) {
	analysis := discussionAnalysis("suggestion")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"disagree"}]}`, issueID), nil
	}}
	feedback, approvals, _, _, _, errText := RunDiscussion(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 0 || approvals != 2 {
		t.Fatalf("deterministic rejection failed: approvals=%d err=%q feedback=%+v", approvals, errText, feedback)
	}
}
