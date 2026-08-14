package delegates

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
)

func TestMain(m *testing.M) {
	if handled, code := RunWatchdog(os.Args); handled {
		os.Exit(code)
	}
	os.Exit(m.Run())
}

type fixedExecutor struct {
	request delegatecontract.Invocation
	result  delegatecontract.InvocationResult
}

type blockingExecutor struct{}

type planningExecutor struct {
	seats  []delegatecontract.GroupPlanSeat
	models []string
	err    error
}

func (e *planningExecutor) Execute(_ context.Context,
	_ delegatecontract.Invocation) delegatecontract.InvocationResult {
	return delegatecontract.InvocationResult{Version: delegatecontract.WireVersion, Status: "done"}
}

func (e *planningExecutor) PlanGroup(_ context.Context,
	seats []delegatecontract.GroupPlanSeat) ([]string, error) {
	e.seats = append([]delegatecontract.GroupPlanSeat(nil), seats...)
	return append([]string(nil), e.models...), e.err
}

func (blockingExecutor) Execute(ctx context.Context, _ delegatecontract.Invocation) delegatecontract.InvocationResult {
	<-ctx.Done()
	return delegatecontract.InvocationResult{Version: 2, Status: "failed", Error: ctx.Err().Error()}
}

func (e *fixedExecutor) Execute(_ context.Context, request delegatecontract.Invocation) delegatecontract.InvocationResult {
	e.request = request
	return e.result
}

func TestExecutionStageCanonicalizesRoleAndReturnsTerminalStatus(t *testing.T) {
	executor := &fixedExecutor{result: delegatecontract.InvocationResult{Version: 2, Status: "done", Response: "complete"}}
	request, _ := json.Marshal(delegatecontract.Invocation{Version: 2, Role: "implement", Persona: "engineer", Prompt: "do it"})
	reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageInvoke}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if executor.request.Role != "code" {
		t.Fatalf("role = %q", executor.request.Role)
	}
	var result delegatecontract.InvocationResult
	if json.Unmarshal(reply, &result) != nil || result.Status != "done" || result.Response != "complete" {
		t.Fatalf("result = %s", reply)
	}
}

func TestExecutionStageAppliesProducerDeadline(t *testing.T) {
	request, _ := json.Marshal(delegatecontract.Invocation{Version: 2, Role: "review",
		Persona: "reviewer", Prompt: "inspect", ExecutionTimeoutMS: 20})
	started := time.Now()
	reply, status := NewHandler(blockingExecutor{})(bus.ModuleInvocation{StageID: StageInvoke}, request)
	if status != bus.ModuleStatusOK || time.Since(started) > time.Second {
		t.Fatalf("status = %d elapsed = %s", status, time.Since(started))
	}
	var result delegatecontract.InvocationResult
	if json.Unmarshal(reply, &result) != nil || result.Status != "failed" ||
		!strings.Contains(result.Error, "deadline exceeded") {
		t.Fatalf("result = %s", reply)
	}
}

func TestGroupPlanStageCanonicalizesAndReturnsAssignments(t *testing.T) {
	executor := &planningExecutor{models: []string{"security-agent", "qa-agent"}}
	request, _ := json.Marshal(delegatecontract.GroupPlan{Version: delegatecontract.WireVersion,
		Seats: []delegatecontract.GroupPlanSeat{
			{Role: "reviewer", Persona: "security"},
			{Role: "review", Persona: "qa"},
		}})
	reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageGroupPlan}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(executor.seats) != 2 || executor.seats[0].Role != "review" {
		t.Fatalf("planned seats = %+v", executor.seats)
	}
	var result delegatecontract.GroupPlanResult
	if err := json.Unmarshal(reply, &result); err != nil ||
		!slices.Equal(result.Models, executor.models) {
		t.Fatalf("result = %s (%v)", reply, err)
	}
}

