package memory

import (
	"encoding/json"
	"os"
	"reflect"
	"sort"
	"testing"
)

// Gold answers are confined to this evaluator. The serving requirement set
// receives only task roles/subjects/relations, never an expected record or answer.
// Reader accuracy is a deliberately separate, narrow deterministic baseline; it
// does not purport to measure a model's answer quality or planner completeness.
func TestMR05FrozenCoverageEvaluation(t *testing.T) {
	raw, err := os.ReadFile("../../../tests/eval/memory_mr05_coverage_cases.json")
	if err != nil {
		t.Fatal(err)
	}
	var corpus struct {
		Cases []struct {
			Name, Mode, Scenario string
			Status               string   `json:"expected_status"`
			Incomplete           bool     `json:"intentionally_incomplete"`
			Answers              []string `json:"answer_objects"`
		}
	}
	if err = json.Unmarshal(raw, &corpus); err != nil || len(corpus.Cases) != 11 {
		t.Fatal(err)
	}
	falseComplete, incomplete, answerCorrect := 0, 0, 0
	for _, test := range corpus.Cases {
		t.Run(test.Name, func(t *testing.T) {
			r, old, current := temporalCoverageFixture(t)
			r.Requirements.QueryMode = test.Mode
			if test.Mode == "comparison" {
				r.Requirements.Obligations = append(r.Requirements.Obligations, evidenceObligation{Subject: "other-service", Relation: "uses"})
			}
			switch test.Scenario {
			case "unrelated":
				current = coverageHit(11, "unrelated", "high-confidence")
			case "unrelated-predecessor":
				h := current.value.(assertionHit)
				h.PriorVersionID = "999"
				current.value = h
			case "unknown-date":
				h := current.value.(assertionHit)
				h.ValidFrom = ""
				current.value = h
			case "dropped-update":
				r.Channels["current_assertions"].Budget = 0
			case "unavailable":
				r.fail("current_assertions", "offline")
			case "revoked":
				h := current.value.(assertionHit)
				h.Lifecycle = "revoked"
				current.value = h
			case "copies":
				r.Requirements.Obligations[0].Role = "independent_support"
				r.Requirements.Obligations[0].MinIndependent = 2
				for i := int64(20); i < 50; i++ {
					r.add("current_assertions", coverageHit(i, "service", "new-database"))
				}
			case "conflicted":
				r.add("current_assertions", coverageHit(12, "service", "conflicting-database"))
			case "current", "transition":
			default:
				t.Fatal("unhandled frozen fixture")
			}
			r.add("current_assertions", current)
			if test.Mode == "temporal_change" {
				r.add("historical_assertions", old)
			}
			if err := r.finish(); err != nil {
				t.Fatal(err)
			}
			if r.Sufficiency != test.Status {
				t.Fatal(r.Sufficiency, r.Coverage)
			}
			if test.Incomplete {
				incomplete++
				if r.Sufficiency == "complete" {
					falseComplete++
				}
			}
			set := map[string]bool{}
			for _, item := range r.Channels["current_assertions"].selected {
				h, ok := coverageAssertion(item)
				if ok {
					set[h.Object] = true
				}
			}
			actual := []string{}
			for value := range set {
				actual = append(actual, value)
			}
			sort.Strings(actual)
			sort.Strings(test.Answers)
			if reflect.DeepEqual(actual, test.Answers) {
				answerCorrect++
			}
		})
	}
	if falseComplete != 0 || incomplete != 9 {
		t.Fatal("false complete gate", falseComplete, incomplete)
	}
	t.Logf("false_complete=%d/%d; deterministic_reader_answer_set_accuracy=%d/%d; model_answer_quality=not_measured", falseComplete, incomplete, answerCorrect, len(corpus.Cases))
}
