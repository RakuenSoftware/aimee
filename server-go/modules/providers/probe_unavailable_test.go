package providers

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestNativeProbeNeverExecutesOrClaimsInference(t *testing.T) {
	for _, kind := range []string{"claude", "codex", "generic"} {
		for _, noRun := range []bool{false, true} {
			t.Run(kind+map[bool]string{false: "/requested", true: "/metadata"}[noRun], func(t *testing.T) {
				m, home, _ := manager(t)
				marker := filepath.Join(home, "executed")
				script := filepath.Join(home, "model")
				if err := os.WriteFile(script, []byte("#!/bin/sh\ntouch '"+marker+"'\nprintf ok\n"), 0700); err != nil {
					t.Fatal(err)
				}
				roster, _ := json.Marshal(object{"models": []object{{"name": "native", "backend": "provider-cli", "cli_kind": kind, "cli_cmd": script, "cli_args": []string{"--anything"}, "tools_enabled": true, "max_turns": 0}}})
				if err := os.WriteFile(filepath.Join(home, "models.json"), roster, 0600); err != nil {
					t.Fatal(err)
				}
				network := &fixtureNetwork{}
				m.SetEgress(network)
				args := []string{"native"}
				if noRun {
					args = append(args, "--no-run")
				}
				out := call(t, m, "model.probe", object{"args": args})
				if out["execution_tested"] != false || out["execution_ok"] != nil || out["model_available"] != nil || str(out, "execution_message") != "Test this native model in a conversation." {
					t.Fatal(out)
				}
				if len(network.requests) != 0 {
					t.Fatal("native probe sent HTTP")
				}
				if _, err := os.Stat(marker); !os.IsNotExist(err) {
					t.Fatal("native probe executed command", err)
				}
			})
		}
	}
}
