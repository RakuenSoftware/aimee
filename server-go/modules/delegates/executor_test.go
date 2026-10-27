package delegates

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
)

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
	return delegatecontract.InvocationResult{Version: delegatecontract.WireVersion, Status: "failed", Error: ctx.Err().Error()}
}

func (e *fixedExecutor) Execute(_ context.Context, request delegatecontract.Invocation) delegatecontract.InvocationResult {
	e.request = request
	result := e.result
	if result.Status == "done" {
		result.History = []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{result.Output.Blocks[0].Block}}}
	}
	return result
}

func TestExecutionStageCanonicalizesRoleAndReturnsTerminalStatus(t *testing.T) {
	executor := &fixedExecutor{result: delegatecontract.InvocationResult{Version: delegatecontract.WireVersion, Status: "done", Output: canonical.TextResponse("complete")}}
	request, _ := json.Marshal(delegatecontract.Invocation{Version: delegatecontract.WireVersion, Role: "implement", Persona: "engineer", Input: canonical.TaskRequest("fixture", "do it")})
	reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageInvoke}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if executor.request.Role != "code" {
		t.Fatalf("role = %q", executor.request.Role)
	}
	var result delegatecontract.InvocationResult
	if json.Unmarshal(reply, &result) != nil || result.Status != "done" || result.Output == nil || result.Output.DisplayText() != "complete" {
		t.Fatalf("result = %s", reply)
	}
}

func TestExecutionStageAppliesProducerDeadline(t *testing.T) {
	request, _ := json.Marshal(delegatecontract.Invocation{Version: delegatecontract.WireVersion, Role: "review",
		Persona: "reviewer", Input: canonical.TaskRequest("fixture", "inspect"), ExecutionTimeoutMS: 20})
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

func TestExecutionStageRejectsMalformedTermination(t *testing.T) {
	executor := &fixedExecutor{result: delegatecontract.InvocationResult{
		Version: delegatecontract.WireVersion, Status: "failed", Error: "bad termination",
		Termination: &delegatecontract.TerminationDiagnostic{
			Kind: delegatecontract.TerminationTurnCap, MaxTurns: 2,
		},
	}}
	request, _ := json.Marshal(delegatecontract.Invocation{Version: delegatecontract.WireVersion,
		Role: "review", Persona: "reviewer", Input: canonical.TaskRequest("fixture", "inspect")})
	if reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageInvoke}, request); status != bus.ModuleStatusInternal || reply != nil {
		t.Fatalf("malformed termination crossed the wire: status=%d reply=%s", status, reply)
	}
}

func TestExecutionStageRejectsTerminationWithMismatchedExecutionCap(t *testing.T) {
	executor := &fixedExecutor{result: delegatecontract.InvocationResult{
		Version: delegatecontract.WireVersion, Status: "failed", Error: "bad termination",
		Termination: &delegatecontract.TerminationDiagnostic{
			Kind: delegatecontract.TerminationTurnCap, MaxTurns: 2, ObservedTurns: 2,
		},
	}}
	request, _ := json.Marshal(delegatecontract.Invocation{Version: delegatecontract.WireVersion,
		Role: "review", Persona: "reviewer", Input: canonical.TaskRequest("fixture", "inspect"), ExecutionTimeoutMS: 1000})
	if reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageInvoke}, request); status != bus.ModuleStatusInternal || reply != nil {
		t.Fatalf("termination with a mismatched execution cap crossed the wire: status=%d reply=%s", status, reply)
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

func TestRegistryExecutorEnforcesMaxParallelWithoutPoisoningAgentHealth(t *testing.T) {
	started := make(chan struct{}, 2)
	release := make(chan struct{})
	executor, _ := hostFixture(t, "claude", hostCallFunc(func(ctx context.Context, _, _ uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
		started <- struct{}{}
		select {
		case <-release:
			return json.Marshal(hostResult("done"))
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}))
	path := filepath.Join(executor.home, "models.json")
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var registry map[string]any
	if err = json.Unmarshal(body, &registry); err != nil {
		t.Fatal(err)
	}
	registry["agents"].([]any)[0].(map[string]any)["max_parallel"] = 1
	body, _ = json.Marshal(registry)
	if err = os.WriteFile(path, body, 0600); err != nil {
		t.Fatal(err)
	}
	results := make(chan delegatecontract.InvocationResult, 2)
	for range 2 {
		go func() { results <- executor.Execute(t.Context(), nativeInvocation()) }()
	}
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("canonical host did not start")
	}
	select {
	case <-started:
		t.Fatal("concurrent host calls exceeded max_parallel")
	case <-time.After(50 * time.Millisecond):
	}
	if _, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "reviewer"}}); !errors.Is(err, delegatecontract.ErrDelegateCapacity) || errors.Is(err, delegatecontract.ErrDelegateTerminal) {
		t.Fatalf("occupied agent was not retryable capacity: %v", err)
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(body, after) {
		t.Fatal("capacity response mutated authoritative registry")
	}
	close(release)
	for range 2 {
		if result := <-results; result.Status != "done" {
			t.Fatalf("host result: %+v", result)
		}
	}
	models, err := executor.PlanGroup(t.Context(), []delegatecontract.GroupPlanSeat{{Role: "review", Persona: "reviewer"}})
	if err != nil || !slices.Equal(models, []string{"native"}) {
		t.Fatalf("capacity poisoned registry: models=%v error=%v", models, err)
	}
}

