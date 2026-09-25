package memory

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
)

// These are proposed work ceilings, never a tool grant. The host must intersect
// them with its authenticated task/operator budgets before executing anything.
// Version 1 proposes canonical memory reads only, with zero external model cost.
type evidenceRecoveryBudget struct {
	MaxRounds         int `json:"max_rounds"`
	MaxItems          int `json:"max_new_items"`
	MaxTokens         int `json:"max_tokens"`
	MaxElapsedMS      int `json:"max_elapsed_ms"`
	MaxCostMicrounits int `json:"max_cost_microunits"`
}

func (b evidenceRecoveryBudget) valid() bool {
	return b.MaxRounds >= 0 && b.MaxRounds <= 1 && b.MaxItems >= 0 && b.MaxItems <= 16 && b.MaxTokens >= 0 && b.MaxTokens <= 4096 && b.MaxElapsedMS >= 0 && b.MaxElapsedMS <= 2000 && b.MaxCostMicrounits == 0
}
func (b *evidenceRecoveryBudget) UnmarshalJSON(raw []byte) error {
	var value evidenceRecoveryBudget
	if err := decodeEvidenceObject(raw, map[string]any{
		"max_rounds": &value.MaxRounds, "max_new_items": &value.MaxItems,
		"max_tokens": &value.MaxTokens, "max_elapsed_ms": &value.MaxElapsedMS,
		"max_cost_microunits": &value.MaxCostMicrounits,
	}); err != nil {
		return err
	}
	if !value.valid() {
		return fmt.Errorf("invalid evidence recovery budget")
	}
	*b = value
	return nil
}

type evidenceRecoveryAction struct {
	Role     string `json:"role"`
	Key      string `json:"attempt_key"`
	Kind     string `json:"kind"`
	Subject  string `json:"subject"`
	Relation string `json:"relation"`
}
type evidenceRecoveryGap struct {
	Subject  string `json:"subject"`
	Relation string `json:"relation"`
	Status   string `json:"status"`
	Reason   string `json:"reason"`
}
type evidenceRecoveryPlan struct {
	Execution         *evidenceRecoveryExecution `json:"execution,omitempty"`
	SchemaVersion     int                        `json:"schema_version"`
	PlannerVersion    string                     `json:"planner_version"`
	RequirementDigest string                     `json:"requirement_digest"`
	SelectionDigest   string                     `json:"selection_digest"`
	TaskRevision      string                     `json:"task_revision"`
	Authority         string                     `json:"authority"`
	State             string                     `json:"state"`
	Budget            evidenceRecoveryBudget     `json:"proposed_budget"`
	Actions           []evidenceRecoveryAction   `json:"actions"`
	Gaps              []evidenceRecoveryGap      `json:"remaining_gaps"`
}

// Regenerate after each packing boundary. A serialized plan is never imported as
// an admitted action. Missing roles may propose a narrowly scoped current-state
// read; dropped, conflicting or unavailable evidence cannot be repaired by blind
// query retries. Attempt identity is independent of query wording and packing so
// the host can deduplicate a task's repeated proposal before admitting any work.
func (r *typedContextResult) planEvidenceRecovery() {
	r.Recovery = nil
	p, c := r.Requirements, r.Coverage
	if p == nil || p.Recovery == nil || c == nil {
		return
	}
	plan := &evidenceRecoveryPlan{SchemaVersion: 1, PlannerVersion: "task-recovery-v2", RequirementDigest: c.RequirementDigest, SelectionDigest: r.SelectionDigest, TaskRevision: p.TaskRevision, Authority: "proposal_only", State: "no_action", Budget: *p.Recovery, Actions: []evidenceRecoveryAction{}, Gaps: []evidenceRecoveryGap{}}
	r.Recovery = plan
	plan.Execution = r.recoveryExecution
	if _, supported := p.expandedRoles(); !p.valid() || !supported {
		plan.State = "unsupported"
		return
	}
	enabled := p.Recovery.MaxRounds > 0 && p.Recovery.MaxItems > 0 && p.Recovery.MaxTokens > 0 && p.Recovery.MaxElapsedMS > 0
	for _, role := range c.Roles {
		if role.Optional || role.Status == "satisfied" {
			continue
		}
		gap := evidenceRecoveryGap{Subject: role.Subject, Relation: role.Relation, Status: role.Status}
		switch {
		case !enabled:
			gap.Reason = "recovery_budget_exhausted"
		case role.Status == "budget_dropped":
			gap.Reason = "packing_budget_requires_host_revision"
		case role.Status == "conflicted":
			gap.Reason = "conflict_requires_review"
		case role.Status != "missing":
			gap.Reason = "source_unavailable_or_unsupported"
		case len(plan.Actions) >= p.Recovery.MaxItems:
			gap.Reason = "recovery_item_budget_exhausted"
		default:
			identity, _ := json.Marshal([]string{"task-recovery-v2", p.TaskRevision, role.Subject, role.Relation, role.Role})
			kind := "lookup_current_role"
			switch role.Role {
			case "historical_predecessor", "temporal_anchor":
				kind = "lookup_source_chain"
			case "approved_procedure":
				kind = "lookup_reviewed_procedure"
			case "counterexample":
				kind = "expand_counterexample_span"
			}
			plan.Actions = append(plan.Actions, evidenceRecoveryAction{Key: fmt.Sprintf("sha256:%x", sha256.Sum256(identity)), Role: role.Role, Kind: kind, Subject: role.Subject, Relation: role.Relation})
			gap.Reason = "host_admission_required"
		}
		plan.Gaps = append(plan.Gaps, gap)
	}
	if len(plan.Actions) > 0 {
		plan.State = "awaiting_host_admission"
	} else if len(plan.Gaps) > 0 {
		plan.State = "blocked"
	}
}
