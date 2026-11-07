package memory

import (
	"errors"
	"os"
	"strings"
)

// BackendSettings is non-secret startup configuration. Credentials remain in Vault.
type BackendSettings struct{ Engine, Endpoint, Auth string }

func ResolveBackendSettings(values map[string]any, lookup func(string) string) (BackendSettings, error) {
	override := strings.TrimSpace(lookup("AIMEE_MEMORY_BACKEND"))
	if override == "" {
		for _, key := range []string{"memory_backend_engine", "memory_backend_endpoint", "memory_backend_auth"} {
			if value, present := values[key]; present {
				if _, ok := value.(string); !ok {
					return BackendSettings{}, errors.New("memory backend settings must be strings")
				}
			}
		}
	}
	text := func(key string) string { value, _ := values[key].(string); return strings.TrimSpace(value) }
	cfg := BackendSettings{text("memory_backend_engine"), text("memory_backend_endpoint"), text("memory_backend_auth")}
	if override != "" {
		cfg = BackendSettings{Engine: override, Endpoint: lookup("AIMEE_MEMORY_BACKEND_URL"), Auth: lookup("AIMEE_MEMORY_BACKEND_AUTH")}
	} else {
		if endpoint := lookup("AIMEE_MEMORY_BACKEND_URL"); endpoint != "" {
			cfg.Endpoint = endpoint
		}
		if auth := lookup("AIMEE_MEMORY_BACKEND_AUTH"); auth != "" {
			cfg.Auth = auth
		}
	}
	if cfg.Engine == "" || cfg.Engine == "aimee-native" {
		cfg.Engine = "native"
	}
	if cfg.Engine != "native" {
		return cfg, errors.New("in-process memory backend retired or unknown; install an independent memory module and export legacy data offline")
	}
	if cfg.Auth == "" {
		cfg.Auth = "bearer"
	}
	if cfg.Auth != "bearer" && cfg.Auth != "none" {
		return cfg, errors.New("unsupported memory backend authentication")
	}
	cfg.Endpoint = ""

	return cfg, nil
}

// ApplyBackendSettings freezes the config-owner snapshot for existing constructors.
func ApplyBackendSettings(values map[string]any) error {
	settings, err := ResolveBackendSettings(values, os.Getenv)
	if err != nil {
		return err
	}
	for key, value := range map[string]string{"AIMEE_MEMORY_BACKEND": settings.Engine, "AIMEE_MEMORY_BACKEND_URL": settings.Endpoint, "AIMEE_MEMORY_BACKEND_AUTH": settings.Auth} {
		if err = os.Setenv(key, value); err != nil {
			return err
		}
	}
	return nil
}
