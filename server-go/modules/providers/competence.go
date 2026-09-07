package providers

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
)

// Competence is evidence about a particular task role, independent of pricing,
// model size and context capacity. No wildcard evidence is accepted.
func validateCompetence(model object) error {
	v, present := model["competence"]
	if !present {
		return nil
	}
	// A persisted assessment cannot follow a later model replacement. The
	// first declaration binds to the current model; explicit reassessment can
	// replace that binding through model.set --competence.
	if bound := str(model, "competence_model"); bound != "" && bound != str(model, "model") {
		delete(model, "competence")
		delete(model, "competence_model")
		return nil
	}
	model["competence_model"] = str(model, "model")
	entries, ok := v.(map[string]any)
	if !ok || len(entries) > 16 {
		return fmt.Errorf("competence must contain at most 16 role assessments")
	}
	for role, value := range entries {
		entry, ok := value.(map[string]any)
		if !validRole(role) || !ok || !scoreValid(entry["score"], 0) ||
			str(entry, "source") == "" || !validText(str(entry, "source"), 256) {
			return fmt.Errorf("competence.%s requires an integer score 0..100 and an evidence source", role)
		}
	}
	return nil
}

func validRole(role string) bool { return role != "" && role != "all" && validText(role, 32) }
func scoreValid(v any, min float64) bool {
	var n float64
	switch v := v.(type) {
	case float64:
		n = v
	case int:
		n = float64(v)
	case json.Number:
		var err error
		n, err = v.Float64()
		if err != nil {
			return false
		}
	default:
		return false
	}
	return !math.IsNaN(n) && !math.IsInf(n, 0) && n >= min && n <= 100 && math.Trunc(n) == n
}

func validateRoleContracts(root object) error {
	v, present := root["role_contracts"]
	if !present {
		return nil
	}
	contracts, ok := v.(map[string]any)
	if !ok || len(contracts) > 16 {
		return fmt.Errorf("role_contracts must contain at most 16 roles")
	}
	for role, value := range contracts {
		entry, ok := value.(map[string]any)
		if !validRole(role) || !ok || !scoreValid(entry["min_competence"], 1) {
			return fmt.Errorf("role_contracts.%s requires min_competence 1..100", role)
		}
	}
	for _, model := range rows(root, "models") {
		assessments, _ := model["competence"].(map[string]any)
		roles := map[string]bool{}
		for role := range contracts {
			roles[role] = true
		}
		for role := range assessments {
			roles[role] = true
		}
		if len(roles) > 16 {
			return fmt.Errorf("combined competence and contract roles exceed runtime capacity")
		}
	}
	return nil
}

// Derived facts cross the native ABI, while original evidence and contracts
// remain owned by the Go store. Native snapshot saves preserve both unchanged.
func applyRoleContracts(root object) {
	contracts, _ := root["role_contracts"].(map[string]any)
	for _, model := range rows(root, "models") {
		assessments, _ := model["competence"].(map[string]any)
		roles := map[string]bool{}
		for role := range contracts {
			roles[role] = true
		}
		for role := range assessments {
			roles[role] = true
		}
		names := make([]string, 0, len(roles))
		for role := range roles {
			names = append(names, role)
		}
		sort.Strings(names)
		facts := []object{}
		for _, role := range names {
			assessment, _ := assessments[role].(map[string]any)
			contract, _ := contracts[role].(map[string]any)
			score, floor := number(assessment, "score"), number(contract, "min_competence")
			reason := "qualified"
			if score < floor {
				reason = "competence_below_threshold"
				if assessment == nil {
					reason = "competence_unknown"
				}
			}
			facts = append(facts, object{"role": role, "score": score, "minimum": floor,
				"eligible": score >= floor, "reason": reason})
		}
		model["routing_competence"] = facts
	}
}
