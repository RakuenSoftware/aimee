package delegates

// The roster file has had two spellings. `aimee model local` writes models.json
// with the entries under "models"; older installs have agents.json with them
// under "agents". NewRegistryExecutor prefers models.json, so reading only
// "agents" meant it chose the file it could not parse: registering a model the
// documented way produced "agent registry has no agents" and every delegation
// was refused, while a machine old enough to still have agents.json worked
// fine. That is why this is tested from the file down rather than on the struct.

import (
	"os"
	"path/filepath"
	"testing"
)

const oneModel = `{
  "default_agent": "qwen38",
  "models": [{"name":"qwen38","model":"default","enabled":true,
              "roles":["explain"],"max_parallel":1}]
}`

const oneAgent = `{
  "default_agent": "legacy",
  "agents": [{"name":"legacy","model":"default","enabled":1,
              "roles":["explain"],"max_parallel":1}]
}`

func TestRegistryReadsBothRosterSpellings(t *testing.T) {
	for _, tc := range []struct {
		file string
		body string
		want string
	}{
		{"models.json", oneModel, "qwen38"},
		{"agents.json", oneAgent, "legacy"},
	} {
		t.Run(tc.file, func(t *testing.T) {
			home := t.TempDir()
			if err := os.WriteFile(filepath.Join(home, tc.file), []byte(tc.body), 0o600); err != nil {
				t.Fatal(err)
			}
			executor, err := NewRegistryExecutor(home)
			if err != nil {
				t.Fatalf("%s: %v -- a roster with one usable entry must load", tc.file, err)
			}
			registry, err := executor.load()
			if err != nil {
				t.Fatal(err)
			}
			got := registry.roster()
			if len(got) != 1 {
				t.Fatalf("%s: roster has %d entries, want 1", tc.file, len(got))
			}
			if got[0].Name != tc.want {
				t.Errorf("%s: entry = %q, want %q", tc.file, got[0].Name, tc.want)
			}
		})
	}
}

func TestRegistryReadsLegacyNumericBooleans(t *testing.T) {
	for _, tc := range []struct {
		name      string
		enabled   string
		available string
		want      bool
	}{
		{name: "one is true", enabled: "1", available: "1", want: true},
		{name: "zero is false", enabled: "0", available: "0", want: false},
		{name: "booleans remain supported", enabled: "true", available: "true", want: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			home := t.TempDir()
			body := `{"agents":[{"name":"legacy","model":"default","roles":["explain"],` +
				`"enabled":` + tc.enabled + `,"delegate_available":` + tc.available + `}]}`
			if err := os.WriteFile(filepath.Join(home, "agents.json"), []byte(body), 0o600); err != nil {
				t.Fatal(err)
			}
			executor, err := NewRegistryExecutor(home)
			if err != nil {
				t.Fatalf("legacy registry did not load: %v", err)
			}
			entry := executor.registry.roster()[0]
			if got := enabled(entry); got != tc.want {
				t.Errorf("enabled=%v want %v", got, tc.want)
			}
			if got := available(entry); got != tc.want {
				t.Errorf("available=%v want %v", got, tc.want)
			}
		})
	}
}

func TestRegistryRejectsInvalidLegacyBoolean(t *testing.T) {
	home := t.TempDir()
	body := `{"agents":[{"name":"legacy","model":"default","roles":["explain"],"enabled":2}]}`
	if err := os.WriteFile(filepath.Join(home, "agents.json"), []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewRegistryExecutor(home); err == nil {
		t.Fatal("invalid legacy boolean loaded successfully")
	}
}

// models.json wins when both are present: it is the file the C side writes now,
// so a stale agents.json left behind by an older install must not shadow it.
func TestModelsJSONWinsOverALegacyAgentsFile(t *testing.T) {
	home := t.TempDir()
	if err := os.WriteFile(filepath.Join(home, "models.json"), []byte(oneModel), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(home, "agents.json"), []byte(oneAgent), 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	registry, err := executor.load()
	if err != nil {
		t.Fatal(err)
	}
	if got := registry.roster(); len(got) != 1 || got[0].Name != "qwen38" {
		t.Errorf("roster = %+v, want the models.json entry", got)
	}
}
