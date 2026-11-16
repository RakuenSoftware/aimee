package memory

import "testing"

func TestBackendSetupSelectionAndEnvironmentPrecedence(t *testing.T) {
	empty := func(string) string { return "" }
	for _, engine := range []string{"", "native", "aimee-native"} {
		cfg, err := ResolveBackendSettings(map[string]any{"memory_backend_engine": engine}, empty)
		if err != nil || cfg.Engine != "native" || cfg.Endpoint != "" {
			t.Fatal(cfg, err)
		}
	}
	env := map[string]string{"AIMEE_MEMORY_BACKEND": "native"}
	cfg, err := ResolveBackendSettings(map[string]any{"memory_backend_engine": "hillock"}, func(k string) string { return env[k] })
	if err != nil || cfg.Engine != "native" {
		t.Fatal(cfg, err)
	}
}
func TestBackendSetupRejectsRetiredSelections(t *testing.T) {
	for _, engine := range []string{"cognee", "hillock", "aims-db", "unknown"} {
		values := map[string]any{"memory_backend_engine": engine, "memory_backend_endpoint": "http://localhost:8097"}
		if _, err := ResolveBackendSettings(values, func(string) string { return "" }); err == nil {
			t.Fatalf("saved engine %q silently accepted", engine)
		}
		if _, err := ResolveBackendSettings(nil, func(k string) string {
			if k == "AIMEE_MEMORY_BACKEND" {
				return engine
			}
			return ""
		}); err == nil {
			t.Fatalf("environment engine %q silently accepted", engine)
		}
	}
	for _, values := range []map[string]any{{"memory_backend_engine": true}, {"memory_backend_endpoint": 42}, {"memory_backend_auth": false}, {"memory_backend_auth": "invalid"}} {
		if _, err := ResolveBackendSettings(values, func(string) string { return "" }); err == nil {
			t.Fatal(values)
		}
	}
}
