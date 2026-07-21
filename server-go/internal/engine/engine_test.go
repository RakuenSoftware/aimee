package engine

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type scriptedRunner struct {
	t          *testing.T
	call       int
	proposal   string
	firstPlan  string
	secondPlan string
	feedback   wfe.ReviewFeedback
}

func (r *scriptedRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	r.call++
	if req.Proposal != r.proposal {
		r.t.Fatal("runner did not receive the complete proposal")
	}
	switch r.call {
	case 1:
		if req.Node.ID != "plan" {
			r.t.Fatalf("first node=%q", req.Node.ID)
		}
		return StepResult{Status: StepAdvanced, Artifact: r.firstPlan}, nil
	case 2:
		if req.Node.ID != "plan_gate" || req.Plan != r.firstPlan {
			r.t.Fatal("gate did not receive the complete first plan")
		}
		return StepResult{Status: StepChanges, Feedback: &r.feedback}, nil
	case 3:
		if req.Node.ID != "plan" || req.Feedback == nil ||
			req.Feedback.Findings[0].Recommendation != r.feedback.Findings[0].Recommendation {
			r.t.Fatal("refining planner did not receive complete structured feedback")
		}
		return StepResult{Status: StepAdvanced, Artifact: r.secondPlan}, nil
	case 4:
		if req.Node.ID != "plan_gate" || req.Plan != r.secondPlan {
			r.t.Fatal("gate did not receive the complete revised plan")
		}
		return StepResult{Status: StepAdvanced, ContentHash: wfe.Hash([]byte(req.Plan))}, nil
	case 5:
		if req.Node.ID != "done" {
			r.t.Fatalf("terminal node=%q", req.Node.ID)
		}
		return StepResult{Status: StepAdvanced}, nil
	default:
		r.t.Fatalf("unexpected runner call %d", r.call)
		return StepResult{}, nil
	}
}

func TestPlanGateRefinementIsLosslessEndToEnd(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`
name: build
start: plan
nodes:
  - id: plan
    block: author.plan
    in: {proposal: source.out}
    next: plan_gate
  - id: source
    block: author.proposal
    next: plan
  - id: plan_gate
    block: gate.roundtable
    in: {src: plan.out}
    params: {max_iters: 24}
    on_pass: done
    on_fail: plan
  - id: done
    block: pr.open
    in: {src: plan.out}
`)
	// The graph validator allows a producer before/after its consumer; execution
	// starts at plan because this test seeds an already-approved proposal.
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	proposal := strings.Repeat("proposal criterion λ\n", 120_000) + "PROPOSAL_END"
	firstPlan := strings.Repeat("initial plan detail 漢字\n", 120_000) + "FIRST_PLAN_END"
	secondPlan := strings.Repeat("revised plan detail 🚀\n", 120_000) + "SECOND_PLAN_END"
	feedback := wfe.ReviewFeedback{SchemaVersion: 1, Findings: []wfe.Finding{{
		ID: "complete-tail", Persona: "qa", Severity: "blocking",
		Summary:        strings.Repeat("observed; ", 100_000) + "SUMMARY_END",
		Recommendation: strings.Repeat("repair; ", 100_000) + "RECOMMENDATION_END",
	}}}
	if err := artifacts.PutProposal("wi_e2e", []byte(proposal)); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
		ID: "wi_e2e", Repo: "repo", ProposalPath: "imported", WorkflowName: "build",
		WorkflowVersion: def.Version, StartStage: "plan", Mode: "autonomous",
	}); err != nil {
		t.Fatal(err)
	}
	runner := &scriptedRunner{t: t, proposal: proposal, firstPlan: firstPlan,
		secondPlan: secondPlan, feedback: feedback}
	engine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		if _, err := engine.Advance(context.Background(), "wi_e2e"); err != nil {
			t.Fatalf("advance %d: %v", i+1, err)
		}
	}
	item, err := store.WorkItem(context.Background(), "wi_e2e")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "accepted" || runner.call != 5 {
		t.Fatalf("item=%+v calls=%d", item, runner.call)
	}
}
