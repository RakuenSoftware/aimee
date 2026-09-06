package providers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

type testVault struct{ values map[string]string }

func (v *testVault) Credential(_ context.Context, op, agent, cred, secret string) (string, error) {
	key := agent + ":" + cred
	switch op {
	case "get":
		return v.values[key], nil
	case "set":
		v.values[key] = secret
	case "delete":
		delete(v.values, key)
	}
	return "", nil
}
func manager(t *testing.T) (*Manager, string, *testVault) {
	t.Helper()
	home := t.TempDir()
	store, err := NewStore(home)
	if err != nil {
		t.Fatal(err)
	}
	v := &testVault{map[string]string{}}
	return NewManager(store, v), home, v
}
func call(t *testing.T, m *Manager, op string, args object) object {
	t.Helper()
	reply, err := m.Manage(context.Background(), Request{Operation: op, Arguments: args, Actor: "webuser:test", SecretWriteAllowed: true})
	if err != nil {
		t.Fatal(op, err)
	}
	return reply
}
func connection(name, key string) object {
	return object{"name": name, "provider": "openai", "endpoint": "http://127.0.0.1:18765/v1", "auth_type": "bearer", "api_key": key, "create": true}
}
func TestProviderLifecycleAndCredentialIsolation(t *testing.T) {
	m, home, vault := manager(t)
	call(t, m, "provider.save_connection", connection("work", "key-work"))
	call(t, m, "provider.save_connection", connection("personal", "key-personal"))
	if got := len(rows(call(t, m, "provider.connections", nil), "providers")); got != 2 {
		t.Fatal(got)
	}
	for _, name := range []string{"work", "personal"} {
		call(t, m, "model.add", object{"args": []string{name + ":m", "ignored", "m", "--registration", name}})
	}
	changed := connection("work", "")
	changed["create"] = false
	changed["endpoint"] = "http://127.0.0.1:18765/moved/v1"
	call(t, m, "provider.save_connection", changed)
	list := rows(call(t, m, "model.list", nil), "agents")
	if str(list[0], "endpoint") != str(changed, "endpoint") || str(list[1], "endpoint") == str(changed, "endpoint") {
		t.Fatal(list)
	}
	if vault.values["work:api_key"] != "key-work" {
		t.Fatal("blank key did not preserve secret")
	}
	changed["api_key"] = "rotated"
	call(t, m, "provider.save_connection", changed)
	if vault.values["work:api_key"] != "rotated" || vault.values["personal:api_key"] != "key-personal" {
		t.Fatal("cross-provider rekey")
	}
	raw, err := os.ReadFile(filepath.Join(home, "models.json"))
	if err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"key-work", "key-personal", "rotated"} {
		if strings.Contains(string(raw), key) {
			t.Fatal("literal secret persisted")
		}
	}
	restarted, _ := NewStore(home)
	m = NewManager(restarted, vault)
	if len(rows(call(t, m, "provider.connections", nil), "providers")) != 2 {
		t.Fatal("connections lost on restart")
	}
	if _, err := m.Manage(context.Background(), Request{Operation: "provider.remove_connection", Arguments: object{"name": "work"}}); err == nil {
		t.Fatal("removed attached model without confirmation")
	}
	call(t, m, "provider.remove_connection", object{"name": "work", "remove_models": true})
	remaining := rows(call(t, m, "model.list", nil), "agents")
	if len(remaining) != 1 || str(remaining[0], "registration") != "personal" {
		t.Fatal(remaining)
	}
	call(t, m, "model.remove", object{"args": []string{"personal:m"}})
	if len(rows(call(t, m, "provider.connections", nil), "providers")) != 1 {
		t.Fatal("last model removal deleted provider")
	}
	call(t, m, "provider.remove_connection", object{"name": "personal"})
	if len(rows(call(t, m, "provider.connections", nil), "providers")) != 0 {
		t.Fatal("provider survived deletion")
	}
}
func TestModelDeclarationsAndSurgicalEdits(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "model.add", object{"args": []string{"model", "http://localhost/v1", "m"}})
	call(t, m, "model.set", object{"args": []string{"model", "--price-in", "0", "--price-out", "2.5", "--price-cached", "0.25", "--context-window", "40000", "--max-output", "4096", "--roles", "code,review"}})
	view := rows(call(t, m, "model.list", nil), "agents")[0]
	if !boolean(view, "price_in_declared", false) || number(view, "price_in_per_mtok") != 0 || number(view, "price_out_per_mtok") != 2.5 || number(view, "price_cached_per_mtok") != .25 {
		t.Fatal(view)
	}
	call(t, m, "model.set", object{"args": []string{"model", "--price-in", "", "--context-window", "0", "--roles", "", "--price-out", "NaN"}})
	view = rows(call(t, m, "model.list", nil), "agents")[0]
	if boolean(view, "price_in_declared", true) || str(view, "context_window_source") != "unknown" || number(view, "price_out_per_mtok") != 2.5 || number(view, "max_output") != 4096 {
		t.Fatal(view)
	}
	b, _ := json.Marshal(view["roles"])
	if string(b) != "[]" {
		t.Fatal(string(b))
	}
}
func TestMalformedRosterAndDuplicateLeaveStateUntouched(t *testing.T) {
	m, home, _ := manager(t)
	call(t, m, "provider.save_connection", connection("work", ""))
	before, _ := os.ReadFile(filepath.Join(home, "models.json"))
	if _, err := m.Manage(context.Background(), Request{Operation: "provider.save_connection", Arguments: connection("work", "")}); err == nil {
		t.Fatal("duplicate accepted")
	}
	after, _ := os.ReadFile(filepath.Join(home, "models.json"))
	if string(before) != string(after) {
		t.Fatal("duplicate changed state")
	}
	for _, bad := range []string{"{", `{"models":{}}`, `{"providers":[{}]}`, `{"providers":[{"name":"x","provider":"openai","api_key_ref":"literal"}]}`} {
		if err := os.WriteFile(filepath.Join(home, "models.json"), []byte(bad), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := m.Manage(context.Background(), Request{Operation: "provider.save_connection", Arguments: connection("new", "")}); err == nil {
			t.Fatal("corrupt roster overwritten")
		}
		after, _ := os.ReadFile(filepath.Join(home, "models.json"))
		if string(after) != bad {
			t.Fatal("changed malformed roster")
		}
	}
}
func TestLegacyRegistrationAndSnapshotWriterPreserveProvider(t *testing.T) {
	m, home, _ := manager(t)
	original := `{"agents":[{"name":"legacy","model":"m","endpoint":"http://localhost/v1","api_key":"$LEGACY_KEY","custom_field":{"keep":true}}],"network":{"keep":"yes"}}`
	if err := os.WriteFile(filepath.Join(home, "agents.json"), []byte(original), 0600); err != nil {
		t.Fatal(err)
	}
	call(t, m, "model.remove", object{"args": []string{"legacy"}})
	providers := rows(call(t, m, "provider.connections", nil), "providers")
	if len(providers) != 1 || str(providers[0], "name") != "legacy" {
		t.Fatal(providers)
	}
	var root object
	b, _ := os.ReadFile(filepath.Join(home, "agents.json"))
	if json.Unmarshal(b, &root) != nil {
		t.Fatal("invalid persisted JSON")
	}
	if str(rows(root, "providers")[0], "api_key_ref") != "$LEGACY_KEY" {
		t.Fatal("lost reference")
	}
	if root["network"] == nil {
		t.Fatal("lost unknown root field")
	}
	call(t, m, "snapshot.save", object{"config": object{"models": []object{}}})
	if len(rows(call(t, m, "provider.connections", nil), "providers")) != 1 {
		t.Fatal("snapshot save erased connection")
	}
}
func TestConcurrentManagersSerializeMutations(t *testing.T) {
	m, home, _ := manager(t)
	var wg sync.WaitGroup
	failures := make(chan error, 10)
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			s, _ := NewStore(home)
			other := NewManager(s, nil)
			_, err := other.Manage(context.Background(), Request{Operation: "provider.save_connection", Arguments: connection(string(rune('a'+i)), "")})
			if err != nil {
				failures <- err
			}
		}(i)
	}
	wg.Wait()
	close(failures)
	for err := range failures {
		t.Error(err)
	}
	if len(rows(call(t, m, "provider.connections", nil), "providers")) != 10 {
		t.Fatal("concurrent provider addition lost")
	}
}
func TestUnattestedCredentialWriteFailsClosed(t *testing.T) {
	m, _, vault := manager(t)
	_, err := m.Manage(context.Background(), Request{Operation: "provider.save_connection", Arguments: connection("work", "secret")})
	if err == nil || len(vault.values) != 0 {
		t.Fatal("unattested write accepted")
	}
}

func TestProfileOperationsDistinguishMissingAndUnknownNames(t *testing.T) {
	m, _, _ := manager(t)
	for _, op := range []string{"provider.show", "provider.models", "provider.test"} {
		for _, tc := range []struct{ name, kind string }{{"", "invalid_argument"}, {"unknown-fixture", "not_found"}} {
			_, err := m.Manage(context.Background(), Request{Operation: op, Arguments: object{"name": tc.name}})
			if err == nil || errorKind(err) != tc.kind {
				t.Fatalf("%s name %q: %v", op, tc.name, err)
			}
		}
	}
}
