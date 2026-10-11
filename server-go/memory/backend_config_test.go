package memory

import "testing"

func TestBackendSetupSelectionAndEnvironmentPrecedence(t *testing.T) {
	empty := func(string) string { return "" }
	for _, engine := range []string{"native", "cognee", "hillock"} {
		cfg, err := ResolveBackendSettings(map[string]any{"memory_backend_engine": engine, "memory_backend_endpoint": "http://localhost:8097", "memory_backend_auth": "bearer"}, empty)
		if err != nil || cfg.Engine != engine {
			t.Fatal(cfg, err)
		}
		if engine != "native" && cfg.Endpoint != "http://localhost:8097" {
			t.Fatal(cfg)
		}
	}
	cfg, err := ResolveBackendSettings(nil, empty)
	if err != nil || cfg.Engine != "native" {
		t.Fatal(cfg, err)
	}
	env := map[string]string{"AIMEE_MEMORY_BACKEND": "hillock", "AIMEE_HILLOCK_URL": "http://deployment:8097"}
	cfg, err = ResolveBackendSettings(map[string]any{"memory_backend_engine": "cognee", "memory_backend_endpoint": "http://saved:8000", "memory_backend_auth": "api-key"}, func(key string) string { return env[key] })
	if err != nil || cfg.Engine != "hillock" || cfg.Endpoint != env["AIMEE_HILLOCK_URL"] || cfg.Auth != "bearer" {
		t.Fatal(cfg, err)
	}
}
func TestBackendSetupRejectsUnknownSelectionsAndInvalidOrigins(t *testing.T) {
	for _, values := range []map[string]any{
		{"memory_backend_engine": "typo"},
		{"memory_backend_engine": true},
		{"memory_backend_endpoint": 42},
		{"memory_backend_engine": "hillock", "memory_backend_auth": "api-key", "memory_backend_endpoint": "http://localhost"},
		{"memory_backend_engine": "cognee", "memory_backend_auth": "invalid", "memory_backend_endpoint": "http://localhost"},
	} {
		if _, err := ResolveBackendSettings(values, func(string) string { return "" }); err == nil {
			t.Fatal(values)
		}
	}
	for _, endpoint := range []string{"", "file:///tmp", "http://user:password@localhost", "http://localhost/admin", "http://localhost?token=x", "http://localhost#fragment"} {
		if _, err := ResolveBackendSettings(map[string]any{"memory_backend_engine": "hillock", "memory_backend_endpoint": endpoint}, func(string) string { return "" }); err == nil {
			t.Fatal(endpoint)
		}
	}
}
