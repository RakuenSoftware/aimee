package memory

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func TestProfilePackCommands(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("AIMEE_PACK_DIR", dir)
	good := `{"name":"good","description":"a valid pack","validation_rules":{"allowed_tiers":["L1","L2"],"allowed_kinds":["fact","preference"]},"scope_defaults":{"tier":"L1","visibility":"default"}}`
	write := func(name, body string) string {
		t.Helper()
		path := filepath.Join(dir, name+".json")
		if err := os.WriteFile(path, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
		return path
	}
	goodPath := write("good", good)
	for _, tt := range []struct{ name, body, message string }{
		{"noname", `{"description":"missing name"}`, "name"},
		{"nodesc", `{"name":"nodesc"}`, "description"},
		{"badkey", `{"name":"badkey","description":"d","unknown_field":"x"}`, "unknown"},
		{"badtier", `{"name":"badtier","description":"d","validation_rules":{"allowed_tiers":["L9"]}}`, "tier"},
	} {
		if _, err := readProfilePack(write(tt.name, tt.body)); err == nil || !strings.Contains(err.Error(), tt.message) {
			t.Fatal(tt.name, err)
		}
	}
	write("mypack", strings.ReplaceAll(strings.ReplaceAll(good, `"good"`, `"mypack"`), `"default"`, `"strict"`))
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		run := func(args map[string]any) map[string]any {
			t.Helper()
			raw, _ := json.Marshal(args)
			r := runPublicCommand(t, client, "pack", string(raw))
			if r["status"] != "ok" {
				t.Fatal(r)
			}
			return r
		}
		if err := os.Remove(filepath.Join(dir, "active-pack")); err != nil && !os.IsNotExist(err) {
			t.Fatal(err)
		}
		listed := run(map[string]any{"action": "list"})["output"].(map[string]any)
		if listed["active"] != "default" || len(listed["packs"].([]any)) != 6 {
			t.Fatal(listed)
		}
		shown := run(map[string]any{"action": "show", "name": "mypack"})["output"].(map[string]any)
		if shown["name"] != "mypack" || shown["default_visibility"] != "strict" || len(shown["allowed_tiers"].([]any)) != 2 || len(shown["allowed_kinds"].([]any)) != 2 {
			t.Fatal(shown)
		}
		run(map[string]any{"action": "validate", "path": goodPath})
		for _, name := range []string{"mypack", "good"} {
			run(map[string]any{"action": "use", "name": name})
			if activeProfilePack(dir) != name {
				t.Fatal(name)
			}
		}
		for _, name := range []string{"does_not_exist", "noname", "../good", "good\nother"} {
			raw, _ := json.Marshal(map[string]any{"action": "use", "name": name})
			if r := runPublicCommand(t, client, "pack", string(raw)); r["status"] != "error" || activeProfilePack(dir) != "good" {
				t.Fatal(r)
			}
		}
	}
	// Concurrent activation publishes one complete selection, never partial text.
	var wg sync.WaitGroup
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			name := []string{"good", "mypack"}[i%2]
			if err := activateProfilePack(dir, name); err != nil {
				t.Error(err)
			}
			if got := activeProfilePack(dir); got != "good" && got != "mypack" {
				t.Error(got)
			}
		}(i)
	}
	wg.Wait()
	if info, err := os.Stat(filepath.Join(dir, "active-pack")); err != nil || info.Mode().Perm() != 0600 {
		t.Fatal(info, err)
	}
	for _, body := range []string{"", strings.Repeat("x", 65537), `[]`, `{"name":"x","description":"d","validation_rules":null}`, `{"name":"x","description":"d","validation_rules":{"allowed_tiers":null}}`, `{"name":"x","description":"d","explain_labels":[]}`} {
		if _, err := readProfilePack(write("invalid", body)); err == nil {
			t.Fatal(body)
		}
	}
}

func TestProfilePackDirectory(t *testing.T) {
	t.Setenv("AIMEE_PACK_DIR", "")
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if dir, err := profilePackDir(); err != nil || dir != filepath.Join(home, "packs") {
		t.Fatal(dir, err)
	}
	t.Setenv("AIMEE_PACK_DIR", filepath.Join(home, "custom"))
	if dir, err := profilePackDir(); err != nil || dir != filepath.Join(home, "custom") {
		t.Fatal(dir, err)
	}
}
