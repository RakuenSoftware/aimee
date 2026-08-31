package executionpolicy

import (
	"encoding/json"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func invoke(t *testing.T, handler bus.ModuleHandler, body map[string]any) response {
	t.Helper()
	wire, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	result, status := handler(bus.ModuleInvocation{StageID: StageTool}, wire)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	var decision response
	if err := json.Unmarshal(result, &decision); err != nil {
		t.Fatal(err)
	}
	return decision
}

func base(tool string, arguments map[string]any) map[string]any {
	return map[string]any{
		"tool": tool, "side_effect": "filesystem",
		"arguments": arguments,
		"computer_use": map[string]any{
			"enabled": true, "default_navigation": "approve",
			"redact_sensitive_screenshots": true,
			"allowed_domains":              []string{"localhost", "*.example.com"},
		},
	}
}

func TestRequiredPolicyDecisions(t *testing.T) {
	policy := &operatorPolicy{
		ForbiddenCommands: []string{"rm -rf"},
		ToolRules:         []toolRule{{PathPrefix: "/safe", AllowedTools: []string{"read_file"}}},
		ApprovalLevels:    map[string]string{"network": "block"},
	}
	handler := newHandler(func() (*operatorPolicy, error) { return policy, nil })

	tests := []struct {
		name    string
		body    map[string]any
		allowed bool
	}{
		{"ordinary action allowed", base("read_file", map[string]any{"path": "/safe/a.txt"}), true},
		{"forbidden command denied", base("bash", map[string]any{"command": "rm -rf build"}), false},
		{"path tool denied", base("write_file", map[string]any{"path": "/safe/a.txt"}), false},
		{"source discovery denied", base("bash", map[string]any{"command": "rg policy_check_tool"}), false},
		{"specific file search allowed", base("bash", map[string]any{"command": "rg needle src/file.c"}), true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			decision := invoke(t, handler, test.body)
			if decision.Allowed != test.allowed || decision.Reason == "" {
				t.Fatalf("decision = %+v, allowed want %v", decision, test.allowed)
			}
		})
	}
}

func TestComputerUsePolicy(t *testing.T) {
	handler := newHandler(func() (*operatorPolicy, error) { return nil, nil })
	allowed := invoke(t, handler, base("computer-use:navigate",
		map[string]any{"url": "https://docs.example.com/page"}))
	if !allowed.Allowed {
		t.Fatalf("allowlisted navigation denied: %+v", allowed)
	}
	deniedBody := base("computer-use:navigate", map[string]any{"url": "https://outside.invalid"})
	denied := invoke(t, handler, deniedBody)
	if denied.Allowed {
		t.Fatalf("off-list navigation allowed: %+v", denied)
	}
	disabledBody := base("computer-use:click", map[string]any{"label": "button"})
	disabledBody["computer_use"].(map[string]any)["enabled"] = false
	if invoke(t, handler, disabledBody).Allowed {
		t.Fatal("disabled computer-use action allowed")
	}
}

func TestSourceDiscoveryParity(t *testing.T) {
	tests := []struct {
		command string
		blocked bool
	}{
		{"grep -r foo src/", true},
		{"grep -rn TODO .", true},
		{"grep foo_func src/agent_policy.c", false},
		{"grep -n foo_func src/agent_policy.c", false},
		{"rg -n foo_func README.md", false},
		{"grep -R foo_func src/agent_policy.c", false},
		{"grep ERROR /var/log/syslog", false},
		{"grep -i fail /tmp/output.txt", false},
		{"grep pattern /etc/nginx/nginx.conf", false},
		{"rg policy_check_tool", true},
		{"rg -l foo_func src/", true},
		{"  rg pattern", true},
		{"rg \"foo.bar\" src/", true},
		{"rg \"Acceptance Criteria\" docs/proposals/pending", false},
		{"grep -R \"Proposal\" ./docs", false},
		{"rg pattern 'docs/proposals/pending'", false},
		{"rg token /home/user/docs/secret", true},
		{"ripgrep foo src/", true},
		{"find . -name '*.c'", true},
		{"find src -type f", true},
		{"find docs/proposals/pending -type f", false},
		{"find ./docs -name '*.md'", false},
		{"find /tmp -name '*.tmp' -delete", false},
		{"find /var/log -mtime -1", false},
		{"find /home/user/proj -name '*.c'", true},
		{"find /opt/src -name \"*.h\"", true},
		{"find /home -name '*.py'", true},
		{"cat src/agent_policy.c", false},
		{"cat README.md", false},
		{"ls src/", false},
		{"make -j4", false},
		{"git status", false},
		{"aimee index find foo", false},
		{"", false},
	}
	for _, test := range tests {
		t.Run(test.command, func(t *testing.T) {
			if got := sourceDiscovery(test.command); got != test.blocked {
				t.Fatalf("sourceDiscovery(%q) = %v, want %v", test.command, got, test.blocked)
			}
		})
	}
}

func TestPolicyLoadFailureAndMalformedInputFailClosed(t *testing.T) {
	handler := newHandler(func() (*operatorPolicy, error) { return nil, errors.New("bad policy") })
	if invoke(t, handler, base("read_file", map[string]any{"path": "/x"})).Allowed {
		t.Fatal("unreadable policy allowed action")
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageTool}, []byte(`{"tool":"bash"}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed status = %d", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageTool + 1}, []byte(`{}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageTool}, []byte(`{"tool":"bash","arguments":{}} {}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("trailing-json status = %d", status)
	}
	invalidArguments := base("read_file", map[string]any{"path": "/x"})
	invalidArguments["arguments"] = nil
	if invoke(t, newHandler(func() (*operatorPolicy, error) { return nil, nil }), invalidArguments).Allowed {
		t.Fatal("null arguments allowed action")
	}
}
