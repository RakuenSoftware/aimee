package providers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// Normal Models saves resend tools even when only the model ID was edited.
// An unchanged tools value must not change the independent respond-tool policy.
func TestModelSetToolsPreservesPolicyUnlessToggled(t *testing.T) {
	profiles := []struct{ name, provider, endpoint, auth string }{
		{"remote-http", "openai", "https://example.test/v1", "bearer"},
		{"native-claude", "claude", "", "claude-oauth"},
		{"local-http", "ollama", "http://localhost:11434/v1", "none"},
	}
	for _, profile := range profiles {
		for _, tools := range []bool{false, true} {
			for _, inject := range []bool{false, true} {
				for _, toggle := range []bool{false, true} {
					name := profile.name
					if tools {
						name += "/tools-on"
					} else {
						name += "/tools-off"
					}
					if inject {
						name += "/inject-on"
					} else {
						name += "/inject-off"
					}
					if toggle {
						name += "/toggle"
					} else {
						name += "/unchanged"
					}
					t.Run(name, func(t *testing.T) {
						m, home, vault := manager(t)
						model := object{"name": "edited", "model": "old-model", "provider": profile.provider, "endpoint": profile.endpoint, "auth_type": profile.auth, "tools_enabled": tools, "inject_respond_tool": inject, "enabled": true}
						if profile.provider == "claude" {
							model["backend"] = "provider-cli"
							model["cli_kind"] = "claude"
							model["cli_cmd"] = "claude"
						}
						raw, err := json.Marshal(object{"models": []object{model}})
						if err != nil {
							t.Fatal(err)
						}
						if err = os.WriteFile(filepath.Join(home, "models.json"), raw, 0600); err != nil {
							t.Fatal(err)
						}
						requested := tools
						if toggle {
							requested = !tools
						}
						flag := "off"
						if requested {
							flag = "on"
						}
						call(t, m, "model.set", object{"args": []string{"edited", "--model", "new-model", "--tools", flag, "--enabled", "on", "--primary-only", "off"}})
						restarted, err := NewStore(home)
						if err != nil {
							t.Fatal(err)
						}
						m = NewManager(restarted, vault)
						got := rows(call(t, m, "snapshot.load", nil)["config"].(map[string]any), "models")[0]
						wantInject := inject
						if toggle {
							wantInject = requested
						}
						if str(got, "model") != "new-model" || boolean(got, "tools_enabled", !requested) != requested || boolean(got, "inject_respond_tool", !wantInject) != wantInject {
							t.Fatalf("save/reload changed policy: tools=%v inject=%v; want tools=%v inject=%v", got["tools_enabled"], got["inject_respond_tool"], requested, wantInject)
						}
					})
				}
			}
		}
	}
}

func TestModelSetExplicitRespondPolicyPreservesRegisteredModel(t *testing.T) {
	for _, tools := range []string{"on", "off"} {
		for _, inject := range []string{"on", "off"} {
			t.Run(tools+"/"+inject, func(t *testing.T) {
				m, home, vault := manager(t)
				call(t, m, "provider.save_connection", connection("account", "private-credential"))
				call(t, m, "model.add", object{"args": []string{"account:model", "ignored", "selected-model", "--registration", "account"}})
				call(t, m, "model.set", object{"args": []string{"account:model", "--max-turns", "7", "--roles", "review", "--competence", `{"review":{"score":80,"source":"operator fixture"}}`}})
				// Reproduce the prior dialog's persisted injection=true policy using
				// the existing, public tools-toggle coupling before repairing it.
				call(t, m, "model.set", object{"args": []string{"account:model", "--tools", "off"}})
				call(t, m, "model.set", object{"args": []string{"account:model", "--tools", "on"}})
				before := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
				call(t, m, "model.set", object{"args": []string{"account:model", "--tools", tools, "--inject-respond-tool", inject}})
				restarted, err := NewStore(home)
				if err != nil {
					t.Fatal(err)
				}
				m = NewManager(restarted, vault)
				after := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
				got := rows(after, "models")[0]
				if boolean(got, "tools_enabled", tools != "on") != (tools == "on") || boolean(got, "inject_respond_tool", inject != "on") != (inject == "on") {
					t.Fatal(got)
				}
				old := rows(before, "models")[0]
				for _, key := range []string{"registration", "model", "endpoint", "auth_type", "max_turns", "roles", "competence"} {
					a, _ := json.Marshal(old[key])
					b, _ := json.Marshal(got[key])
					if string(a) != string(b) {
						t.Fatalf("changed unrelated %s", key)
					}
				}
				if vault.values["account:api_key"] != "private-credential" {
					t.Fatal("changed provider credential")
				}
				// A full-dialog save without the explicit option must preserve the override.
				call(t, m, "model.set", object{"args": []string{"account:model", "--tools", tools}})
				got = rows(call(t, m, "snapshot.load", nil)["config"].(map[string]any), "models")[0]
				if boolean(got, "inject_respond_tool", inject != "on") != (inject == "on") {
					t.Fatal("override lost on unchanged save")
				}
			})
		}
	}
}

func TestModelSetRespondPolicyRejectsMalformedWithoutSaving(t *testing.T) {
	m, home, _ := manager(t)
	call(t, m, "model.add", object{"args": []string{"model", "https://example.test/v1", "old-model"}})
	path := filepath.Join(home, "models.json")
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []string{"", "enabled", "FALSE", "2"} {
		_, err = m.Manage(context.Background(), Request{Operation: "model.set", Arguments: object{"args": []string{"model", "--model", "changed-model", "--inject-respond-tool", value}}, Actor: "webuser:test"})
		if err == nil {
			t.Fatalf("accepted invalid value %q", value)
		}
		after, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if string(before) != string(after) {
			t.Fatal("invalid policy changed roster")
		}
	}
}