func TestGroupPlanStagePreservesTypedCapacityFailure(t *testing.T) {
	executor := &planningExecutor{err: fmt.Errorf("%w: saturated", delegatecontract.ErrDelegateCapacity)}
	request, _ := json.Marshal(delegatecontract.GroupPlan{Version: delegatecontract.WireVersion,
		Seats: []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "qa"}}})
	reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageGroupPlan}, request)
	var result delegatecontract.GroupPlanResult
	if status != bus.ModuleStatusOK || json.Unmarshal(reply, &result) != nil ||
		!delegatecontract.IsCapacityBackpressure(errors.New(result.Error)) || len(result.Models) != 0 {
		t.Fatalf("capacity failure did not cross the module boundary: status=%d reply=%q", status, reply)
	}
}

func TestGroupPlanStageKeepsNonCapacityFailureAsTransportError(t *testing.T) {
	executor := &planningExecutor{err: errors.New("registry unavailable")}
	request, _ := json.Marshal(delegatecontract.GroupPlan{Version: delegatecontract.WireVersion,
		Seats: []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "qa"}}})
	reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageGroupPlan}, request)
	if status != bus.ModuleStatusInternal || len(reply) != 0 {
		t.Fatalf("non-capacity planner failure became a domain reply: status=%d reply=%q", status, reply)
	}
}

func TestRegistryExecutorRunsArgvWithoutShell(t *testing.T) {
	home := t.TempDir()
	script := filepath.Join(home, "delegate-helper")
	if err := os.WriteFile(script, []byte("#!/bin/sh\nread prompt\nprintf 'answer:%s' \"$prompt\"\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	registry := map[string]any{"default_agent": "helper", "agents": []map[string]any{{
		"name": "helper", "model": "test", "cli_kind": "generic", "cli_cmd": script, "enabled": true,
		"roles": []string{"code"},
	}}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	workdir := filepath.Join(home, "worktree")
	if err := os.MkdirAll(filepath.Join(workdir, ".git"), 0o700); err != nil {
		t.Fatal(err)
	}
	result := executor.Execute(t.Context(), delegatecontract.Invocation{Version: 2, Role: "code",
		Persona: "security", Prompt: "inspect", Workdir: workdir, Tools: true})
	if result.Status != "done" || !strings.Contains(result.Response, "You are acting as security.") {
		t.Fatalf("result = %+v", result)
	}
}

