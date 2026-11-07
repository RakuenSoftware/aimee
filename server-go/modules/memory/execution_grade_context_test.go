package memory

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestConfiguredTestHistoryNextRunAssembly(t *testing.T) {
	snapshot := map[string]any{"schema_version": 1, "principal": "operator", "scope_kind": "project", "scope_id": "project-a", "coverage": "recent_bounded", "items": []any{map[string]any{"qualification": "configured_tests_only", "verification": "tests_bound_revision", "verdict": "failed", "source": map[string]any{"actor": "operator", "event_id": "terminal"}, "proof": map[string]any{"scope_kind": "project", "scope_id": "project-a", "snapshot_commit": strings.Repeat("a", 40)}}}}
	raw, _ := json.Marshal(snapshot)
	result, err := ingressAssemble(ingressAssemblyRequest{Project: "project-a", ExecutionGrades: raw, Budget: 4096})
	if err != nil {
		t.Fatal(err)
	}
	envelope, _ := result["envelope"].(string)
	if !strings.Contains(envelope, "configured-test-history") || !strings.Contains(envelope, `"verdict":"failed"`) || strings.Contains(envelope, "approved_procedures") {
		t.Fatal(envelope)
	}
	if _, err = ingressAssemble(ingressAssemblyRequest{Project: "foreign", ExecutionGrades: raw}); err == nil {
		t.Fatal("foreign project accepted")
	}
	snapshot["principal"] = "foreign"
	raw, _ = json.Marshal(snapshot)
	if _, err = executionGradeContexts(raw, "project-a"); err == nil {
		t.Fatal("foreign actor accepted")
	}
}

func TestDelegateExperienceRemainsScopedDataWithoutProcedureAuthority(t *testing.T) {
	experience := map[string]any{"principal": "operator", "qualification": "configured_tests_only", "scope_kind": "project", "scope_id": "project-a", "verdict": "passed", "action_coverage": "bounded_canonical_tool_calls", "source": map[string]any{"actor": "operator"}, "parent_source": map[string]any{"actor": "operator"}, "actions": []any{map[string]any{"name": "git_verify", "input_sha256": strings.Repeat("a", 64)}}}
	grade := map[string]any{"qualification": "configured_tests_only", "verification": "tests_bound_revision", "verdict": "passed", "source": map[string]any{"actor": "operator"}, "proof": map[string]any{"scope_kind": "project", "scope_id": "project-a"}, "delegate_experience": experience}
	snapshot := map[string]any{"schema_version": 1, "principal": "operator", "scope_kind": "project", "scope_id": "project-a", "coverage": "recent_bounded", "items": []any{grade}}
	raw, _ := json.Marshal(snapshot)
	context, err := executionGradeContexts(raw, "project-a")
	if err != nil || len(context) != 1 || !strings.Contains(context[0], `authorization="none"`) || !strings.Contains(context[0], `trust="data"`) || !strings.Contains(context[0], "git_verify") || strings.Contains(context[0], "approved_procedures") {
		t.Fatal("observational context", err, context)
	}
	for key, value := range map[string]any{"principal": "foreign", "scope_id": "foreign-project", "qualification": "semantic_success", "verdict": "failed", "action_coverage": "applied_procedure"} {
		old := experience[key]
		experience[key] = value
		raw, _ = json.Marshal(snapshot)
		if _, err := executionGradeContexts(raw, "project-a"); err == nil {
			t.Fatal("unbound or causal claim admitted", key)
		}
		experience[key] = old
	}
	experience["parent_source"] = map[string]any{"actor": "foreign"}
	raw, _ = json.Marshal(snapshot)
	if _, err := executionGradeContexts(raw, "project-a"); err == nil {
		t.Fatal("foreign parent actor admitted")
	}
}
