package providers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestReadOnlyRolesAndPersonasPreserveBytes(t *testing.T) {
	m, home, _ := manager(t)
	call(t, m, "model.add", object{"args": []string{"m", "http://localhost/v1", "m"}})
	path := filepath.Join(home, "models.json")
	before, _ := os.ReadFile(path)
	for _, op := range []string{"model.roles", "model.personas"} {
		reply := call(t, m, op, object{"args": []string{"m"}})
		if !boolean(reply, "read_only", false) {
			t.Fatal(reply)
		}
		after, _ := os.ReadFile(path)
		if string(before) != string(after) {
			t.Fatal("read mutated roster")
		}
	}
	call(t, m, "model.roles", object{"args": []string{"m", "review"}})
	r := call(t, m, "model.roles", object{"args": []string{"m", "--reset"}})
	b, _ := json.Marshal(r["roles"])
	for _, role := range []string{"code", "explain", "refactor", "draft", "execute", "summarize", "format", "reason", "search"} {
		found := false
		for _, v := range r["roles"].([]any) {
			found = found || v == role
		}
		if !found {
			t.Fatalf("missing %s in %s", role, b)
		}
	}
}
func TestSnapshotRejectsStaleWriter(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "model.add", object{"args": []string{"m", "http://localhost/v1", "m"}})
	snapshot := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	expected := str(snapshot, "revision")
	call(t, m, "model.set", object{"args": []string{"m", "--roles", "review"}})
	if _, err := m.Manage(context.Background(), Request{Operation: "snapshot.save", Arguments: object{"config": snapshot, "expected_revision": expected}}); err == nil {
		t.Fatal("stale writer accepted")
	}
	current := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	call(t, m, "snapshot.save", object{"config": current, "expected_revision": str(current, "revision")})
}
func TestCredentialDeletionRecoveryAndNameReuse(t *testing.T) {
	m, home, vault := manager(t)
	call(t, m, "provider.save_connection", connection("account", "old-secret"))
	call(t, m, "provider.remove_connection", object{"name": "account"})
	// Restart between the durable roster deletion and external Vault cleanup.
	store, _ := NewStore(home)
	m = NewManager(store, vault)
	call(t, m, "provider.save_connection", connection("account", ""))
	if vault.values["account:api_key"] != "" {
		t.Fatal("deleted account credential reused")
	}
	call(t, m, "provider.save_connection", connection("other", "other-secret"))
	edit := connection("account", "new-secret")
	edit["create"] = false
	call(t, m, "provider.save_connection", edit)
	if vault.values["other:api_key"] != "other-secret" {
		t.Fatal("cross-account mutation")
	}
}
func TestLegacyCredentialMigrationAndExpansion(t *testing.T) {
	m, home, vault := manager(t)
	raw := `{"agents":[{"name":"work","provider":"openai","endpoint":"https://example.invalid/v1","api_key":"legacy-secret","models":["first","second"],"enabled":0}],"custom":{"retained":true}}`
	path := filepath.Join(home, "agents.json")
	os.WriteFile(path, []byte(raw), 0600)
	listed := rows(call(t, m, "model.list", nil), "agents")
	if len(listed) != 2 || boolean(listed[0], "enabled", true) || str(listed[1], "registration") != "work" {
		t.Fatal(listed)
	}
	if vault.values["work:api_key"] != "legacy-secret" {
		t.Fatal("key not migrated")
	}
	b, _ := os.ReadFile(path)
	var root object
	json.Unmarshal(b, &root)
	if root["custom"] == nil {
		t.Fatal("extension lost")
	}
	for _, row := range rows(root, "models") {
		if str(row, "api_key") != "" {
			t.Fatal("plaintext remains")
		}
	}
}
func TestLegacyExpansionFailsAsWholeRoster(t *testing.T) {
	for _, models := range []string{`[]`, `["one","one"]`, `["one",null]`, `"unknown"`, `{}`} {
		t.Run(models, func(t *testing.T) {
			m, home, _ := manager(t)
			raw := `{"models":[{"name":"work","provider":"openai","models":` + models + `}]}`
			os.WriteFile(filepath.Join(home, "models.json"), []byte(raw), 0600)
			if _, err := m.Manage(context.Background(), Request{Operation: "model.list"}); err == nil {
				t.Fatal("partial fleet accepted")
			}
		})
	}
}
func TestCatalogIdentityUsesHostLabelsAndOperatorDeclaration(t *testing.T) {
	for _, tc := range []struct{ endpoint, model, wire, explicit, want string }{
		{"https://api.kimi.com/v1", "m", "anthropic", "", "moonshotai"},
		{"https://api.kimi.com.attacker.example/v1", "m", "anthropic", "", "anthropic"},
		{"https://gateway.example/api.minimax.io/v1", "m", "openai", "", "openai"},
		{"https://gateway.example/v1", "minimaximum-production", "openai", "", "openai"},
		{"https://gateway.example/v1", "moonshotai/kimi-k2.7", "openai", "", "moonshotai"},
		{"https://api.minimax.io/v1", "MiniMax-M3", "anthropic", "custom", "custom"},
		{"", "gpt-5.5", "chatgpt", "", "openai"},
	} {
		got := catalogProvider(object{"provider": tc.wire, "endpoint": tc.endpoint, "model": tc.model, "catalog_provider": tc.explicit})
		if got != tc.want {
			t.Errorf("%+v: %s", tc, got)
		}
	}
}
func TestMetadataPublishedValuesAndBandsWin(t *testing.T) {
	m, home, _ := manager(t)
	snapshot := filepath.Join(home, "snapshot.json")
	os.WriteFile(snapshot, []byte(`{"openai":{"models":{"sample":{"name":"Sample","tool_call":true,"reasoning":true,"limit":{"context":1000000,"output":128000},"cost":{"input":5,"output":22.5,"cache_read":0.5,"tiers":[{"tier":{"type":"context","size":272000},"input":10,"output":45,"cache_read":1}]}}}}}`), 0600)
	t.Setenv("XDG_CACHE_HOME", home)
	t.Setenv("AIMEE_MODELS_DEV_SNAPSHOT", snapshot)
	call(t, m, "model.add", object{"args": []string{"model", "https://example.invalid/v1", "sample"}})
	model := rows(call(t, m, "model.list", nil), "agents")[0]
	if number(model, "effective_context_window") != 1000000 || number(model, "effective_max_output") != 128000 || number(model, "price_base_in_per_mtok") != 5 || len(rows(model, "price_bands")) != 1 {
		t.Fatal(model)
	}
	call(t, m, "model.set", object{"args": []string{"model", "--context-window", "40000", "--max-output", "4096", "--price-in", "0"}})
	model = rows(call(t, m, "model.list", nil), "agents")[0]
	if number(model, "effective_context_window") != 40000 || number(model, "price_base_in_per_mtok") != 0 || number(rows(model, "price_bands")[0], "in_per_mtok") != 0 {
		t.Fatal(model)
	}
}
func TestSubscriptionRegistrationIsIdempotent(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "model.register_subscription", object{"vendor": "claude"})
	call(t, m, "model.set", object{"args": []string{"claude", "--primary-only", "off", "--roles", "review"}})
	call(t, m, "model.register_subscription", object{"vendor": "claude"})
	models := rows(call(t, m, "model.list", nil), "agents")
	if len(models) != 1 || boolean(models[0], "primary_only", true) {
		t.Fatal(models)
	}
}