func TestRegistryExecutorEnforcesMaxParallelWithoutPoisoningAgentHealth(t *testing.T) {
	home := t.TempDir()
	workdir := filepath.Join(home, "worktree")
	if err := os.MkdirAll(filepath.Join(workdir, ".git"), 0o700); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(home, "delegate-helper")
	scriptBody := "#!/bin/sh\n" +
		"touch '" + home + "/started.'$$\n" +
		"while [ ! -f '" + home + "/release' ]; do sleep 0.01; done\n" +
		"printf done\n"
	if err := os.WriteFile(script, []byte(scriptBody), 0o700); err != nil {
		t.Fatal(err)
	}
	registry := map[string]any{"default_agent": "helper", "agents": []map[string]any{{
		"name": "helper", "cli_kind": "generic", "cli_cmd": script, "roles": []string{"code"},
		"max_parallel": 1, "delegate_available": true,
	}}}
	body, _ := json.Marshal(registry)
	registryPath := filepath.Join(home, "models.json")
	if err := os.WriteFile(registryPath, body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	request := delegatecontract.Invocation{Version: 2, Role: "code", Persona: "engineer",
		Prompt: "work", Workdir: workdir, Tools: true}
	results := make(chan delegatecontract.InvocationResult, 2)
	for range 2 {
		go func() { results <- executor.Execute(t.Context(), request) }()
	}
	deadline := time.Now().Add(time.Second)
	for {
		matches, _ := filepath.Glob(filepath.Join(home, "started.*"))
		if len(matches) == 1 {
			break
		}
		if len(matches) > 1 || time.Now().After(deadline) {
			t.Fatalf("started %d delegates with max_parallel=1", len(matches))
		}
		time.Sleep(10 * time.Millisecond)
	}
	time.Sleep(50 * time.Millisecond)
	if matches, _ := filepath.Glob(filepath.Join(home, "started.*")); len(matches) != 1 {
		t.Fatalf("started %d delegates before the slot released", len(matches))
	}
	if _, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{
		{Role: "code", Persona: "engineer"},
	}); !errors.Is(err, delegatecontract.ErrDelegateCapacity) || errors.Is(err, delegatecontract.ErrDelegateTerminal) {
		t.Fatalf("occupied agent was not classified as retryable capacity: %v", err)
	}
	healthAfter, err := os.ReadFile(registryPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(healthAfter, body) {
		t.Fatalf("capacity response mutated authoritative health: before=%s after=%s", body, healthAfter)
	}
	if err := os.WriteFile(filepath.Join(home, "release"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	for range 2 {
		result := <-results
		if result.Status != "done" {
			t.Fatalf("delegate result = %+v", result)
		}
	}
	models, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{
		{Role: "code", Persona: "engineer"},
	})
	if err != nil || !slices.Equal(models, []string{"helper"}) {
		t.Fatalf("capacity response poisoned subsequent agent health: models=%v err=%v", models, err)
	}
}

func TestRegistryExecutorTypesCallerDeadlineAsExecutionDeadline(t *testing.T) {
	home := t.TempDir()
	workdir := filepath.Join(home, "worktree")
	if err := os.MkdirAll(filepath.Join(workdir, ".git"), 0o700); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(home, "slow-delegate")
	if err := os.WriteFile(script, []byte("#!/bin/sh\nsleep 2\nprintf done\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	registry := map[string]any{"agents": []map[string]any{{
		"name": "helper", "cli_kind": "generic", "cli_cmd": script, "roles": []string{"code"},
	}}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Millisecond)
	defer cancel()
	result := executor.Execute(ctx, delegatecontract.Invocation{Version: 2, Role: "code",
		Persona: "engineer", Prompt: "work", Workdir: workdir, Tools: true})
	if result.Status != "failed" || !delegatecontract.IsExecutionDeadline(errors.New(result.Error)) ||
		delegatecontract.IsCapacityDeadline(errors.New(result.Error)) {
		t.Fatalf("caller deadline was not typed as an execution deadline: %+v", result)
	}
}

func TestPlanGroupPreservesDiversityEligibilityAndCapacity(t *testing.T) {
	home := t.TempDir()
	registry := map[string]any{"agents": []map[string]any{
		{"name": "a", "provider": "p1", "model": "m1", "cli_kind": "codex", "cli_cmd": "codex",
			"roles": []string{"review"}, "personas": []string{"all"}, "max_parallel": 2},
		{"name": "b", "provider": "p2", "model": "m2", "cli_kind": "codex", "cli_cmd": "codex",
			"roles": []string{"review"}, "personas": []string{"security"}, "max_parallel": 1},
		{"name": "c", "provider": "p1", "model": "m3", "cli_kind": "codex", "cli_cmd": "codex",
			"roles": []string{"review"}, "personas": []string{"all"}, "max_parallel": 1},
		{"name": "primary", "provider": "p3", "cli_kind": "codex", "cli_cmd": "codex",
			"roles": []string{"review"}, "primary_only": true, "max_parallel": 9},
	}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	models, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{
		{Role: "review", Persona: "security"},
		{Role: "review", Persona: "security"},
		{Role: "review", Persona: "qa"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(models, []string{"a", "b", "c"}) {
		t.Fatalf("diverse group plan = %v", models)
	}
	if _, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{
		{Role: "review", Persona: "qa", Model: "c"},
		{Role: "review", Persona: "qa", Model: "c"},
	}); err == nil || !errors.Is(err, delegatecontract.ErrDelegateCapacity) ||
		!strings.Contains(err.Error(), "max_parallel") || errors.Is(err, delegatecontract.ErrDelegateTerminal) {
		t.Fatalf("over-capacity pinned plan error = %v", err)
	}
}

func TestPlanGroupExcludesUnavailableLocalBackendAndKeepsHealthyFallback(t *testing.T) {
	home := t.TempDir()
	registry := map[string]any{"agents": []map[string]any{
		{"name": "local-gemma4", "provider": "local", "model": "aimee-synth", "cli_kind": "generic",
			"cli_cmd": "local-helper", "enabled": true, "delegate_available": false,
			"roles": []string{"review"}, "personas": []string{"all"}, "max_parallel": 2},
		{"name": "healthy-remote", "provider": "remote", "model": "reviewer", "cli_kind": "codex",
			"cli_cmd": "codex", "enabled": true, "delegate_available": true,
			"roles": []string{"review"}, "personas": []string{"all"}, "max_parallel": 2},
	}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	models, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "qa"}})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(models, []string{"healthy-remote"}) {
		t.Fatalf("authoritative health did not retain only the healthy fallback: %v", models)
	}
}

func TestPlanGroupDoesNotCallMissingEligibilityCapacity(t *testing.T) {
	home := t.TempDir()
	registry := map[string]any{"agents": []map[string]any{
		{"name": "unavailable", "cli_kind": "codex", "cli_cmd": "codex", "delegate_available": false,
			"roles": []string{"review"}, "personas": []string{"all"}},
		{"name": "wrong-role", "cli_kind": "codex", "cli_cmd": "codex", "delegate_available": true,
			"roles": []string{"code"}, "personas": []string{"all"}},
	}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	_, err = executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "qa"}})
	if err == nil || delegatecontract.IsCapacityBackpressure(err) ||
		errors.Is(err, delegatecontract.ErrDelegateCapacity) {
		t.Fatalf("missing eligible healthy backend was mislabeled as capacity: %v", err)
	}
}

