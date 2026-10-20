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
