package panel

import (
	"context"
	"fmt"
	"strings"
	"testing"
)

func chairmanRun() Run {
	artifact := Artifact{Stage: "plan", Content: "complete plan"}
	artifact.Hash = Hash([]byte(artifact.Content))
	return Run{ID: "wi", Stage: "gate", Workdir: "/worktree",
		OriginalRequest: "fix the scheduler", Reviewed: artifact}
}

func chairmanResponse(body string) string {
	run := chairmanRun()
	return fmt.Sprintf(`{"run_id":%q,"artifact_hash":%q,%s`, run.ID, run.Reviewed.Hash, body)
}

func TestChairmanSubmitsFinalApprovalAfterDeterministicSynthesis(t *testing.T) {
	wantHash := chairmanRun().Reviewed.Hash
	agents := &discussionTestAgents{respond: func(request SeatRequest) (string, error) {
		if request.Selector != "codex" || request.Persona != "chairman" || request.MaxTurnsCap != delegateMaxTurnsCap || !strings.Contains(request.DurableSlot, ":chairman") || !strings.Contains(request.Prompt, "BEGIN_CHAIRMAN_DATA") || !strings.Contains(request.Prompt, "plurality, format, or existence is never original-request drift") || !strings.Contains(request.Prompt, `"artifact_hash":"`+wantHash+`"`) {
			t.Fatalf("chairman request=%+v", request)
		}
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":" Approve ","findings":[]}`), nil
	}}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := RunChairman(context.Background(), agents, chairmanRun(), Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
	if errText != "" || approvals != len(analysis.Reports) || len(feedback.Findings) != 0 || len(agents.requests) != 1 {
		t.Fatalf("chairman approval failed: approvals=%d err=%q feedback=%+v calls=%d", approvals, errText, feedback, len(agents.requests))
	}
}

func TestChairmanReceivesFrozenDiffEvidenceContract(t *testing.T) {
	run := chairmanRun()
	run.Reviewed.Stage = "frozen_diff"
	artifact := run.Reviewed
	agents := &discussionTestAgents{respond: func(request SeatRequest) (string, error) {
		if !strings.Contains(request.Prompt, "patch does not embed those logs or metadata") ||
			!strings.Contains(request.Prompt, "use its tools to verify a material operational requirement") {
			t.Fatalf("chairman did not receive frozen-diff evidence contract: %s", request.Prompt)
		}
		return fmt.Sprintf(`{"run_id":%q,"artifact_hash":%q,"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`, run.ID, artifact.Hash), nil
	}}
	analysis := discussionAnalysis("blocking")
	if _, approvals, _, _, errText, _ := RunChairman(context.Background(), agents, run, Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "frozen_diff"); errText != "" || approvals != len(analysis.Reports) {
		t.Fatalf("chairman approval failed: approvals=%d err=%q", approvals, errText)
	}
}

func TestChairmanChangesReceiveStableFinalIDs(t *testing.T) {
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"direction is right"},"verdict":"changes","findings":[{"id":"raw","severity":"foundational","location":"design","summary":"architecture cannot work","recommendation":"replace it"}]}`), nil
	}}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := RunChairman(context.Background(), agents, chairmanRun(), Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
	if errText != "" || approvals != 0 || len(feedback.Findings) != 1 || !strings.HasPrefix(feedback.Findings[0].ID, "issue-") || feedback.Findings[0].Persona != "chairman" {
		t.Fatalf("chairman changes failed: approvals=%d err=%q feedback=%+v", approvals, errText, feedback)
	}
}

func TestChairmanDriftedChangesBecomeActionableFeedback(t *testing.T) {
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return chairmanResponse(`"artifact_stage":"plan","original_request_alignment":{"status":"drifted","summary":"the plan substitutes a different outcome"},"verdict":"changes","findings":[{"id":"scope","severity":"blocking","location":"objective","summary":"wrong outcome","recommendation":"restore the requested outcome"}]}`), nil
	}}
	analysis := discussionAnalysis("blocking")
	feedback, approvals, _, _, errText, _ := RunChairman(context.Background(), agents, chairmanRun(), Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan")
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
		agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) { return response, nil }}
		analysis := discussionAnalysis("blocking")
		if _, _, _, _, errText, _ := RunChairman(context.Background(), agents, chairmanRun(), Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan"); errText == "" {
			t.Fatalf("malformed chairman verdict passed: %s", response)
		}
	}
}

func TestChairmanRejectsAnotherRunsArtifactIdentity(t *testing.T) {
	run := chairmanRun()
	agents := &discussionTestAgents{respond: func(SeatRequest) (string, error) {
		return fmt.Sprintf(`{"run_id":"another-run","artifact_hash":%q,"artifact_stage":"plan","original_request_alignment":{"status":"aligned"},"verdict":"approve","findings":[]}`, run.Reviewed.Hash), nil
	}}
	analysis := discussionAnalysis("blocking")
	if _, _, _, _, errText, _ := RunChairman(context.Background(), agents, run, Panel{ChairmanEnabled: true, Chairman: "codex"}, analysis, analysis.Feedback, 0, false, "plan"); !strings.Contains(errText, "mismatched run or artifact identity") {
		t.Fatalf("stale chairman response was not rejected: %q", errText)
	}
}
