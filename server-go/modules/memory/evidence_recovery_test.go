package memory

import (
	"encoding/json"
	"strings"
	"testing"
)

func recoveryFixture(t *testing.T) *typedContextResult {
	r := coverageFixture(t)
	r.Requirements.Recovery = &evidenceRecoveryBudget{MaxRounds: 1, MaxItems: 2, MaxTokens: 512, MaxElapsedMS: 200}
	return r
}
func TestEvidenceRecoveryProposesOnlyMissingRequiredRoles(t *testing.T) {
	r := recoveryFixture(t)
	r.Requirements.Obligations = append(r.Requirements.Obligations,
		evidenceObligation{Subject: "optional", Relation: "uses", Optional: true},
		evidenceObligation{Subject: "database", Relation: "uses"})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	p := r.Recovery
	if p == nil || p.State != "awaiting_host_admission" || p.Authority != "proposal_only" || len(p.Actions) != 2 || len(p.Gaps) != 2 || r.Sufficiency != "insufficient" {
		t.Fatal(p)
	}
	if p.RequirementDigest != r.Coverage.RequirementDigest || p.SelectionDigest != r.SelectionDigest {
		t.Fatal("unbound plan", p)
	}
	key := p.Actions[0].Key
	r.Requirements.Recovery.MaxItems = 1
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if len(r.Recovery.Actions) != 1 || r.Recovery.Actions[0].Key != key || r.Recovery.Gaps[1].Reason != "recovery_item_budget_exhausted" {
		t.Fatal(r.Recovery)
	}
	// Packing changes cannot reset attempt identity and invite duplicate work.
	r.add("current_assertions", coverageHit(50, "unrelated", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Recovery.Actions[0].Key != key {
		t.Fatal("packing reset attempt key")
	}
	r.Requirements.TaskRevision = "task:8"
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Recovery.Actions[0].Key == key {
		t.Fatal("new task revision reused attempt key")
	}
}
func TestEvidenceRecoveryDoesNotRetryBlockedEvidence(t *testing.T) {
	for _, tc := range []struct {
		name   string
		change func(*typedContextResult)
		reason string
	}{
		{"dropped", func(r *typedContextResult) {
			r.Channels["current_assertions"].Budget = 0
			r.add("current_assertions", coverageHit(1, "service", "cache"))
		}, "packing_budget_requires_host_revision"},
		{"conflicting", func(r *typedContextResult) {
			r.add("current_assertions", coverageHit(1, "service", "cache"))
			r.add("current_assertions", coverageHit(2, "service", "database"))
		}, "conflict_requires_review"},
		{"unavailable", func(r *typedContextResult) { r.fail("current_assertions", "offline") }, "source_unavailable_or_unsupported"},
		{"rounds", func(r *typedContextResult) { r.Requirements.Recovery.MaxRounds = 0 }, "recovery_budget_exhausted"},
		{"items", func(r *typedContextResult) { r.Requirements.Recovery.MaxItems = 0 }, "recovery_budget_exhausted"},
		{"tokens", func(r *typedContextResult) { r.Requirements.Recovery.MaxTokens = 0 }, "recovery_budget_exhausted"},
		{"time", func(r *typedContextResult) { r.Requirements.Recovery.MaxElapsedMS = 0 }, "recovery_budget_exhausted"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := recoveryFixture(t)
			tc.change(r)
			if err := r.finish(); err != nil {
				t.Fatal(err)
			}
			if r.Recovery.State != "blocked" || len(r.Recovery.Actions) != 0 || r.Recovery.Gaps[0].Reason != tc.reason {
				t.Fatal(r.Recovery)
			}
		})
	}
	r := recoveryFixture(t)
	r.add("current_assertions", coverageHit(1, "service", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Recovery.State != "no_action" || len(r.Recovery.Gaps) != 0 {
		t.Fatal(r.Recovery)
	}
	raw, _ := json.Marshal(r)
	zero := 0
	out, err := ingressAssemble(ingressAssemblyRequest{Budget: 1000, TypedRequested: true, TypedContextJSON: string(raw), ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &zero}})
	if err != nil {
		t.Fatal(err)
	}
	plan := out["typed_projection"].(map[string]any)["evidence_recovery"].(*evidenceRecoveryPlan)
	if plan.State != "blocked" || len(plan.Actions) != 0 || plan.Gaps[0].Reason != "packing_budget_requires_host_revision" {
		t.Fatal("outer packing reused stale plan", plan)
	}
}
func TestEvidenceRecoveryBudgetAdmission(t *testing.T) {
	for _, budget := range []string{`null`, `{"max_rounds":2}`, `{"max_new_items":17}`, `{"max_tokens":4097}`, `{"max_elapsed_ms":2001}`, `{"max_cost_microunits":1}`, `{"max_rounds":-1}`, `{"max_tokens":null}`, `{"MaxRounds":1}`, `{"max_rounds":0,"max_rounds":1}`, `{"execute":true}`} {
		raw := `{"schema_version":1,"task_revision":"7","query_mode":"current_state","obligations":[{"subject":"service","relation":"uses"}],"recovery_budget":` + budget + `}`
		if _, err := decodeEvidenceRequirements([]byte(raw)); err == nil {
			t.Fatal("accepted unsafe recovery budget", budget)
		}
	}
	r := coverageFixture(t)
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(r)
	if r.Recovery != nil || strings.Contains(string(raw), `"evidence_recovery"`) {
		t.Fatal("recovery enabled without opt-in")
	}
	r = recoveryFixture(t)
	r.Requirements.QueryMode = "timeline"
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Recovery.State != "unsupported" || len(r.Recovery.Actions) != 0 || r.Sufficiency != "unknown" {
		t.Fatal(r.Recovery)
	}
}
