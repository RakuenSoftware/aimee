package memory

import (
	"encoding/json"
	"testing"
)

func TestExecutionGradeSourceReleaseFreshnessAndReplay(t *testing.T) {
	for _, scenario := range []string{"same", "erased", "changed", "foreign_actor", "missing", "wrong_check", "replay"} {
		t.Run(scenario, func(t *testing.T) {
			state := &sourceReleaseState{}
			args := sourceReleaseArgs(map[string]any{"request_id": "request", "principal": "caller", "project": "app"})
			grades := json.RawMessage(`{"schema_version":1,"principal":"actor","scope_kind":"project","scope_id":"app","coverage":"recent_bounded","items":[{"source":{"event_id":"terminal"},"verification_ref":"proof"}]}`)
			token, err := state.prepare(args, map[string]any{"execution_test_grades": grades})
			if err != nil || token == "" {
				t.Fatal(token, err)
			}
			args["source_release_ticket"], _ = json.Marshal(token)
			args["operation"] = json.RawMessage(`"source-release-plan"`)
			plan := sourceReleaseCall(t, state, args)
			if plan["execution_grade_request"] == nil {
				t.Fatal(plan)
			}
			var fresh map[string]any
			_ = json.Unmarshal(grades, &fresh)
			fresh["check_id"] = plan["execution_grade_request"].(map[string]any)["check_id"]
			answer, _ := json.Marshal(fresh)
			switch scenario {
			case "erased":
				answer = json.RawMessage(`{"schema_version":1,"principal":"actor","scope_kind":"project","scope_id":"app","coverage":"recent_bounded","items":[]}`)
			case "changed":
				answer = json.RawMessage(`{"schema_version":1,"principal":"actor","scope_kind":"project","scope_id":"app","coverage":"recent_bounded","items":[{"source":{"event_id":"changed"},"verification_ref":"proof"}]}`)
			case "foreign_actor":
				answer = json.RawMessage(`{"schema_version":1,"principal":"foreign","scope_kind":"project","scope_id":"app","coverage":"recent_bounded","items":[{"source":{"event_id":"terminal"},"verification_ref":"proof"}]}`)
			case "wrong_check":
				fresh["check_id"] = "old-attempt"
				answer, _ = json.Marshal(fresh)
			case "missing":
				answer = nil
			}
			args["operation"] = json.RawMessage(`"source-release-result"`)
			args["execution_grade_response"] = answer
			result := sourceReleaseCall(t, state, args)
			if scenario == "same" || scenario == "replay" {
				if result["admitted"] != true {
					t.Fatal(result)
				}
				if again := sourceReleaseCall(t, state, args); again["admitted"] == true {
					t.Fatal("replayed admission", again)
				}
			} else if result["admitted"] == true {
				t.Fatal("stale source admitted", result)
			}
		})
	}
}

func TestExecutionGradePackingRetainsOnlyVisibleReceipts(t *testing.T) {
	raw := json.RawMessage(`{"schema_version":1,"principal":"actor","scope_kind":"project","scope_id":"app","coverage":"recent_bounded","items":[{"id":"first"},{"id":"second"}]}`)
	got := retainedExecutionGrades(raw, []int{1, 5}, 2)
	var snapshot executionGradeSnapshot
	if json.Unmarshal(got, &snapshot) != nil || len(snapshot.Items) != 1 || string(snapshot.Items[0]) != `{"id":"second"}` {
		t.Fatal(string(got))
	}
	if result := retainedExecutionGrades(raw, []int{5}, 2); len(result) != 0 {
		t.Fatal(string(result))
	}
}

func TestConfiguredTestProviderReceiptNamesCoverage(t *testing.T) {
	state := &sourceReleaseState{}
	args := receiptTestAdmission(t, state)
	var ticket string
	if json.Unmarshal(args["source_release_ticket"], &ticket) != nil {
		t.Fatal("ticket")
	}
	state.entries[ticket].executionGrades = json.RawMessage(`{"schema_version":1,"principal":"operator","scope_kind":"project","scope_id":"private","coverage":"recent_bounded","items":[{"source":{"event_id":"terminal"},"verification_ref":"proof"}]}`)
	result := sourceReleaseCall(t, state, args)
	detail, ok := result["prepared_detail"].(string)
	if !ok {
		t.Fatal(result)
	}
	prepared, valid := decodePreparedReceipt([]byte(detail))
	if !valid || prepared.Binding.SourceCoverage != "retained_inputs_with_configured_tests" || len(prepared.Binding.ConfiguredTests) == 0 {
		t.Fatal("unidentified receipt input", valid, detail)
	}
}
