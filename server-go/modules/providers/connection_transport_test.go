package providers

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestCodexConnectionTransportSurvivesStoreReloadAndEdits(t *testing.T) {
	m, home, vault := manager(t)
	p := object{"name": "native", "provider": "chatgpt", "endpoint": "https://chatgpt.com/backend-api/codex", "auth_type": "codex-oauth", "create": true, "backend": "provider-cli", "cli_kind": "codex"}
	call(t, m, "provider.save_connection", p)
	call(t, m, "model.add", object{"args": []string{"native:model", "ignored", "account-model", "--registration", "native"}})
	expectedCommand := "codex"
	check := func(wantNative bool) {
		t.Helper()
		saved := call(t, m, "model.list", nil)
		model := find(rows(saved, "agents"), "native:model")
		if str(model, "registration") != "native" || str(model, "provider") != "chatgpt" || str(model, "auth_type") != "codex-oauth" || str(model, "endpoint") != str(p, "endpoint") {
			t.Fatalf("connection lineage lost: %v", model)
		}
		if wantNative {
			if str(model, "backend") != "provider-cli" || str(model, "cli_kind") != "codex" || str(model, "cli_cmd") != expectedCommand || !boolean(model, "session_reuse", false) {
				t.Fatalf("native enrollment lost: %v", model)
			}
		} else if str(model, "backend") != "http" || str(model, "cli_kind") != "" || str(model, "cli_cmd") != "" || boolean(model, "session_reuse", false) {
			t.Fatalf("HTTP mode retained native dispatch: %v", model)
		}
		connections := rows(call(t, m, "provider.connections", nil), "providers")
		if str(find(connections, "native"), "backend") != str(model, "backend") {
			t.Fatal("editor cannot round-trip transport")
		}
	}
	check(true)
	p["create"], p["cli_cmd"] = false, "/approved/new/codex"
	expectedCommand = "/approved/new/codex"
	call(t, m, "provider.save_connection", p)
	check(true)
	call(t, m, "model.set", object{"args": []string{"native:model", "--max-tokens", "2048"}})
	check(true)
	p["create"], p["backend"] = false, "http"
	delete(p, "cli_kind")
	delete(p, "cli_cmd")
	call(t, m, "provider.save_connection", p)
	check(false)
	if len(vault.values) != 0 {
		t.Fatal("transport edits minted or rewrote credentials")
	}
	if _, err := os.ReadFile(filepath.Join(home, "models.json")); err != nil {
		t.Fatal(err)
	}
}

func TestInvalidConnectionTransportIsAtomicBeforeCredentialWrite(t *testing.T) {
	for _, fields := range []object{{"backend": "provider-cli"}, {"backend": "typo"}, {"backend": "provider-cli", "cli_kind": "codex", "session_reuse": "yes"}, {"backend": "http", "cli_kind": "codex"}} {
		m, home, vault := manager(t)
		p := connection("work", "")
		call(t, m, "provider.save_connection", p)
		before, _ := os.ReadFile(filepath.Join(home, "models.json"))
		p["create"], p["api_key"] = false, "must-not-be-written"
		for field, value := range fields {
			p[field] = value
		}
		_, err := m.Manage(context.Background(), Request{Operation: "provider.save_connection", Arguments: p, Actor: "webuser:test", SecretWriteAllowed: true})
		after, _ := os.ReadFile(filepath.Join(home, "models.json"))
		if err == nil || string(before) != string(after) || len(vault.values) != 0 {
			t.Fatalf("invalid transport committed state: fields=%v err=%v", fields, err)
		}
	}
}

func TestExplicitNativeConnectionPreservesCustomAliasCommands(t *testing.T) {
	for _, kind := range []string{"claude", "codex"} {
		t.Run(kind, func(t *testing.T) {
			m, home, _ := manager(t)
			raw := `{"providers":[{"name":"account","provider":"chatgpt","endpoint":"https://chatgpt.com/backend-api/codex","auth_type":"codex-oauth","backend":"provider-cli","cli_kind":"` + kind + `","cli_cmd":"` + kind + `"}],"models":[{"name":"custom","registration":"account","provider":"chatgpt","backend":"provider-cli","cli_kind":"` + kind + `","cli_cmd":"/opt/owned/custom-` + kind + `","model":"account-model"}]}`
			if err := os.WriteFile(filepath.Join(home, "models.json"), []byte(raw), 0600); err != nil {
				t.Fatal(err)
			}
			got := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
			if actual := str(find(rows(got, "models"), "custom"), "cli_cmd"); actual != "/opt/owned/custom-"+kind {
				t.Fatalf("unrelated read rewrote legacy custom command: got %q", actual)
			}
			edit := object{"name": "account", "provider": "chatgpt",
				"endpoint": "https://chatgpt.com/backend-api/codex", "auth_type": "codex-oauth"}
			call(t, m, "provider.save_connection", edit)
			for _, command := range []string{kind, "/approved/new/" + kind} {
				edit["backend"], edit["cli_kind"], edit["cli_cmd"] = "provider-cli", kind, command
				call(t, m, "provider.save_connection", edit)
				config := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
				if actual := str(find(rows(config, "models"), "custom"), "cli_cmd"); actual != "/opt/owned/custom-"+kind {
					t.Fatalf("connection edit rewrote custom command: got %q", actual)
				}
			}
		})
	}
}
