package memory

import (
	"context"
	"errors"
	"net/url"
	"os"
	"strings"
	"time"

	appconfig "github.com/JBailes/aimee/server-go/config"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
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
	if cfg.Engine != "native" && cfg.Engine != "cognee" && cfg.Engine != "hillock" {
		return cfg, errors.New("unknown memory backend")
	}
	if cfg.Auth == "" {
		cfg.Auth = "bearer"
	}
	if cfg.Auth != "bearer" && cfg.Auth != "none" && (cfg.Auth != "api-key" || cfg.Engine != "cognee") {
		return cfg, errors.New("unsupported memory backend authentication")
	}
	if cfg.Engine == "native" {
		cfg.Endpoint = ""
		return cfg, nil
	}
	if cfg.Endpoint == "" {
		if cfg.Engine == "hillock" {
			cfg.Endpoint = lookup("AIMEE_HILLOCK_URL")
		} else {
			cfg.Endpoint = lookup("AIMEE_COGNEE_URL")
		}
	}
	endpoint, err := url.Parse(cfg.Endpoint)
	if err != nil || (endpoint.Scheme != "http" && endpoint.Scheme != "https") || endpoint.Host == "" || endpoint.User != nil || (endpoint.Path != "" && endpoint.Path != "/") || endpoint.RawQuery != "" || endpoint.Fragment != "" {
		return cfg, errors.New("memory backend endpoint must be an HTTP(S) origin")
	}
	return cfg, nil
}

// ConfigureBackendProcess reads through the config owner's bus before serving.
// The process-local environment keeps existing adapter/egress constructors using
// the same frozen settings, with deployment environment overrides taking priority.
func ConfigureBackendProcess(ctx context.Context, socket string, principal uint32) error {
	client, err := ring.ConnectClient(ctx, socket, 1, principal)
	if err != nil {
		return err
	}
	defer client.Detach()
	caller, err := ring.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		return err
	}
	defer caller.CloseAndWait()
	config, err := appconfig.NewClient(caller, 5*time.Second)
	if err != nil {
		return err
	}
	// The config module starts with this process, not before it.
	values, err := config.SnapshotWhenServed(ctx, 30*time.Second)
	if err != nil {
		return err
	}
	return ApplyBackendSettings(values)
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