func TestAgentLimiterTypesDeadlineReachedWhileWaitingForCapacity(t *testing.T) {
	limiter := &agentLimiter{max: 1, changed: make(chan struct{})}
	release, err := limiter.acquire(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Millisecond)
	defer cancel()
	_, err = limiter.acquire(ctx)
	if !errors.Is(err, delegatecontract.ErrDelegateCapacityDeadline) ||
		!errors.Is(err, context.DeadlineExceeded) || errors.Is(err, delegatecontract.ErrDelegateTerminal) {
		t.Fatalf("capacity wait deadline lost its identity: %v", err)
	}
}

func TestAgentLimiterPreservesCancellationWhileWaitingForCapacity(t *testing.T) {
	limiter := &agentLimiter{max: 1, changed: make(chan struct{})}
	release, err := limiter.acquire(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	_, err = limiter.acquire(ctx)
	if !errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) ||
		delegatecontract.IsCapacityDeadline(err) || delegatecontract.IsExecutionDeadline(err) {
		t.Fatalf("capacity wait cancellation was retyped as a deadline: %v", err)
	}
}

func TestExecutorCommandRejectsShellOperators(t *testing.T) {
	if _, err := splitCommand("delegate; touch /tmp/escaped"); err == nil {
		t.Fatal("shell operator accepted")
	}
}

func TestSelectAgentFallsBackFromHTTPDefaultButHonoursExplicitModel(t *testing.T) {
	registry := agentRegistry{DefaultAgent: "remote", Agents: []agentEntry{
		{Name: "remote", Model: "remote-model", Backend: "openai", Roles: []string{"review"}},
		{Name: "local", Model: "local-model", CLIKind: "codex", CLICmd: "codex", Roles: []string{"review"}},
	}}
	selected, err := selectAgent(registry, "", "review", "security")
	if err != nil || selected.Name != "local" {
		t.Fatalf("default selection = %+v, %v", selected, err)
	}
	if _, err := selectAgent(registry, "remote-model", "review", "security"); err == nil {
		t.Fatal("explicit HTTP-only model silently fell back to another agent")
	}
}

