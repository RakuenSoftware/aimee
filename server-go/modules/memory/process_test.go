package memory

import (
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	configclient "github.com/JBailes/aimee/server-go/config"
)

type processConfigCaller struct {
	t      *testing.T
	values map[string]any
	err    error
}

func (c *processConfigCaller) Call(_ context.Context, event, stage uint32, _ uint64, deadline time.Duration, body []byte) ([]byte, error) {
	c.t.Helper()
	if event != configclient.EventConfig || stage != configclient.StageConfig || deadline != 5*time.Second || string(body) != `{"operation":"snapshot"}` {
		c.t.Fatalf("unexpected configuration request: %d/%d %s %s", event, stage, deadline, body)
	}
	if c.err != nil {
		return nil, c.err
	}
	return json.Marshal(map[string]any{"ok": true, "values": c.values})
}

func TestProcessResourcesConfiguration(t *testing.T) {
	t.Setenv("EMBEDDER_URL", "https://environment.example")
	caller := &processConfigCaller{t: t}
	config, err := configclient.NewClient(caller, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	resources := processResources{config: config}
	for _, test := range []struct {
		values   map[string]any
		endpoint string
	}{
		{map[string]any{"embedder_url": "https://configured.example", "embedder_model": "local"}, "https://configured.example"},
		{map[string]any{"embedder_model": "local"}, "https://aimee-embedder:8762"},
		{map[string]any{}, "https://environment.example"},
	} {
		caller.values = test.values
		got, err := resources.EmbeddingEndpoint()
		if err != nil || got != test.endpoint {
			t.Fatal(got, err)
		}
	}
	caller.values = map[string]any{"memory_maintenance_enabled": true}
	settings, err := resources.MemorySettings()
	if err != nil || settings["memory_maintenance_enabled"] != true {
		t.Fatal(settings, err)
	}
	caller.err = errors.New("config unavailable")
	if got, err := resources.EmbeddingEndpoint(); err == nil || got != "" {
		t.Fatal("configuration failure fell back to environment", got, err)
	}
	if _, err := resources.MemorySettings(); err == nil {
		t.Fatal("configuration failure hidden")
	}
}

func TestProcessHandlerRequiresPlacementAndStore(t *testing.T) {
	socket := filepath.Join(t.TempDir(), "missing.sock")
	for _, placement := range []string{"", "global", "user"} {
		handler, err := NewProcessHandler(context.Background(), socket, placement)
		if handler != nil || err == nil || !strings.Contains(strings.ToLower(err.Error()), "placement") {
			t.Fatal(placement, err)
		}
	}
	for _, placement := range []string{"server", "kb"} {
		handler, err := NewProcessHandler(context.Background(), socket, placement)
		if handler != nil || err == nil {
			t.Fatal("missing store became a default handler", placement, err)
		}
		if handler, err = NewProcessHandler(nil, socket, placement); handler != nil || err == nil {
			t.Fatal("nil lifecycle accepted")
		}
		if handler, err = NewProcessHandler(context.Background(), "", placement); handler != nil || err == nil {
			t.Fatal("missing socket accepted")
		}
	}
}
