package delegates

import (
	"context"
	"encoding/json"
	contract "github.com/JBailes/aimee/server-go/delegate"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func retiredModelFixture(t *testing.T, kind string) (*RegistryExecutor, string, string) {
	t.Helper()
	home := t.TempDir()
	work := filepath.Join(home, "work")
	if err := os.MkdirAll(filepath.Join(work, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(home, "executed-unstructured-model")
	script := filepath.Join(home, "model")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ntouch '"+marker+"'\nprintf 'unstructured model answer'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	roster, _ := json.Marshal(map[string]any{"default_agent": "legacy", "models": []map[string]any{{"name": "legacy", "cli_kind": kind, "cli_cmd": script, "roles": []string{"code"}}}})
	if err := os.WriteFile(filepath.Join(home, "models.json"), roster, 0600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	return executor, marker, work
}
func TestUnstructuredModelCannotBePlanned(t *testing.T) {
	for _, kind := range []string{"generic", "unknown"} {
		t.Run(kind, func(t *testing.T) {
			executor, marker, _ := retiredModelFixture(t, kind)
			for _, model := range []string{"", "$random", "legacy"} {
				if names, err := executor.PlanGroup(context.Background(), []contract.GroupPlanSeat{{Role: "code", Persona: "engineer", Model: model}}); err == nil || len(names) != 0 {
					t.Fatalf("unstructured command selected as model %q: names=%v error=%v", model, names, err)
				}
			}
			if _, err := os.Stat(marker); !os.IsNotExist(err) {
				t.Fatal("planning executed command")
			}
		})
	}
}
func TestUnstructuredModelRefusesBeforeProcessOrWorkspace(t *testing.T) {
	for _, kind := range []string{"generic", "unknown"} {
		t.Run(kind, func(t *testing.T) {
			executor, marker, work := retiredModelFixture(t, kind)
			request := contract.Invocation{Version: contract.WireVersion, Model: "legacy", Role: "code", Persona: "engineer", Tools: true, MaxTurns: 0, Workdir: work, Input: contract.TaskRequest("engineer", "write task")}
			result := executor.Execute(context.Background(), request)
			if _, err := os.Stat(marker); !os.IsNotExist(err) {
				t.Error("unstructured model command executed before canonical refusal")
			}
			if result.Status != "failed" || result.Output != nil || len(result.History) != 0 || !strings.Contains(result.Error, "unsupported") {
				t.Fatalf("stdout acquired model authority: status=%s error=%s", result.Status, result.Error)
			}
			if _, err := os.Stat(marker); !os.IsNotExist(err) {
				t.Fatal("unstructured model command ran")
			}
			request.Workdir = filepath.Join(work, "not-an-owned-checkout")
			if result = executor.Execute(context.Background(), request); !strings.Contains(result.Error, "unsupported") {
				t.Fatalf("unsupported model reached workspace processing: %s", result.Error)
			}
		})
	}
}

func TestRetiredDefaultNeverRunsAndCanonicalFallbackStillNeedsHost(t *testing.T) {
	executor, marker, _ := retiredModelFixture(t, "generic")
	roster, err := executor.load()
	if err != nil {
		t.Fatal(err)
	}
	for _, model := range []string{"", "$random"} {
		if _, err := selectAgent(roster, model, "code", "engineer"); err == nil || !strings.Contains(err.Error(), "unsupported") {
			t.Fatalf("retired implicit default accepted: model=%q err=%v", model, err)
		}
	}
	roster.Models = append(roster.Models, agentEntry{Name: "native", CLIKind: "claude", CLICmd: "must-not-run", Roles: []string{"code"}})
	for _, model := range []string{"", "$random"} {
		chosen, err := selectAgent(roster, model, "code", "engineer")
		if err != nil || chosen.Name != "native" {
			t.Fatalf("canonical fallback lost: %+v %v", chosen, err)
		}
	}
	request := contract.Invocation{Version: contract.WireVersion, Model: "legacy", Role: "code", Persona: "engineer", Tools: true, MaxTurns: 0, Input: contract.TaskRequest("engineer", "write task")}
	if result := executor.Execute(t.Context(), request); result.Status != "failed" || !strings.Contains(result.Error, "unsupported") {
		t.Fatalf("unsupported missing-workspace executed: %+v", result)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("retired default command ran")
	}
}