func TestRegistryExecutorTypesCallerDeadlineAsExecutionDeadline(t *testing.T) {
	executor, _ := hostFixture(t, "claude", hostCallFunc(func(ctx context.Context, _, _ uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	}))
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Millisecond)
	defer cancel()
	result := executor.Execute(ctx, nativeInvocation())
	if result.Status != "failed" || !delegatecontract.IsExecutionDeadline(errors.New(result.Error)) || delegatecontract.IsCapacityDeadline(errors.New(result.Error)) {
		t.Fatalf("caller deadline was not execution deadline: %+v", result)
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
	unavailable, available := compatibleBool(false), compatibleBool(true)
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

func TestNativeModelsRequireCanonicalHost(t *testing.T) {
	for _, kind := range []string{"claude", "claude-code", "codex"} {
		executor, marker := hostFixture(t, kind, nil)
		result := executor.Execute(t.Context(), nativeInvocation())
		if result.Status != "failed" || !strings.Contains(result.Error, "owner unavailable") {
			t.Fatalf("missing canonical host did not fail closed: %+v", result)
		}
		if _, err := os.Stat(marker); !os.IsNotExist(err) {
			t.Fatal("native SDK command ran")
		}
	}
}

func turnCapExecutorFixture(t *testing.T, kind string) (*RegistryExecutor, string) {
	t.Helper()
	return hostFixture(t, kind, hostCallFunc(func(_ context.Context, _, _ uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
		var request delegatecontract.Invocation
		if err := json.Unmarshal(body, &request); err != nil {
			return nil, err
		}
		if request.MaxTurns != 1 || request.ExecutionTimeoutMS != 5000 {
			t.Fatal("native execution limit lost before host")
		}
		return json.Marshal(delegatecontract.InvocationResult{Version: delegatecontract.WireVersion, Agent: request.Model, Status: "failed", Error: delegatecontract.ErrDelegateTurnCap.Error(), Termination: &delegatecontract.TerminationDiagnostic{Kind: delegatecontract.TerminationTurnCap, MaxTurns: request.MaxTurns, ObservedTurns: 2, ExecutionTimeoutMS: request.ExecutionTimeoutMS, ElapsedMS: 1, Detail: "[REDACTED]"}})
	}))
}

func assertTurnCapResult(t *testing.T, result delegatecontract.InvocationResult) {
	t.Helper()
	if result.Status != "failed" || !strings.Contains(result.Error, "aimee_err=turn_cap") ||
		result.Termination == nil {
		t.Fatalf("turn-cap result = %+v", result)
	}
	diagnostic := result.Termination
	if diagnostic.Kind != delegatecontract.TerminationTurnCap || diagnostic.MaxTurns != 1 ||
		diagnostic.ObservedTurns != 2 || diagnostic.ExecutionTimeoutMS != 5000 ||
		diagnostic.ElapsedMS <= 0 {
		t.Fatalf("turn-cap diagnostic = %+v", diagnostic)
	}
	if len(diagnostic.Detail) > delegatecontract.TerminationDetailMax ||
		strings.Contains(diagnostic.Detail, "stub-secret") ||
		!strings.Contains(diagnostic.Detail, "[REDACTED]") {
		t.Fatalf("unsafe or unbounded diagnostic detail = %q", diagnostic.Detail)
	}
}

func TestRegistryExecutorPreservesHostTurnCap(t *testing.T) {
	for _, kind := range []string{"claude", "codex"} {
		t.Run(kind, func(t *testing.T) {
			executor, pidFile := turnCapExecutorFixture(t, kind)
			result := executor.Execute(t.Context(), delegatecontract.Invocation{
				Version: delegatecontract.WireVersion, Role: "review", Persona: "qa",
				Input: canonical.TaskRequest("fixture", "exceed the cap"), Tools: true, MaxTurns: 1, ExecutionTimeoutMS: 5000,
			})
			assertTurnCapResult(t, result)
			if _, err := os.Stat(pidFile); !os.IsNotExist(err) {
				t.Fatal("native SDK bypassed host owner")
			}
		})
	}
}

func TestExecutionStagePreservesHostTurnCapWire(t *testing.T) {
	for _, kind := range []string{"claude", "codex"} {
		t.Run(kind, func(t *testing.T) {
			executor, pidFile := turnCapExecutorFixture(t, kind)
			request, _ := json.Marshal(delegatecontract.Invocation{
				Version: delegatecontract.WireVersion, Role: "review", Persona: "qa",
				Input: canonical.TaskRequest("fixture", "cross the wire"), Tools: true, MaxTurns: 1, ExecutionTimeoutMS: 5000,
			})
			reply, status := NewHandler(executor)(bus.ModuleInvocation{StageID: StageInvoke}, request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("wire status = %d", status)
			}
			var result delegatecontract.InvocationResult
			if err := json.Unmarshal(reply, &result); err != nil {
				t.Fatal(err)
			}
			assertTurnCapResult(t, result)
			if _, err := os.Stat(pidFile); !os.IsNotExist(err) {
				t.Fatal("native SDK bypassed host owner")
			}
		})
	}
}

func TestExecutorCancellationReachesCanonicalHost(t *testing.T) {
	started := make(chan struct{})
	executor, marker := hostFixture(t, "codex", hostCallFunc(func(ctx context.Context, _, _ uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
		close(started)
		<-ctx.Done()
		return nil, ctx.Err()
	}))
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan delegatecontract.InvocationResult, 1)
	go func() { done <- executor.Execute(ctx, nativeInvocation()) }()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("host did not start")
	}
	cancel()
	select {
	case result := <-done:
		if result.Status != "failed" || result.Termination == nil || result.Termination.Kind != delegatecontract.TerminationCancelled {
			t.Fatalf("cancel result: %+v", result)
		}
	case <-time.After(time.Second):
		t.Fatal("host did not acknowledge cancellation")
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("cancellation launched a fallback process")
	}
}