func TestNativeSnapshotPreservesUnknownFieldsAndConnectionEdits(t *testing.T) {
	m, home, _ := manager(t)
	os.WriteFile(filepath.Join(home, "models.json"), []byte(`{"models":[{"name":"seat","model":"m","endpoint":"http://old/v1","future":{"enabled":true}}]}`), 0600)
	cfg := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	model := rows(cfg, "models")[0]
	delete(model, "future")
	model["endpoint"] = "http://new/v1"
	result := call(t, m, "snapshot.save", object{"config": cfg, "expected_revision": str(cfg, "revision")})
	cfg = call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	if rows(cfg, "models")[0]["future"] == nil || str(rows(cfg, "providers")[0], "endpoint") != "http://new/v1" || str(result, "revision") != str(cfg, "revision") {
		t.Fatal(cfg, result)
	}
}
func TestInvalidModelDoesNotRotateCredential(t *testing.T) {
	m, _, vault := manager(t)
	call(t, m, "model.add", object{"args": []string{"seat", "http://localhost/v1", "m", "--key", "old"}})
	_, err := m.Manage(context.Background(), Request{Operation: "model.add", Arguments: object{"args": []string{"seat", "http://localhost/v1", "m", "--key", "replacement", "--context-window", "10", "--max-output", "100"}}, SecretWriteAllowed: true})
	if err == nil || vault.values["seat:api_key"] != "old" {
		t.Fatal(err, vault.values)
	}
}
