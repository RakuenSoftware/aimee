package engine

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type discussionTestAgents struct {
	mu       sync.Mutex
	requests []DelegateRequest
	respond  func(DelegateRequest) (string, error)
}

func (a *discussionTestAgents) Delegate(_ context.Context, request DelegateRequest) (DelegateResult, error) {
	a.mu.Lock()
	a.requests = append(a.requests, request)
	a.mu.Unlock()
	response, err := a.respond(request)
	return DelegateResult{Response: response}, err
}

func discussionAnalysis(severity string) panelAnalysis {
	return panelAnalysis{
		Feedback: wfe.ReviewFeedback{SchemaVersion: 1, ArtifactHash: "hash", Findings: []wfe.Finding{{ID: "direction", Persona: "architecture", Severity: severity, Summary: "the direction is wrong"}}},
		Reports: []panelSeatReport{
			{Seat: panelSeat{persona: "architecture", participant: "participant-a", ordinal: 0}, Response: panelResponse{ArtifactStage: "plan", Verdict: "changes"}},
			{Seat: panelSeat{persona: "reviewer", participant: "participant-b", ordinal: 1}, Response: panelResponse{ArtifactStage: "plan", Verdict: "changes"}},
		},
		Voters: 2,
	}
}

func TestDiscussionNitsHaveExactlyOneCycle(t *testing.T) {
	analysis := discussionAnalysis("nit")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	runner := &NativeRunner{agents: agents}
	feedback, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
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
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"abstain"}]}`, issueID), nil
	}}
	runner := &NativeRunner{agents: agents}
	feedback, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 2 {
		t.Fatalf("abstention extended or erased issue: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionRejectsIncompleteBallots(t *testing.T) {
	analysis := discussionAnalysis("blocking")
	analysis.Feedback.Findings = append(analysis.Feedback.Findings, wfe.Finding{ID: "second", Severity: "blocking", Summary: "another defect"})
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	runner := &NativeRunner{agents: agents}
	_, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if !strings.Contains(errText, "discussion quorum 0/2") {
		t.Fatalf("incomplete ballots counted as successful: %q", errText)
	}
}

func TestDiscussionOrdinaryDisagreementDoesNotExtend(t *testing.T) {
	analysis := discussionAnalysis("blocking")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(request DelegateRequest) (string, error) {
		position := "agree"
		if strings.HasSuffix(request.DurableSlot, "seat:1") {
			position = "disagree"
		}
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":%q}]}`, issueID, position), nil
	}}
	runner := &NativeRunner{agents: agents}
	feedback, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 2 {
		t.Fatalf("ordinary disagreement extended: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionFoundationalTieExtendsOnlyUntilMajority(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(request DelegateRequest) (string, error) {
		position := "agree"
		if strings.Contains(request.DurableSlot, ":discussion:1:") && strings.HasSuffix(request.DurableSlot, "seat:1") {
			position = "disagree"
		}
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":%q}]}`, issueID, position), nil
	}}
	runner := &NativeRunner{agents: agents}
	feedback, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 1 || len(agents.requests) != 4 {
		t.Fatalf("foundational consensus: calls=%d err=%q feedback=%+v", len(agents.requests), errText, feedback)
	}
}

func TestDiscussionFoundationalAgreementHasOneCycle(t *testing.T) {
	analysis := discussionAnalysis("foundational")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"agree"}]}`, issueID), nil
	}}
	runner := &NativeRunner{agents: agents}
	_, _, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(agents.requests) != 2 {
		t.Fatalf("unanimous foundational issue extended: calls=%d err=%q", len(agents.requests), errText)
	}
}

func TestDiscussionRejectMajorityDropsIssueDeterministically(t *testing.T) {
	analysis := discussionAnalysis("suggestion")
	issueID := makeDiscussionIssues(analysis.Feedback.Findings)[0].ID
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"positions":[{"id":%q,"position":"disagree"}]}`, issueID), nil
	}}
	runner := &NativeRunner{agents: agents}
	feedback, approvals, _, errText := runner.runPanelDiscussion(context.Background(), StepRequest{WorkItem: db1.WorkItem{ID: "wi"}, Node: wfe.Node{ID: "gate"}}, roundtablecfg.Panel{Discussion: true, MinSuccessful: 2}, analysis, "plan")
	if errText != "" || len(feedback.Findings) != 0 || approvals != 2 {
		t.Fatalf("deterministic rejection failed: approvals=%d err=%q feedback=%+v", approvals, errText, feedback)
	}
}
