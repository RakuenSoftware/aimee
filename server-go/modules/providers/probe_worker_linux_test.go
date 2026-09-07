//go:build linux

package providers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestCLIProbeIsBoundedAndIsolated(t *testing.T) {
	m, home, _ := manager(t)
	script := filepath.Join(home, "diagnostic.sh")
	os.WriteFile(script, []byte("#!/bin/sh\n[ \"$PWD\" != \"$HOME\" ] || exit 3\nread prompt\nprintf 'ok\\n'\n"), 0700)
	config := object{"models": []object{{"name": "cli", "provider": "generic", "backend": "provider-cli", "cli_cmd": script, "primary_only": true, "max_turns": 100, "session_reuse": true}}}
	b, _ := json.Marshal(config)
	os.WriteFile(filepath.Join(home, "models.json"), b, 0600)
	out := runConfiguredProbe(m.store, "cli")
	if !boolean(out, "execution_ok", false) {
		t.Fatal(out)
	}
	os.WriteFile(script, []byte("#!/bin/sh\nsleep 5\n"), 0700)
	config["models"].([]object)[0]["timeout_ms"] = 30
	b, _ = json.Marshal(config)
	os.WriteFile(filepath.Join(home, "models.json"), b, 0600)
	start := time.Now()
	out = runConfiguredProbe(m.store, "cli")
	if boolean(out, "execution_ok", true) || time.Since(start) > 2*time.Second {
		t.Fatal(out, time.Since(start))
	}
	// CLI --no-run must not perform HTTP discovery or launch the diagnostic.
	out, err := m.Manage(context.Background(), Request{Operation: "model.probe", Arguments: object{"args": []string{"cli", "--no-run"}}})
	if err != nil || boolean(out, "execution_tested", true) {
		t.Fatal(out, err)
	}
}
func TestUnknownBackendNeverMakesNetworkRequest(t *testing.T) {
	m, home, _ := manager(t)
	os.WriteFile(filepath.Join(home, "models.json"), []byte(`{"models":[{"name":"bad","model":"m","backend":"future-backend","endpoint":"http://localhost/v1"}]}`), 0600)
	network := &fixtureNetwork{}
	m.SetEgress(network)
	out := call(t, m, "model.probe", object{"args": []string{"bad"}})
	if len(network.requests) != 0 || boolean(out, "execution_ok", true) {
		t.Fatal(out)
	}
}

func TestProbeOutputRequiresSuccessfulFinalMessage(t *testing.T) {
	for _, output := range []string{`{"type":"system","message":"started"}`, `{"type":"result","result":"error","is_error":true}`, `not json`} {
		if validProbeOutput("claude", output) {
			t.Fatal(output)
		}
	}
	if !validProbeOutput("claude", `{"type":"result","result":"ok"}`) || !validProbeOutput("codex", `{"type":"item.completed","item":{"type":"agent_message","text":"ok"}}`) {
		t.Fatal("valid diagnostic rejected")
	}
}