func TestSelectAgentExcludesUnavailableAgents(t *testing.T) {
	unavailable, available := false, true
	registry := agentRegistry{DefaultAgent: "offline", Agents: []agentEntry{
		{Name: "offline", Model: "offline-model", CLIKind: "codex", CLICmd: "codex",
			Roles: []string{"review"}, Available: &unavailable},
		{Name: "online", Model: "online-model", CLIKind: "codex", CLICmd: "codex",
			Roles: []string{"review"}, Available: &available},
	}}
	selected, err := selectAgent(registry, "", "review", "security")
	if err != nil || selected.Name != "online" {
		t.Fatalf("unbound selection = %+v, %v", selected, err)
	}
	if _, err := selectAgent(registry, "offline-model", "review", "security"); err == nil {
		t.Fatal("explicit selector launched an unavailable agent")
	}
}

func TestSelectAgentEnforcesRoleAndPersona(t *testing.T) {
	registry := agentRegistry{Agents: []agentEntry{{Name: "reviewer", CLIKind: "claude",
		CLICmd: "claude", Roles: []string{"review"}, Personas: []string{"security"}}}}
	if _, err := selectAgent(registry, "reviewer", "code", "security"); err == nil {
		t.Fatal("agent accepted an undeclared role")
	}
	if _, err := selectAgent(registry, "reviewer", "review", "qa"); err == nil {
		t.Fatal("agent accepted an undeclared persona")
	}
	if _, err := selectAgent(registry, "reviewer", "review", "security"); err != nil {
		t.Fatal(err)
	}
}

func TestSelectAgentAlwaysRejectsPrimaryOnly(t *testing.T) {
	registry := agentRegistry{DefaultAgent: "primary", Agents: []agentEntry{{Name: "primary",
		CLIKind: "codex", CLICmd: "codex", Roles: []string{"review"}, PrimaryOnly: true}}}
	if _, err := selectAgent(registry, "", "review", "security"); err == nil {
		t.Fatal("primary-only default was selected for delegation")
	}
	if _, err := selectAgent(registry, "primary", "review", "security"); err == nil {
		t.Fatal("explicit primary-only agent was selected for delegation")
	}
}