func TestRegistryExecutorExecutionDeadlineDiagnostic(t *testing.T) {
	executor, marker := hostFixture(t, "codex", hostCallFunc(func(ctx context.Context, _, _ uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	}))
	ctx, cancel := context.WithTimeout(t.Context(), 75*time.Millisecond)
	defer cancel()
	request := nativeInvocation()
	request.ExecutionTimeoutMS = 75
	result := executor.Execute(ctx, request)
	if result.Status != "failed" || result.Termination == nil || result.Termination.Kind != delegatecontract.TerminationExecutionDeadline || result.Termination.ExecutionTimeoutMS != 75 || result.Termination.ElapsedMS <= 0 || !strings.Contains(result.Error, "aimee_err=execution_deadline") {
		t.Fatalf("execution deadline result: %+v", result)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("execution deadline launched a fallback process")
	}
}

func TestRegistryExecutorFirstBootFindsNormalModelRegistration(t *testing.T) {
	home := t.TempDir()
	calls := 0
	executor, err := NewRegistryExecutorWithHost(home, hostCallFunc(func(_ context.Context, _, _ uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
		calls++
		var request delegatecontract.Invocation
		if err := json.Unmarshal(body, &request); err != nil {
			t.Fatal(err)
		}
		if request.Input == nil || !request.Input.Valid() {
			t.Fatal("canonical input lost")
		}
		return json.Marshal(hostResult("actual owner answer"))
	}))
	if err != nil {
		t.Fatal(err)
	}
	registry := []byte(`{"models":[{"name":"native","cli_kind":"claude","cli_cmd":"claude","roles":["review"]}]}`)
	if err := os.WriteFile(filepath.Join(home, "models.json"), registry, 0600); err != nil {
		t.Fatal(err)
	}
	result := executor.Execute(t.Context(), nativeInvocation())
	if text, textErr := result.Output.SemanticText(); result.Status != "done" || calls != 1 || textErr != nil || text != "actual owner answer" {
		t.Fatal("normal first-boot registration unavailable", result, calls)
	}
	request := nativeInvocation()
	request.Input = nil
	result = executor.Execute(t.Context(), request)
	if result.Status != "failed" || calls != 1 {
		t.Fatal("registry discovery bypassed canonical input validation", result, calls)
	}
}
func TestRegistryExecutorCanonicalReplacesLegacyAtSameTimestamp(t *testing.T) {
	home := t.TempDir()
	legacy := filepath.Join(home, "agents.json")
	if err := os.WriteFile(legacy, []byte(`{"agents":[{"name":"legacy","cli_kind":"generic"}]}`), 0600); err != nil {
		t.Fatal(err)
	}
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(legacy)
	if err != nil {
		t.Fatal(err)
	}
	canonical := filepath.Join(home, "models.json")
	if err := os.WriteFile(canonical, []byte(`{"models":[{"name":"native","cli_kind":"claude"}]}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(canonical, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	registry, err := executor.load()
	if err != nil || registry.roster()[0].Name != "native" {
		t.Fatal("cached legacy displaced canonical authority", registry, err)
	}
	// Atomic owner writes can retain both timestamp and size; inode identity changes.
	replacement := canonical + ".new"
	if err := os.WriteFile(replacement, []byte(`{"models":[{"name":"second","cli_kind":"claude"}]}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(replacement, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(replacement, canonical); err != nil {
		t.Fatal(err)
	}
	registry, err = executor.load()
	if err != nil || registry.roster()[0].Name != "second" {
		t.Fatal("atomic replacement lost", registry, err)
	}
	info, err = os.Stat(canonical)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(canonical, []byte(strings.Repeat("!", int(info.Size()))), 0600); err != nil {
		t.Fatal(err)
	}
	if err = os.Chtimes(canonical, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	if _, err = executor.load(); err == nil {
		t.Fatal("malformed same-inode/mtime/size canonical owner served cached authority")
	}
}
func TestRegistryExecutorCanonicalErrorsNeverUseLegacy(t *testing.T) {
	for _, kind := range []string{"malformed", "directory", "dangling"} {
		t.Run(kind, func(t *testing.T) {
			home := t.TempDir()
			if err := os.WriteFile(filepath.Join(home, "agents.json"), []byte(`{"agents":[{"name":"legacy","cli_kind":"generic"}]}`), 0600); err != nil {
				t.Fatal(err)
			}
			executor, err := NewRegistryExecutor(home)
			if err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(home, "models.json")
			switch kind {
			case "malformed":
				err = os.WriteFile(path, []byte(`{"models":`), 0600)
			case "directory":
				err = os.Mkdir(path, 0700)
			case "dangling":
				err = os.Symlink(filepath.Join(home, "absent"), path)
			}
			if err != nil {
				t.Fatal(err)
			}
			if _, err = executor.load(); err == nil {
				t.Fatal("canonical failure fell back to legacy authority", kind)
			}
			if _, err = NewRegistryExecutor(home); err == nil {
				t.Fatal("unusable canonical entry treated as unconfigured first boot", kind)
			}
		})
	}
}
