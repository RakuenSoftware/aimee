package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/delegate"
	canonical "github.com/JBailes/aimee/server-go/ir"
)

const HostExecuteEvent uint32 = 6679
const HostExecuteStage uint32 = 23
const HostExecutePrincipal uint32 = 76

// NewProcessHandler attaches a request-only identity distinct from the serving
// delegates principal. Both multicall and standalone exports use this path.
func NewProcessHandler(ctx context.Context, socket string) (bus.ModuleHandler, error) {
	if ctx == nil || socket == "" {
		return nil, errors.New("native host execution requires the owned module bus")
	}
	client, err := bus.ConnectClient(ctx, socket, 1, HostExecutePrincipal)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		return nil, err
	}
	executor, err := NewRegistryExecutorWithHost(os.Getenv("AIMEE_HOME"), caller)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		return nil, err
	}
	go func() { <-ctx.Done(); caller.CloseAndWait(); client.Detach() }()
	return NewHandler(executor), nil
}

func NewRegistryExecutorWithHost(home string, caller contract.StageCaller) (*RegistryExecutor, error) {
	executor, err := NewRegistryExecutor(home)
	if err != nil {
		return nil, err
	}
	executor.host = caller
	return executor, nil
}

var hostTrace atomic.Uint64

func (r *RegistryExecutor) executeHost(ctx context.Context, request contract.Invocation, agent agentEntry) contract.InvocationResult {
	fail := contract.InvocationResult{Version: contract.WireVersion, Status: "failed", Agent: agent.Name}
	if r.host == nil {
		fail.Error = "canonical native execution owner unavailable"
		return fail
	}
	if !contract.ValidExecutionScope(request.ExecutionScope) || request.Operation == "" || len(request.Operation) > 256 || !utf8.ValidString(request.Operation) || strings.ContainsAny(request.Operation, "\x00\r\n") || request.Input == nil || !request.Input.Valid() {
		fail.Error = "native execution requires an admitted request and host-issued source scope"
		return fail
	}
	// Selection is a host-validated registry identity, never arbitrary cli_cmd.
	request.Model = agent.Name
	body, err := json.Marshal(request)
	if err != nil {
		fail.Error = "invalid canonical native invocation"
		return fail
	}
	deadline := contract.DefaultDeadline
	if request.ExecutionTimeoutMS > 0 {
		deadline = time.Duration(request.ExecutionTimeoutMS) * time.Millisecond
	}
	if until, ok := ctx.Deadline(); ok && time.Until(until) < deadline {
		deadline = time.Until(until)
	}
	if deadline <= 0 {
		fail.Error = contract.ErrDelegateExecutionDeadline.Error()
		return fail
	}
	started := time.Now()
	reply, err := r.host.Call(ctx, HostExecuteEvent, HostExecuteStage, hostTrace.Add(1), deadline, body)
	if err != nil {
		fail.Error = contract.SafeDiagnostic(err.Error())
		kind := ""
		if errors.Is(ctx.Err(), context.DeadlineExceeded) || errors.Is(err, context.DeadlineExceeded) {
			kind = contract.TerminationExecutionDeadline
			fail.Error = contract.ErrDelegateExecutionDeadline.Error()
		}
		if errors.Is(ctx.Err(), context.Canceled) || errors.Is(err, context.Canceled) {
			kind = contract.TerminationCancelled
			fail.Error = context.Canceled.Error()
		}
		if kind != "" {
			fail.Termination = &contract.TerminationDiagnostic{Kind: kind, ExecutionTimeoutMS: request.ExecutionTimeoutMS, ElapsedMS: time.Since(started).Milliseconds()}
		}
		return fail
	}
	var result contract.InvocationResult
	if !canonical.Decode(reply, &result) || contract.AdmitResult(&result) != nil || result.Agent != agent.Name {
		fail.Error = "native execution owner returned an invalid canonical result"
		return fail
	}
	return result
}

func (r *RegistryExecutor) ResolveTriggerExecutionScope(ctx context.Context, request contract.TriggerExecutionScopeRequest) (string, error) {
	if r.host == nil || !request.Valid() {
		return "", errors.New("registered trigger host source unavailable")
	}
	body, err := json.Marshal(struct {
		Version             int                                   `json:"version"`
		Action              string                                `json:"action"`
		Registration        contract.TriggerExecutionRegistration `json:"registration"`
		ConfigurationSHA256 string                                `json:"configuration_sha256"`
	}{contract.WireVersion, "trigger_scope", request.Registration, request.ConfigurationSHA256})
	if err != nil {
		return "", err
	}
	reply, err := r.host.Call(ctx, HostExecuteEvent, HostExecuteStage, hostTrace.Add(1), 30*time.Second, body)
	if err != nil {
		return "", err
	}
	var scope contract.TriggerExecutionScopeReply
	if !canonical.Decode(reply, &scope) || !contract.ValidExecutionScope(scope.ExecutionScope) {
		return "", errors.New("invalid host trigger execution scope")
	}
	return scope.ExecutionScope, nil
}
