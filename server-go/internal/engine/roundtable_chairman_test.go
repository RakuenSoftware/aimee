package engine

import (
	"context"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func chairmanRequest() StepRequest {
	artifact := wfe.Artifact{Type: "plan", Content: []byte("complete plan")}
	artifact.Hash = wfe.Hash(artifact.Content)
	return StepRequest{WorkItem: db1.WorkItem{ID: "wi", Worktree: "/worktree"}, Node: wfe.Node{ID: "gate"}, Proposal: "fix the scheduler", Inputs: map[string]wfe.Artifact{"src": artifact}}
}

func chairmanResponse(body string) string {
	req := chairmanRequest()
	return fmt.Sprintf(`{"run_id":%q,"artifact_hash":%q,%s`, req.WorkItem.ID, req.Inputs["src"].Hash, body)
}

func TestChairmanSubmitsFinalApprovalAfterDeterministicSynthesis(t *testing.T) {
	wantHash := chairmanRequest().Inputs["src"].Hash
	agents := &discussionTestAgents{respond: func(request DelegateRequest) (string, error) {
		if request.Delegate != "codex" || request.Persona != "chairman" || request.MaxTurnsCap != roundtableDelegateMaxTurnsCap || !strings.Contains(request.DurableSlot, ":chairman") || !strings.Contains(request.Prompt, "BEGIN_CHAIRMAN_DATA") || !strings.Contains(request.Prompt, "plurality, format, or existence is never original-request drift") || !strings.Contains(request.Prompt, `"artifact_hash":"`+wantHash+`"`) {
			t.Fatalf("chairman request=%+v", request)
		}
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":" Approve ","findings":[]}`), nil
	}}
	runner := &NativeRunner{agents: agents}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := runner.runPanelChairman(context.Background(), chairmanRequest(), roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
	if errText != "" || approvals != len(analysis.Reports) || len(feedback.Findings) != 0 || len(agents.requests) != 1 {
		t.Fatalf("chairman approval failed: approvals=%d err=%q feedback=%+v calls=%d", approvals, errText, feedback, len(agents.requests))
	}
}

func TestChairmanReceivesFrozenDiffEvidenceContract(t *testing.T) {
	req := chairmanRequest()
	artifact := req.Inputs["src"]
	artifact.Type = "frozen_diff"
	req.Inputs["src"] = artifact
	agents := &discussionTestAgents{respond: func(request DelegateRequest) (string, error) {
		if !strings.Contains(request.Prompt, "patch does not embed those logs or metadata") ||
			!strings.Contains(request.Prompt, "use its tools to verify a material operational requirement") {
			t.Fatalf("chairman did not receive frozen-diff evidence contract: %s", request.Prompt)
		}
		return fmt.Sprintf(`{"run_id":%q,"artifact_hash":%q,"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`, req.WorkItem.ID, artifact.Hash), nil
	}}
	runner := &NativeRunner{agents: agents}
	analysis := discussionAnalysis("blocking")
	if _, approvals, _, _, errText, _ := runner.runPanelChairman(context.Background(), req, roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "frozen_diff"); errText != "" || approvals != len(analysis.Reports) {
		t.Fatalf("chairman approval failed: approvals=%d err=%q", approvals, errText)
	}
}

func TestChairmanChangesReceiveStableFinalIDs(t *testing.T) {
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"direction is right"},"verdict":"changes","findings":[{"id":"raw","severity":"foundational","location":"design","summary":"architecture cannot work","recommendation":"replace it"}]}`), nil
	}}
	runner := &NativeRunner{agents: agents}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := runner.runPanelChairman(context.Background(), chairmanRequest(), roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
	if errText != "" || approvals != 0 || len(feedback.Findings) != 1 || !strings.HasPrefix(feedback.Findings[0].ID, "issue-") || feedback.Findings[0].Persona != "chairman" {
		t.Fatalf("chairman changes failed: approvals=%d err=%q feedback=%+v", approvals, errText, feedback)
	}
}

func TestChairmanDriftedChangesBecomeActionableFeedback(t *testing.T) {
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"drifted","summary":"the plan substitutes a different outcome"},"verdict":"changes","findings":[{"id":"scope","severity":"blocking","location":"objective","summary":"wrong outcome","recommendation":"restore the requested outcome"}]}`), nil
	}}
	runner := &NativeRunner{agents: agents}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := runner.runPanelChairman(context.Background(), chairmanRequest(), roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
	if errText != "" || approvals != 0 || len(feedback.Findings) != 2 {
		t.Fatalf("drifted changes did not reach refinement: approvals=%d err=%q feedback=%+v", approvals, errText, feedback)
	}
	if feedback.Findings[0].Persona != "chairman" || !strings.Contains(feedback.Findings[0].Summary, "alignment is drifted") {
		t.Fatalf("missing alignment feedback: %+v", feedback.Findings)
	}
}

func TestChairmanFailsClosedOnMalformedFinalVerdict(t *testing.T) {
	for _, response := range []string{
		chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"aligned"},"verdict":"approve","findings":[{"id":"x"}]}`),
		chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"drifted"},"verdict":"approve","findings":[]}`),
		chairmanResponse(`"artifact_stage":"intent","original_request_alignment":{"status":"aligned"},"verdict":"approve","findings":[]}`),
	} {
		agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) { return response, nil }}
		runner := &NativeRunner{agents: agents}
		analysis := discussionAnalysis("blocking")
		if _, _, _, _, errText, _ := runner.runPanelChairman(context.Background(), chairmanRequest(), roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan"); errText == "" {
			t.Fatalf("malformed chairman verdict passed: %s", response)
		}
	}
}

func TestChairmanRejectsAnotherRunsArtifactIdentity(t *testing.T) {
	req := chairmanRequest()
	agents := &discussionTestAgents{respond: func(DelegateRequest) (string, error) {
		return fmt.Sprintf(`{"run_id":"another-run","artifact_hash":%q,"artifact_stage":"plan","original_request_alignment":{"status":"aligned"},"verdict":"approve","findings":[]}`, req.Inputs["src"].Hash), nil
	}}
	runner := &NativeRunner{agents: agents}
	analysis := discussionAnalysis("blocking")
	if _, _, _, _, errText, _ := runner.runPanelChairman(context.Background(), req, roundtablecfg.Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan"); !strings.Contains(errText, "mismatched run or artifact identity") {
		t.Fatalf("stale chairman response was not rejected: %q", errText)
	}
}
