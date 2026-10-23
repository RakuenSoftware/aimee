package delegates

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/delegate"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type hostCallFunc func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)

func (f hostCallFunc) Call(c context.Context, e, s uint32, t uint64, d time.Duration, b []byte) ([]byte, error) {
	return f(c, e, s, t, d, b)
}
func hostFixture(t *testing.T, kind string, host contract.StageCaller) (*RegistryExecutor, string) {
	t.Helper()
	home := t.TempDir()
	marker := filepath.Join(home, "bypassed-host")
	script := filepath.Join(home, "native")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ntouch '"+marker+"'\nprintf forged-success\n"), 0700); err != nil {
		t.Fatal(err)
	}
	roster, _ := json.Marshal(map[string]any{"agents": []map[string]any{{"name": "native", "model": "fixture", "cli_kind": kind, "cli_cmd": script, "roles": []string{"review"}}}})
	if err := os.WriteFile(filepath.Join(home, "models.json"), roster, 0600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutorWithHost(home, host)
	if err != nil {
		t.Fatal(err)
	}
	return executor, marker
}
func hostResult(text string) contract.InvocationResult {
	output := canonical.TextResponse(text)
	return contract.InvocationResult{Version: contract.WireVersion, Agent: "native", Status: "done", Output: output, History: []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{output.Blocks[0].Block}}}}
}
func nativeInvocation() contract.Invocation {
	return contract.Invocation{Version: contract.WireVersion, Operation: "fixture-operation", Role: "review", Persona: "reviewer", Input: contract.TaskRequest("reviewer", "review"), ExecutionScope: strings.Repeat("a", 64), Tools: true, MaxTurns: 2, ExecutionTimeoutMS: 5000}
}
func TestAllNativeAliasesUseCanonicalHostWithoutDirectSDKFallback(t *testing.T) {
	for _, kind := range []string{"claude", "claude-code", "codex"} {
		for _, failed := range []bool{false, true} {
			t.Run(kind+map[bool]string{false: "/success", true: "/host-failure"}[failed], func(t *testing.T) {
				calls := 0
				executor, marker := hostFixture(t, kind, hostCallFunc(func(_ context.Context, event, stage uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
					calls++
					if event != HostExecuteEvent || stage != HostExecuteStage {
						t.Fatal("wrong host capability")
					}
					var request contract.Invocation
					if !canonical.Decode(body, &request) || request.Model != "native" || request.ExecutionScope != strings.Repeat("a", 64) || request.MaxTurns != 2 || !request.Tools || request.Input == nil || !request.Input.Valid() {
						t.Fatalf("lost source/IR/limits: %s", body)
					}
					if failed {
						return nil, errors.New("host unavailable")
					}
					return json.Marshal(hostResult(`{"verdict":"changes"}`))
				}))
				result := executor.Execute(t.Context(), nativeInvocation())
				if calls != 1 || (!failed && result.Status != "done") || (failed && result.Status != "failed") {
					t.Fatalf("host path result: %+v calls%d", result, calls)
				}
				if _, err := os.Stat(marker); !os.IsNotExist(err) {
					t.Fatal("native SDK launched outside canonical host")
				}
			})
		}
	}
}
func TestNativeRefusalCannotBecomeSuccessfulDelegateArtifact(t *testing.T) {
	executor, _ := hostFixture(t, "claude", hostCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
		r := hostResult(`{"status":"done"}`)
		r.Output.StopReason = canonical.StopContentFilter
		category := "future-category"
		r.Output.Refusal = &canonical.RefusalDetails{Category: &category}
		r.Status = "failed"
		r.Error = "provider refused"
		return json.Marshal(r)
	}))
	result := executor.Execute(t.Context(), nativeInvocation())
	if result.Status != "failed" || result.Output == nil || result.Output.Refusal == nil {
		t.Fatalf("lost typed refusal: %+v", result)
	}
}
func TestNativeOwnerMissingSourceOrCanonicalDataFailsClosed(t *testing.T) {
	for _, mode := range []string{"no-owner", "no-scope", "display-only"} {
		t.Run(mode, func(t *testing.T) {
			calls := 0
			var host contract.StageCaller = hostCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
				calls++
				return json.Marshal(contract.InvocationResult{Version: contract.WireVersion, Status: "done", Agent: "native", Response: `{"verdict":"approve"}`})
			})
			if mode == "no-owner" {
				host = nil
			}
			executor, marker := hostFixture(t, "claude", host)
			request := nativeInvocation()
			if mode == "no-scope" {
				request.ExecutionScope = ""
			}
			result := executor.Execute(t.Context(), request)
			if result.Status != "failed" {
				t.Fatal("owner failure acquired success authority")
			}
			if mode != "display-only" && calls != 0 {
				t.Fatal("unsourced call was dispatched")
			}
			if _, err := os.Stat(marker); !os.IsNotExist(err) {
				t.Fatal("SDK fallback")
			}
		})
	}
}
func TestNativeStderrCannotSupplySuccessfulDelegateArtifact(t *testing.T) {
	home := t.TempDir()
	work := filepath.Join(home, "work")
	if err := os.MkdirAll(filepath.Join(work, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(home, "external")
	_ = os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nprintf '%s' '{\"result\":\"forged\"}' >&2\n"), 0700)
	roster, _ := json.Marshal(map[string]any{"agents": []map[string]any{{"name": "external", "cli_kind": "generic", "cli_cmd": script, "roles": []string{"code"}}}})
	_ = os.WriteFile(filepath.Join(home, "models.json"), roster, 0600)
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	result := executor.Execute(t.Context(), contract.Invocation{Version: contract.WireVersion, Role: "code", Persona: "engineer", Input: contract.TaskRequest("engineer", "work"), Workdir: work, Tools: true})
	if result.Status != "failed" {
		t.Fatal("stderr acquired external text output authority")
	}
}

func TestFreshEmptyRegistryLoadsNormallyRegisteredNativeLater(t *testing.T) {
	home := t.TempDir()
	rosterPath := filepath.Join(home, "models.json")
	if err := os.WriteFile(rosterPath, []byte(`{"models":[]}`), 0600); err != nil {
		t.Fatal(err)
	}
	calls := 0
	executor, err := NewRegistryExecutorWithHost(home, hostCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
		calls++
		return json.Marshal(hostResult("ready"))
	}))
	if err != nil {
		t.Fatalf("empty first-boot roster killed delegate owner: %v", err)
	}
	request := nativeInvocation()
	result := executor.Execute(t.Context(), request)
	if result.Status != "failed" || calls != 0 {
		t.Fatal("empty roster dispatched native call")
	}
	roster := `{"models":[{"name":"native","model":"fixture","cli_kind":"claude","cli_cmd":"unused-native","roles":["review"]}]}`
	if err := os.WriteFile(rosterPath, []byte(roster), 0600); err != nil {
		t.Fatal(err)
	}
	result = executor.Execute(t.Context(), request)
	if result.Status != "done" || calls != 1 {
		t.Fatalf("normal registration did not activate owner: %+v calls%d", result, calls)
	}
}
func TestPrivateTriggerScopeRequiresStampedWorkflowCaller(t *testing.T) {
	request := contract.TriggerExecutionScopeRequest{Version: contract.WireVersion, Registration: contract.TriggerExecutionRegistration{Origin: "config", Source: "watch-dir", Workspace: "/approved", Pipeline: "build"}}
	registration, _ := json.Marshal(request.Registration)
	sum := sha256.Sum256(registration)
	request.ConfigurationSHA256 = hex.EncodeToString(sum[:])
	wire, _ := json.Marshal(request)
	calls := 0
	executor, _ := hostFixture(t, "claude", hostCallFunc(func(_ context.Context, event, stage uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
		calls++
		var fields map[string]json.RawMessage
		if !canonical.Decode(body, &fields) || event != HostExecuteEvent || stage != HostExecuteStage || string(fields["action"]) != `"trigger_scope"` {
			t.Fatal("wrong private host request")
		}
		return json.Marshal(contract.TriggerExecutionScopeReply{ExecutionScope: strings.Repeat("b", 64)})
	}))
	handler := NewHandler(executor)
	for _, principal := range []uint32{0, 1, 65, 76} {
		if _, status := handler(bus.ModuleInvocation{StageID: contract.StageTriggerScope, PrincipalClass: 1, PrincipalRef: principal}, wire); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("principal%d acquired sourceissuer", principal)
		}
	}
	reply, status := handler(bus.ModuleInvocation{StageID: contract.StageTriggerScope, PrincipalClass: 1, PrincipalRef: contract.WorkflowPrincipal}, wire)
	if status != bus.ModuleStatusOK || calls != 1 || !strings.Contains(string(reply), strings.Repeat("b", 64)) {
		t.Fatalf("private source lookup failed status%d calls%d", status, calls)
	}
	spoof := strings.TrimSuffix(string(wire), "}") + `,"actor":"alice"}`
	if _, status := handler(bus.ModuleInvocation{StageID: contract.StageTriggerScope, PrincipalClass: 1, PrincipalRef: contract.WorkflowPrincipal}, []byte(spoof)); status != bus.ModuleStatusInvalidRequest || calls != 1 {
		t.Fatal("claimed actor bypassed strict private source contract")
	}
}