func TestRegistryRejectsNegativeMaxParallel(t *testing.T) {
	home := t.TempDir()
	if err := os.WriteFile(filepath.Join(home, "models.json"), []byte(
		`{"agents":[{"name":"bad","cli_cmd":"codex","roles":["code"],"max_parallel":-1}]}`),
		0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewRegistryExecutor(home); err == nil || !strings.Contains(err.Error(), "negative max_parallel") {
		t.Fatalf("negative max_parallel error = %v", err)
	}
}

func TestExecutorArgvFailsClosedWhenToolsCannotBeDisabled(t *testing.T) {
	_, err := executorArgv(agentEntry{CLIKind: "codex", CLICmd: "codex"},
		delegatecontract.Invocation{Role: "review", Tools: false})
	if err == nil || !strings.Contains(err.Error(), "tools-disabled") {
		t.Fatalf("codex tools-disabled error = %v", err)
	}
	argv, err := executorArgv(agentEntry{CLIKind: "claude", CLICmd: "claude"},
		delegatecontract.Invocation{Role: "review", Tools: false})
	tools := slices.Index(argv, "--tools")
	if err != nil || tools < 0 || tools+1 >= len(argv) || argv[tools+1] != "" {
		t.Fatalf("claude tools-disabled argv = %q, %v", argv, err)
	}
}

func TestTurnMonitorCancelsAtTheConfiguredCap(t *testing.T) {
	ctx, cancel := context.WithCancel(t.Context())
	var output bytes.Buffer
	monitor := newTurnMonitor("claude", 1, cancel, &output)
	_, _ = monitor.Write([]byte("{\"type\":\"assistant\"}\n{\"type\":\"assistant\"}\n"))
	if !monitor.Exceeded() || ctx.Err() == nil {
		t.Fatal("Claude turn cap did not cancel execution")
	}
	ctx, cancel = context.WithCancel(t.Context())
	monitor = newTurnMonitor("codex", 1, cancel, &output)
	_, _ = monitor.Write([]byte("{\"type\":\"item.completed\",\"item\":{\"type\":\"command_execution\"}}\n"))
	if !monitor.Exceeded() || ctx.Err() == nil {
		t.Fatal("Codex turn cap did not cancel execution")
	}
}

func TestWatchdogKillsAndReapsDeepProcessTreeWhenProducerDies(t *testing.T) {
	controlRead, controlWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer controlRead.Close()
	outputRead, outputWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer outputRead.Close()
	defer outputWrite.Close()
	done := make(chan int, 1)
	go func() {
		done <- runWatchdog(controlRead,
			[]string{"/bin/sh", "-c", `/bin/sh -c 'sleep 30 & child=$!; printf "%s %s %s\n" "$PPID" "$$" "$child"; wait' & wait`},
			strings.NewReader(""), outputWrite, outputWrite)
	}()
	var leader, child, grandchild int
	if _, err := fmt.Fscan(outputRead, &leader, &child, &grandchild); err != nil {
		t.Fatal(err)
	}
	if leader <= 0 || child <= 0 || grandchild <= 0 || leader == child || child == grandchild {
		t.Fatalf("invalid process tree %d -> %d -> %d", leader, child, grandchild)
	}
	if err := controlWrite.Close(); err != nil {
		t.Fatal(err)
	}
	if code := <-done; code != 125 {
		t.Fatalf("watchdog exit = %d", code)
	}
	for _, pid := range []int{leader, child, grandchild} {
		if err := syscall.Kill(pid, 0); !errors.Is(err, syscall.ESRCH) {
			t.Fatalf("delegate process %d survived producer death: %v", pid, err)
		}
	}
}

func TestExecutorCancellationTerminatesDelegateProcessGroup(t *testing.T) {
	home := t.TempDir()
	workdir := filepath.Join(home, "worktree")
	if err := os.MkdirAll(filepath.Join(workdir, ".git"), 0o700); err != nil {
		t.Fatal(err)
	}
	pidFile := filepath.Join(home, "delegate.pid")
	script := filepath.Join(home, "delegate-helper")
	scriptBody := "#!/bin/sh\nprintf '%s' \"$$\" > '" + pidFile + "'\nsleep 30 & wait\n"
	if err := os.WriteFile(script, []byte(scriptBody), 0o700); err != nil {
		t.Fatal(err)
	}
	registry := map[string]any{"agents": []map[string]any{{
		"name": "helper", "cli_kind": "generic", "cli_cmd": script,
		"roles": []string{"code"},
	}}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	request := delegatecontract.Invocation{Version: delegatecontract.WireVersion,
		Role: "code", Persona: "engineer", Prompt: "work", Workdir: workdir, Tools: true}
	ctx, cancel := context.WithCancel(t.Context())
	done := make(chan delegatecontract.InvocationResult, 1)
	go func() {
		done <- executor.Execute(ctx, request)
	}()
	deadline := time.Now().Add(3 * time.Second)
	var pid int
	for pid == 0 && time.Now().Before(deadline) {
		contents, readErr := os.ReadFile(pidFile)
		if readErr == nil {
			_, _ = fmt.Sscanf(string(contents), "%d", &pid)
		}
		if pid == 0 {
			time.Sleep(10 * time.Millisecond)
		}
	}
	if pid == 0 {
		t.Fatal("delegate process did not start")
	}
	cancel()
	select {
	case result := <-done:
		if result.Status != "failed" || strings.TrimSpace(result.Error) == "" {
			t.Fatalf("cancelled result = %+v", result)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("module call did not acknowledge cancellation")
	}
	if err := syscall.Kill(pid, 0); !errors.Is(err, syscall.ESRCH) {
		t.Fatalf("delegate process %d survived module cancellation: %v", pid, err)
	}
}
