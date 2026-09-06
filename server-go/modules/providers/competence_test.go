package providers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestCompetencePersistsAndNativeSavePreservesEvidence(t *testing.T) {
	m, home, _ := manager(t)
	raw := `{"role_contracts":{"code":{"min_competence":70},"review":{"min_competence":90}},"models":[
 {"name":"free","model":"m","auth_type":"none","roles":["all"],"price_in_per_mtok":0,"price_out_per_mtok":0,"competence":{"code":{"score":80,"source":"operator: bounded edits validated"}}},
 {"name":"unknown","model":"m","auth_type":"none","roles":["all"]}]}`
	if err := os.WriteFile(filepath.Join(home, "models.json"), []byte(raw), 0600); err != nil {
		t.Fatal(err)
	}
	snapshot := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	models := rows(snapshot, "models")
	facts := rows(models[0], "routing_competence")
	if len(facts) != 2 || !boolean(facts[0], "eligible", false) || boolean(facts[1], "eligible", true) || str(facts[1], "reason") != "competence_unknown" {
		t.Fatal(facts)
	}
	for _, fact := range rows(models[1], "routing_competence") {
		if boolean(fact, "eligible", true) {
			t.Fatal("unknown competence qualified", fact)
		}
	}
	// Native writer only serializes its ABI fields; evidence and global contracts
	// must not disappear or turn into an unqualified wildcard after a save.
	delete(snapshot, "role_contracts")
	for _, model := range models {
		delete(model, "competence")
		delete(model, "routing_competence")
	}
	call(t, m, "snapshot.save", object{"config": snapshot, "expected_revision": str(snapshot, "revision")})
	after := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	if after["role_contracts"] == nil || rows(after, "models")[0]["competence"] == nil {
		t.Fatal("native save erased competence policy")
	}
	if !boolean(rows(rows(after, "models")[0], "routing_competence")[0], "eligible", false) {
		t.Fatal("qualified verdict lost")
	}
}
func TestCompetenceRejectsMalformedPolicyAndEvidence(t *testing.T) {
	for _, bad := range []string{
		`{"code":{"score":101,"source":"operator"}}`, `{"all":{"score":80,"source":"operator"}}`,
		`{"code":{"score":80}}`, `{"code":{"score":70.5,"source":"operator"}}`,
		`{"code":{"score":"80","source":"operator"}}`, `null`,
	} {
		var value any
		json.Unmarshal([]byte(bad), &value)
		if normalizeModel(object{"competence": value}) == nil {
			t.Fatal("accepted malformed evidence", bad)
		}
	}
	for _, bad := range []string{`{"code":{"min_competence":0}}`, `{"code":{"min_competence":101}}`, `{"all":{"min_competence":80}}`, `null`} {
		var value any
		json.Unmarshal([]byte(bad), &value)
		if validateRoleContracts(object{"role_contracts": value}) == nil {
			t.Fatal("accepted malformed contract", bad)
		}
	}
	m, home, _ := manager(t)
	os.WriteFile(filepath.Join(home, "models.json"), []byte(`{"role_contracts":{"code":{"min_competence":101}},"models":[]}`), 0600)
	if _, err := m.Manage(context.Background(), Request{Operation: "snapshot.load"}); err == nil {
		t.Fatal("invalid policy reached native routing")
	}
}

func TestCompetenceModelReplacementAndSpoofedVerdict(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "model.add", object{"args": []string{"m", "http://localhost/v1", "original"}})
	call(t, m, "model.set", object{"args": []string{"m", "--competence", `{"code":{"score":80,"source":"operator: verified edits"}}`}})
	snapshot := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	model := rows(snapshot, "models")[0]
	if str(model, "competence_model") != "original" {
		t.Fatal("evidence is not bound to model identity")
	}
	model["model"] = "replacement"
	model["routing_competence"] = []object{{"role": "code", "score": 100, "eligible": true}}
	snapshot["role_contracts"] = object{"code": object{"min_competence": 70}}
	call(t, m, "snapshot.save", object{"config": snapshot, "expected_revision": str(snapshot, "revision")})
	after := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	facts := rows(rows(after, "models")[0], "routing_competence")
	if len(facts) != 1 || boolean(facts[0], "eligible", true) {
		t.Fatal("replacement inherited competence or trusted forged verdict", facts)
	}
}
