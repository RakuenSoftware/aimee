package providers

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type selectorConfig struct {
	recoveryConfig
	selected string
	writes   int
}

func (c *selectorConfig) StringValue(key string) (string, bool, error) {
	if key != "provider" {
		return "", false, fmt.Errorf("unexpected selector config key")
	}
	return c.selected, c.selected != "", nil
}
func (c *selectorConfig) Set(key string, value any) error {
	name, ok := value.(string)
	if key != "provider" || !ok {
		return fmt.Errorf("unexpected selector config mutation")
	}
	c.selected = name
	c.writes++
	return nil
}

func TestProviderSelectorRetainsInstalledModelNameBoundaries(t *testing.T) {
	m, _, _ := manager(t)
	cfg := &selectorConfig{recoveryConfig: recoveryConfig{limits: map[string]int{}}}
	m.SetConfig(cfg)
	for _, size := range []int{15, 16, 22, 63} {
		t.Run(fmt.Sprint(size), func(t *testing.T) {
			name := "m" + strings.Repeat("a", size-1)
			call(t, m, "model.add", object{"args": []string{name, "http://127.0.0.1:18765/v1", "model", "--auth-type", "none"}})
			before := cfg.writes
			reply, err := m.Manage(context.Background(), Request{Operation: "provider.set", Arguments: object{"name": name}, Actor: "webuser:test"})
			if err != nil || str(reply, "provider") != name || cfg.selected != name || cfg.writes != before+1 {
				t.Fatalf("provider.set must retain installed model name of %d bytes: %v", size, err)
			}
			got := call(t, m, "provider.get", nil)
			if str(got, "provider") != name {
				t.Fatal("selected model lookup changed the name")
			}
		})
	}
}

func TestProviderSelectorStillRefusesUnknownAndInvalidNamesWithoutMutation(t *testing.T) {
	m, home, vault := manager(t)
	cfg := &selectorConfig{recoveryConfig: recoveryConfig{limits: map[string]int{}}}
	m.SetConfig(cfg)
	call(t, m, "model.add", object{"args": []string{"known", "http://127.0.0.1:18765/v1", "model", "--auth-type", "none"}})
	before, err := os.ReadFile(filepath.Join(home, "models.json"))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"", strings.Repeat("a", 64), "nul\x00name", "cr\rname", "lf\nname", strings.Repeat("u", 63)} {
		_, err := m.Manage(context.Background(), Request{Operation: "provider.set", Arguments: object{"name": name}, Actor: "webuser:test"})
		if err == nil || cfg.writes != 0 {
			t.Fatal("invalid or uninstalled selector reached config mutation")
		}
		after, err := os.ReadFile(filepath.Join(home, "models.json"))
		if err != nil || !bytes.Equal(before, after) || len(vault.values) != 0 {
			t.Fatal("refused selector changed models or credentials")
		}
	}
}