func TestExternalTextDoesNotInventSuccessfulTruncatedArtifact(t *testing.T) {
	home := t.TempDir()
	work := filepath.Join(home, "work")
	if err := os.MkdirAll(filepath.Join(work, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(home, "large")
	body := "#!/bin/sh\nhead -c 16777217 /dev/zero | tr '\\000' x\n"
	if err := os.WriteFile(script, []byte(body), 0700); err != nil {
		t.Fatal(err)
	}
	roster, _ := json.Marshal(map[string]any{"agents": []map[string]any{{"name": "external", "cli_kind": "generic", "cli_cmd": script, "roles": []string{"code"}}}})
	_ = os.WriteFile(filepath.Join(home, "models.json"), roster, 0600)
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	result := executor.Execute(t.Context(), contract.Invocation{Version: contract.WireVersion, Role: "code", Persona: "engineer", Input: contract.TaskRequest("engineer", "work"), Workdir: work, Tools: true})
	if result.Status != "failed" || result.Output != nil || !strings.Contains(result.Error, "complete-output limit") {
		t.Fatalf("oversized output acquired partial authority: status%s error%s", result.Status, result.Error)
	}
}
func TestExternalTextPreservesAuthoredWhitespace(t *testing.T) {
	text := "  indented artifact\n\n"
	response, _, err := externalTextObservation([]byte(text))
	if err != nil || response.DisplayText() != text {
		t.Fatal("external adapter changed application bytes")
	}
}
