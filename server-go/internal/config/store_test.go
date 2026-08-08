package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPolicyWritesPreserveUnrelatedConfigAndAreImmediatelyLive(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(path, []byte("provider: codex\ncustom:\n  keep: yes\nautonomy:\n  concurrency: 2\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := store.Int("autonomy.concurrency", 8); got != 2 {
		t.Fatalf("concurrency=%d", got)
	}
	if err := store.Set("autonomy.concurrency", float64(5)); err != nil {
		t.Fatal(err)
	}
	if got := store.Int("autonomy.concurrency", 8); got != 5 {
		t.Fatalf("live concurrency=%d", got)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(content), "provider: codex") || !strings.Contains(string(content), "keep: yes") {
		t.Fatalf("unrelated config lost:\n%s", content)
	}
	if err := store.Set("autonomy.max_turns", -1); err == nil {
		t.Fatal("negative policy accepted")
	}
}

func TestPolicyProjectionIncludesRuntimeConcurrencyDefault(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	store, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	values, err := store.Values()
	if err != nil {
		t.Fatal(err)
	}
	if got := values["autonomy.concurrency"]; got != 5 {
		t.Fatalf("autonomy.concurrency default=%v, want 5", got)
	}
	if got := values["autonomy.per_workflow_concurrency"]; got != 1 {
		t.Fatalf("autonomy.per_workflow_concurrency default=%v, want 1", got)
	}
	if got := values["autonomy.delegate_pending_secs"]; got != 120 {
		t.Fatalf("autonomy.delegate_pending_secs default=%v, want 120", got)
	}
	if err := store.Set("autonomy.delegate_pending_secs", 1); err == nil {
		t.Fatal("delegate lease below minimum accepted")
	}
	if err := store.Set("autonomy.delegate_pending_secs", 3601); err == nil {
		t.Fatal("delegate lease above maximum accepted")
	}
}

func TestEditableProjectionAndStructuralConflictsFailClosed(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(path, []byte("provider_token: secret\nautonomy: broken\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	store, _ := NewStore(path)
	values, err := store.Values()
	if err != nil {
		t.Fatal(err)
	}
	if _, leaked := values["provider_token"]; leaked {
		t.Fatal("secret leaked through editable config projection")
	}
	if err := store.Set("security.token", "replacement"); err == nil {
		t.Fatal("unknown config key accepted")
	}
	if err := store.Set("autonomy.concurrency", 3); err == nil {
		t.Fatal("scalar parent was destructively replaced")
	}
	content, _ := os.ReadFile(path)
	if !strings.Contains(string(content), "autonomy: broken") {
		t.Fatalf("config mutated:\n%s", content)
	}
}

func TestTriggerRulesUseOptimisticVersion(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	store, _ := NewStore(path)
	version, err := store.Version("trigger_rules")
	if err != nil {
		t.Fatal(err)
	}
	rules := []map[string]any{{"source": "watch-dir", "pipeline": map[string]any{"template": "build", "workspace": "/repo"}}}
	if err := store.SetVersioned("trigger_rules", rules, version); err != nil {
		t.Fatal(err)
	}
	if err := store.SetVersioned("trigger_rules", []any{}, version); err == nil {
		t.Fatal("stale trigger edit accepted")
	}
}

func TestTriggerRulesRejectOversizedRegistry(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	store, _ := NewStore(path)
	version, err := store.Version("trigger_rules")
	if err != nil {
		t.Fatal(err)
	}
	rules := make([]map[string]any, MaxTriggerRules+1)
	for i := range rules {
		rules[i] = map[string]any{
			"source":   "watch-dir",
			"pipeline": map[string]any{"template": "build", "workspace": "/repo"},
		}
	}
	if err := store.SetVersioned("trigger_rules", rules, version); err == nil || !strings.Contains(err.Error(), "maximum is 32") {
		t.Fatalf("oversized registry error = %v", err)
	}
}

func TestTriggerRulesRejectUnsafeHumanInputs(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	store, _ := NewStore(path)
	version, _ := store.Version("trigger_rules")
	base := map[string]any{
		"source": "watch-dir", "event": "docs/proposals/pending",
		"pipeline": map[string]any{"template": "build", "workspace": "/repo"},
	}
	for name, mutate := range map[string]func(map[string]any){
		"traversal":          func(rule map[string]any) { rule["event"] = "../outside" },
		"relative-workspace": func(rule map[string]any) { rule["pipeline"].(map[string]any)["workspace"] = "repo" },
		"git-option":         func(rule map[string]any) { rule["schedule"] = "--all" },
		"negative-cap":       func(rule map[string]any) { rule["pipeline"].(map[string]any)["max_spend_usd"] = -1 },
	} {
		t.Run(name, func(t *testing.T) {
			rule := map[string]any{
				"source": base["source"], "event": base["event"],
				"pipeline": map[string]any{"template": "build", "workspace": "/repo"},
			}
			mutate(rule)
			if err := store.SetVersioned("trigger_rules", []map[string]any{rule}, version); err == nil {
				t.Fatal("unsafe trigger rule was accepted")
			}
		})
	}
}

// A wall cap below the write-role floor makes every implement stage refuse
// before it starts, so the stage can never finish however often it retries.
// Rejecting the value when it is set reports that as the configuration error it
// is, instead of leaving it to be inferred from dying attempts.
func TestWallCapBelowWriteRoleFloorIsRejectedNamingBothValues(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(path, []byte("provider: codex\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	err = store.Set("autonomy.max_wall_secs", float64(MinAutonomyMaxWallSecs-1))
	if err == nil {
		t.Fatal("a wall cap under which no write stage can ever finish was accepted")
	}
	if !strings.Contains(err.Error(), "359") || !strings.Contains(err.Error(), "360") {
		t.Fatalf("error %q must name both the configured value and the required floor", err)
	}
	// And where the floor comes from, so it reads as derived rather than chosen.
	if !strings.Contains(err.Error(), "300s verifier reserve") ||
		!strings.Contains(err.Error(), "60s minimum run") {
		t.Fatalf("error %q must name the two components the floor is derived from", err)
	}
}

// The shipped default must keep loading. A floor that rejected the default would
// be a worse failure than the misconfiguration it exists to catch.
func TestDefaultWallCapRemainsAcceptable(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(path, []byte("provider: codex\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	shipped, ok := number(policyDefaults["autonomy.max_wall_secs"])
	if !ok {
		t.Fatalf("shipped default missing: %#v", policyDefaults["autonomy.max_wall_secs"])
	}
	if shipped < MinAutonomyMaxWallSecs {
		t.Fatalf("shipped default %d is below the enforced floor %d", shipped, MinAutonomyMaxWallSecs)
	}
	if err := store.Set("autonomy.max_wall_secs", float64(shipped)); err != nil {
		t.Fatalf("shipped default rejected: %v", err)
	}
	if err := store.Set("autonomy.max_wall_secs", float64(MinAutonomyMaxWallSecs)); err != nil {
		t.Fatalf("exact floor rejected: %v", err)
	}
}
