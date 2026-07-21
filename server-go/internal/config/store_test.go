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
	if got := values["autonomy.concurrency"]; got != 2 {
		t.Fatalf("autonomy.concurrency default=%v, want 2", got)
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
