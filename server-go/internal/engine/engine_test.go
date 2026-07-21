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
		if req.Node.ID != "plan_gate" || string(req.Inputs["src"].Content) != r.firstPlan {
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
		if req.Node.ID != "plan_gate" || string(req.Inputs["src"].Content) != r.secondPlan {
			r.t.Fatal("gate did not receive the complete revised plan")
		}
		return StepResult{Status: StepAdvanced, ContentHash: req.Inputs["src"].Hash}, nil
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

type artifactRoutingRunner struct {
	t    *testing.T
	call int
}

func (r *artifactRoutingRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	r.call++
	switch req.Node.ID {
	case "freeze":
		return StepResult{Status: StepAdvanced, ArtifactType: "frozen_diff", Artifact: "complete diff"}, nil
	case "accept_gate":
		if got := string(req.Inputs["src"].Content); got != "complete diff" {
			r.t.Fatalf("acceptance gate reviewed %q, want complete diff", got)
		}
		return StepResult{Status: StepChanges, Feedback: &wfe.ReviewFeedback{Findings: []wfe.Finding{{
			ID: "fix", Persona: "qa", Severity: "blocking", Summary: "fix code", Recommendation: "revise",
		}}}}, nil
	default:
		r.t.Fatalf("unexpected node %q", req.Node.ID)
		return StepResult{}, nil
	}
}

func TestGateReviewsItsBoundArtifactNotThePlanShortcut(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: build
start: freeze
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: accept_gate
  - id: accept_gate
    block: gate.roundtable
    in: {src: freeze.out}
    on_pass: done
    on_fail: impl
  - id: done
    block: pr.open
    in: {src: freeze.out}
`)
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
	if err := artifacts.PutProposal("wi_artifacts", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_artifacts", "impl", "branch", []byte("head")); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutPlan("wi_artifacts", []byte("unrelated plan")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{ID: "wi_artifacts", Repo: "repo", ProposalPath: "p", WorkflowName: "build", WorkflowVersion: def.Version, StartStage: "freeze"}); err != nil {
		t.Fatal(err)
	}
	runner := &artifactRoutingRunner{t: t}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := eng.Advance(context.Background(), "wi_artifacts"); err != nil {
		t.Fatal(err)
	}
	out, err := eng.Advance(context.Background(), "wi_artifacts")
	if err != nil {
		t.Fatal(err)
	}
	if out.NextStage != "impl" || out.Parked {
		t.Fatalf("review transition=%+v", out)
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
    block: gate.deliver
    in: {verdict: plan_gate.out}
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
	if _, err := artifacts.PutNodeArtifact("wi_e2e", "source", "proposal", []byte(proposal)); err != nil {
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
