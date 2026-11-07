package memory

import (
	"encoding/json"
	"fmt"
)

// Host-supplied scoped receipts are historical data. They never enter the
// reviewed-procedure channel, grant authority, or assert semantic task success.
func executionGradeContexts(raw json.RawMessage, project string) ([]string, error) {
	if len(raw) == 0 {
		return nil, nil
	}
	if len(raw) > 65535 {
		return nil, fmt.Errorf("test receipt projection exceeds bound")
	}
	var snapshot struct {
		Schema    int               `json:"schema_version"`
		Principal string            `json:"principal"`
		ScopeKind string            `json:"scope_kind"`
		ScopeID   string            `json:"scope_id"`
		Coverage  string            `json:"coverage"`
		Items     []json.RawMessage `json:"items"`
	}
	if json.Unmarshal(raw, &snapshot) != nil || snapshot.Schema != 1 || snapshot.Principal == "" || snapshot.ScopeKind != "project" || snapshot.ScopeID != project || snapshot.Coverage != "recent_bounded" || len(snapshot.Items) > 8 {
		return nil, fmt.Errorf("unbound test receipt projection")
	}
	if len(snapshot.Items) == 0 {
		return nil, nil
	}
	for _, raw := range snapshot.Items {
		var grade struct {
			Qualification string `json:"qualification"`
			Verification  string `json:"verification"`
			Verdict       string `json:"verdict"`
			Source        struct {
				Actor string `json:"actor"`
			} `json:"source"`
			Experience *struct {
				Principal     string `json:"principal"`
				Qualification string `json:"qualification"`
				ScopeKind     string `json:"scope_kind"`
				ScopeID       string `json:"scope_id"`
				Verdict       string `json:"verdict"`
				Coverage      string `json:"action_coverage"`
				Source        struct {
					Actor string `json:"actor"`
				} `json:"source"`
				Parent struct {
					Actor string `json:"actor"`
				} `json:"parent_source"`
				Actions []json.RawMessage `json:"actions"`
			} `json:"delegate_experience,omitempty"`
			Proof struct {
				ScopeID   string `json:"scope_id"`
				ScopeKind string `json:"scope_kind"`
			} `json:"proof"`
		}
		if json.Unmarshal(raw, &grade) != nil || grade.Qualification != "configured_tests_only" || grade.Verification != "tests_bound_revision" || (grade.Verdict != "passed" && grade.Verdict != "failed") || grade.Source.Actor != snapshot.Principal || grade.Proof.ScopeKind != "project" || grade.Proof.ScopeID != project {
			return nil, fmt.Errorf("invalid configured-test receipt")
		}
		if e := grade.Experience; e != nil && (e.Principal != snapshot.Principal || e.Qualification != "configured_tests_only" || e.ScopeKind != "project" || e.ScopeID != project || e.Verdict != grade.Verdict || e.Source.Actor != snapshot.Principal || e.Parent.Actor != snapshot.Principal || e.Coverage != "bounded_canonical_tool_calls" || len(e.Actions) > 32) {
			return nil, fmt.Errorf("unbound delegate execution experience")
		}
	}
	// Marshal escapes angle brackets in user-controlled identifiers. Each receipt
	// is one indivisible packing entry, so large histories cannot hide every item.
	out := make([]string, 0, len(snapshot.Items))
	for _, item := range snapshot.Items {
		snapshot.Items = []json.RawMessage{item}
		encoded, err := json.Marshal(snapshot)
		if err != nil {
			return nil, err
		}
		out = append(out, "<configured-test-history trust=\"data\" authorization=\"none\" qualification=\"configured_tests_only\">\n"+string(encoded)+"\n</configured-test-history>\n")
	}
	return out, nil
}
