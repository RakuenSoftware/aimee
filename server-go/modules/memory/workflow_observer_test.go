package memory

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestWorkflowSignals(t *testing.T) {
	for _, tt := range []struct{ command, signal, rule string }{
		{"gh pr create --base testing --title foo", "pr-target", "PRs target the `testing` branch (learned from `gh pr create --base testing`)."},
		{"gh pr create --base='develop'", "pr-target", "PRs target the `develop` branch (learned from `gh pr create --base develop`)."},
		{"gh pr create --title foo", "", ""},
		{"make test", "test-command", "Test command: `make test`"},
		{"pytest tests/", "test-command", "Test command: `pytest`"},
		{"cd src && make unit-tests", "test-command", "Test command: `make unit-tests`"},
		{"grep 'make test' README.md", "", ""},
		{"git push origin feat/login", "active-branch", "Active development on branch `feat/login` (observed via `git push origin feat/login`)."},
		{"git push origin main", "", ""}, {"git push -u origin master", "", ""}, {"git push origin HEAD", "", ""},
		{"ls -la", "", ""}, {"", "", ""},
		{"echo 'gh pr create --base testing'", "", ""}, {"echo git push origin feature", "", ""},
		{"gh pr create --baseball testing", "", ""}, {"gh pr create --base='unfinished", "", ""},
		{"gh pr create --base testing # git push origin other", "pr-target", "PRs target the `testing` branch (learned from `gh pr create --base testing`)."},
		{"git push origin $BRANCH", "", ""}, {"git push origin " + strings.Repeat("x", 128), "", ""},
	} {
		signal, rule := parseWorkflowSignal(tt.command)
		if signal != tt.signal || rule != tt.rule {
			t.Fatalf("%q: %q %q", tt.command, signal, rule)
		}
	}
	for _, label := range []string{"make unit-tests", "make check", "make test", "npm test", "pnpm test", "yarn test", "cargo test", "go test", "pytest", "ctest"} {
		if signal, rule := parseWorkflowSignal(" \t" + label + " --verbose"); signal != "test-command" || rule != "Test command: `"+label+"`" {
			t.Fatal(label, signal, rule)
		}
	}
}

func TestWorkflowWorkspace(t *testing.T) {
	for _, tt := range []struct {
		cwd   string
		roots []string
		want  string
	}{
		{"/dev/app/src", []string{"/dev/app"}, "app"}, {"/dev/app", []string{"/dev/app"}, "app"},
		{"/dev/application", []string{"/dev/app"}, ""}, {"/dev/app/nested", []string{"/dev/app", "/dev/app/nested"}, "app"},
		{"/dev/app", nil, ""},
	} {
		if got := workflowWorkspace(tt.cwd, tt.roots); got != tt.want {
			t.Fatal(tt, got)
		}
	}
}

func TestWorkflowObserverPlansAndReceipts(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		plan := runHostRuntime(t, handler, `{"operation":"workflow-plan","mode":"observe","command":"make test","cwd":"/dev/app/src","workspaces":["/dev/app"]}`)
		request := plan["request"].(map[string]any)
		if request["workspace"] != "app" || request["signal_type"] != "test-command" || request["rule"] != "Test command: `make test`" || request["observed_confidence"] != 0.6 || request["session_id"] != "post_tool_update" {
			t.Fatal(request)
		}
		for _, receipt := range []string{`{"status":"ok","id":9007199254740993}`, `{"status":"ok","id":9223372036854775807}`} {
			args, _ := json.Marshal(map[string]any{"operation": "workflow-result", "request": request, "receipt": receipt})
			result := runHostRuntime(t, handler, string(args))
			var r commandArgs
			_ = json.Unmarshal([]byte(receipt), &r)
			if result["learned"] != true || !strings.Contains(result["output"].(string), string(r["id"])) {
				t.Fatal(result)
			}
		}
		for _, receipt := range []string{"", `broken`, `{"status":"error","id":1}`, `{"status":"ok"}`, `{"status":"ok","id":1.5}`, `{"status":"ok","id":0}`, `{"status":"ok","id":9223372036854775808}`} {
			args, _ := json.Marshal(map[string]any{"operation": "workflow-result", "request": request, "receipt": receipt})
			result := runHostRuntime(t, handler, string(args))
			if result["learned"] == true || result["status"] != "error" {
				t.Fatal(receipt, result)
			}
		}
		for _, args := range []string{
			`{"operation":"workflow-plan","mode":"observe","command":"ls","cwd":"/dev/app","workspaces":["/dev/app"]}`,
			`{"operation":"workflow-plan","mode":"observe","command":"make test","cwd":"/dev/other","workspaces":["/dev/app"]}`,
		} {
			result := runHostRuntime(t, handler, args)
			if result["request"] != nil || result["learned"] != false {
				t.Fatal(result)
			}
		}
		plan = runHostRuntime(t, handler, `{"operation":"workflow-plan","mode":"explicit","project":"explicit","signal_type":"tests","rule":"token=abc","session_id":"s","cwd":"/dev/app","workspaces":["/dev/app"]}`)
		request = plan["request"].(map[string]any)
		if request["workspace"] != "explicit" || request["observed_confidence"] != float64(1) || request["session_id"] != "s" || request["rule"] != "[REDACTED]" {
			t.Fatal(request)
		}
		for _, op := range []string{"workflow-plan", "workflow-result"} {
			frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"`+op+`"}`))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(op, status)
			}
		}
	}
}
