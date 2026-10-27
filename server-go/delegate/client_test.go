package delegate

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
)

type recordingCaller struct {
	request []byte
	result  InvocationResult
	reply   []byte
	err     error
}

type groupRoutingCaller struct {
	mu            sync.Mutex
	plannedModels []string
	planError     string
	invokedModels []string
	plan          GroupPlan
}

func (c *groupRoutingCaller) Call(_ context.Context, _, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if stage == StageGroupPlan {
		if err := json.Unmarshal(request, &c.plan); err != nil {
			return nil, err
		}
		return json.Marshal(GroupPlanResult{Version: WireVersion, Models: c.plannedModels, Error: c.planError})
	}
	var invocation Invocation
	if err := json.Unmarshal(request, &invocation); err != nil {
		return nil, err
	}
	c.invokedModels = append(c.invokedModels, invocation.Model)
	return json.Marshal(testCanonicalInvocation(InvocationResult{Version: WireVersion, Status: "done",
		Agent: invocation.Model, Response: "ok"}))
}

func (c *recordingCaller) Call(_ context.Context, kind, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	if kind != EventKind || stage != StageInvoke {
		panic("wrong delegate stage")
	}
	c.request = append([]byte(nil), request...)
	if c.err != nil {
		return nil, c.err
	}
	if c.reply != nil {
		return c.reply, nil
	}
	return json.Marshal(c.result)
}

func TestBusWireOmitsCallerLifecycleState(t *testing.T) {
	caller := &recordingCaller{result: testCanonicalInvocation(InvocationResult{Version: WireVersion, Status: "done", Response: "ok"})}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "security",
		Delegate: "codex", Input: canonical.TaskRequest("security", "review this"), Workdir: "/repo", Tools: true,
		WorkItemID: "wi-1", Stage: "gate", ExecutionVersion: "v7", RetryTag: "retry",
		DurableSlot: "seat-2", Participant: "opaque", ReplayOnly: false})
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]any
	if err := json.Unmarshal(caller.request, &fields); err != nil {
		t.Fatal(err)
	}
	for _, forbidden := range []string{"work_item_id", "stage", "execution_version", "retry_tag",
		"durable_slot", "participant", "replay_only"} {
		if _, exists := fields[forbidden]; exists {
			t.Fatalf("caller state %q crossed the delegate wire", forbidden)
		}
	}
	for _, required := range []string{"role", "persona", "model", "ir", "workdir", "tools",
		"execution_timeout_ms"} {
		if _, exists := fields[required]; !exists {
			t.Fatalf("delegate field %q missing", required)
		}
	}
}

func TestReplayOnlyDoesNotLaunchStatelessDelegate(t *testing.T) {
	client := &BusClient{caller: &recordingCaller{}, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "code", Persona: "engineer",
		Input: canonical.TaskRequest("engineer", "work"), ReplayOnly: true})
	if !errors.Is(err, ErrDelegateReplayUnavailable) {
		t.Fatalf("replay-only error = %v", err)
	}
	var execution *DelegateExecutionError
	if !errors.As(err, &execution) || !execution.Dispatched || execution.CostKnown {
		t.Fatalf("replay billing boundary = %#v", execution)
	}
}

func TestCostLimitFailsBeforeDispatch(t *testing.T) {
	caller := &recordingCaller{}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "security",
		Input: canonical.TaskRequest("security", "review this"), MaxCostUSD: 0.25})
	if !errors.Is(err, ErrDelegateCostLimitUnsupported) {
		t.Fatalf("cost limit error = %v", err)
	}
	if caller.request != nil {
		t.Fatal("an unenforceable cost-capped request was dispatched")
	}
}

func TestDelegateResultPreservesCallerLocalParticipant(t *testing.T) {
	caller := &recordingCaller{result: testCanonicalInvocation(InvocationResult{Version: WireVersion, Status: "done", Response: "ok"})}
	client := &BusClient{caller: caller, deadline: time.Second}
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "security",
		Input: canonical.TaskRequest("security", "review this"), Participant: "seat-security"})
	if err != nil {
		t.Fatal(err)
	}
	if result.Participant != "seat-security" {
		t.Fatalf("participant = %q", result.Participant)
	}
}

func TestDispatchBoundaryClassifiesBusAndReplyFailures(t *testing.T) {
	request := DelegateRequest{Role: "review", Persona: "security", Input: canonical.TaskRequest("security", "review")}
	for _, preDispatch := range []error{
		bus.ErrModuleCallCapabilityAbsent,
		bus.ErrModuleCallRejected,
		errors.Join(bus.ErrModuleCallNotDispatched, errors.New("ring full")),
	} {
		client := &BusClient{caller: &recordingCaller{err: preDispatch}, deadline: time.Second}
		_, err := client.Delegate(t.Context(), request)
		var execution *DelegateExecutionError
		if !errors.Is(err, preDispatch) || errors.As(err, &execution) {
			t.Fatalf("pre-dispatch %v classified as %#v", preDispatch, err)
		}
	}
	for _, postDispatch := range [][]byte{[]byte("not-json"), []byte(`{"version":2,"status":"working"}`)} {
		client := &BusClient{caller: &recordingCaller{reply: postDispatch}, deadline: time.Second}
		_, err := client.Delegate(t.Context(), request)
		var execution *DelegateExecutionError
		if !errors.As(err, &execution) || !execution.Dispatched || execution.CostKnown {
			t.Fatalf("post-dispatch reply %q classified as %#v", postDispatch, err)
		}
	}
}

func TestDelegateGroupPlansDiversityAndKeepsParticipantContinuityLocal(t *testing.T) {
	caller := &groupRoutingCaller{plannedModels: []string{"review-a", "review-b"}}
	client := &BusClient{caller: caller, deadline: time.Second}
	requests := []DelegateRequest{
		{Role: "review", Persona: "security", Input: canonical.TaskRequest("security", "one")},
		{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "two"), Participant: "review-b"},
	}
	results := client.DelegateGroup(t.Context(), requests)
	if len(results) != 2 || results[0].Participant != "review-a" ||
		results[1].Participant != "review-b" {
		t.Fatalf("group results = %+v", results)
	}
	if caller.plan.Seats[1].Model != "review-b" {
		t.Fatalf("participant continuity crossed as %+v instead of a local model pin", caller.plan.Seats[1])
	}
	for _, forbidden := range []string{"participant", "durable_slot", "work_item_id"} {
		body, _ := json.Marshal(caller.plan)
		if strings.Contains(string(body), forbidden) {
			t.Fatalf("caller field %q crossed group wire: %s", forbidden, body)
		}
	}
	slices.Sort(caller.invokedModels)
	if !slices.Equal(caller.invokedModels, []string{"review-a", "review-b"}) {
		t.Fatalf("invoked models = %v", caller.invokedModels)
	}
}

func TestDelegateGroupPreservesCapacityFromTheModuleBoundary(t *testing.T) {
	caller := &groupRoutingCaller{planError: "group saturated: " + ErrDelegateCapacity.Error()}
	client := &BusClient{caller: caller, deadline: time.Second}
	results := client.DelegateGroup(t.Context(), []DelegateRequest{{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")}})
	if len(results) != 1 || !errors.Is(results[0].Err, ErrDelegateCapacity) ||
		!IsCapacityBackpressure(results[0].Err) || errors.Is(results[0].Err, ErrDelegateTerminal) {
		t.Fatalf("group capacity lost its load classification: %+v", results)
	}
}

func TestDelegateResultPreservesCapacityWaitDeadline(t *testing.T) {
	caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed",
		Error: ErrDelegateCapacityDeadline.Error()}}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
	if !errors.Is(err, ErrDelegateCapacityDeadline) || !errors.Is(err, context.DeadlineExceeded) ||
		errors.Is(err, ErrDelegateTerminal) {
		t.Fatalf("capacity wait deadline was collapsed into execution failure: %v", err)
	}
}

func TestDelegateResultPreservesExecutionDeadline(t *testing.T) {
	caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed",
		Error: ErrDelegateExecutionDeadline.Error(), Termination: &TerminationDiagnostic{
			Kind: TerminationExecutionDeadline, ExecutionTimeoutMS: 1000, ElapsedMS: 999,
		}}}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
	if !errors.Is(err, ErrDelegateExecutionDeadline) || !errors.Is(err, context.DeadlineExceeded) ||
		errors.Is(err, ErrDelegateCapacityDeadline) || errors.Is(err, ErrDelegateTerminal) {
		t.Fatalf("execution deadline was collapsed into another failure: %v", err)
	}
	var execution *DelegateExecutionError
	if !errors.As(err, &execution) || execution.Termination == nil ||
		execution.Termination.Kind != TerminationExecutionDeadline {
		t.Fatalf("execution deadline diagnostic was lost: %#v", err)
	}
}

func TestDelegateResultPreservesTurnCapDiagnostic(t *testing.T) {
	caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed",
		Error: ErrDelegateTurnCap.Error(), Termination: &TerminationDiagnostic{
			Kind: TerminationTurnCap, MaxTurns: 2, ObservedTurns: 3,
			ExecutionTimeoutMS: 1000, ElapsedMS: 1250, Detail: "bounded detail",
		}}}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
	if !errors.Is(err, ErrDelegateTurnCap) || errors.Is(err, context.DeadlineExceeded) ||
		errors.Is(err, context.Canceled) || errors.Is(err, ErrDelegateTerminal) {
		t.Fatalf("turn cap was collapsed into another failure: %v", err)
	}
	var execution *DelegateExecutionError
	if !errors.As(err, &execution) || execution.Termination == nil ||
		execution.Termination.ObservedTurns != 3 {
		t.Fatalf("turn-cap diagnostic was lost: %#v", err)
	}
}

func TestDelegateResultPreservesProducerCancellation(t *testing.T) {
	caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed",
		Error: context.Canceled.Error(), Termination: &TerminationDiagnostic{
			Kind: TerminationCancelled, ExecutionTimeoutMS: 1000, ElapsedMS: 250,
		}}}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
	if !errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) ||
		IsCapacityDeadline(err) || IsExecutionDeadline(err) || errors.Is(err, ErrDelegateTerminal) {
		t.Fatalf("producer cancellation was retyped: %v", err)
	}
}

func TestDelegateResultRejectsMalformedTermination(t *testing.T) {
	for name, termination := range map[string]*TerminationDiagnostic{
		"turn count": {
			Kind: TerminationTurnCap, MaxTurns: 2, ObservedTurns: 0,
			ExecutionTimeoutMS: 1000,
		},
		"omitted execution cap": {
			Kind: TerminationTurnCap, MaxTurns: 2, ObservedTurns: 2,
		},
	} {
		t.Run(name, func(t *testing.T) {
			caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed",
				Error: ErrDelegateTurnCap.Error(), Termination: termination}}
			client := &BusClient{caller: caller, deadline: time.Second}
			_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
			if err == nil || !strings.Contains(err.Error(), "invalid terminal result") ||
				errors.Is(err, ErrDelegateTurnCap) {
				t.Fatalf("malformed termination was accepted: %v", err)
			}
		})
	}
}

func TestDelegateCallerCancellationIsNotRetypedAsDeadline(t *testing.T) {
	caller := &recordingCaller{err: context.Canceled}
	client := &BusClient{caller: caller, deadline: time.Second}
	_, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: canonical.TaskRequest("qa", "review")})
	if !errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) ||
		IsCapacityDeadline(err) || IsExecutionDeadline(err) {
		t.Fatalf("caller cancellation was retyped at the module/client boundary: %v", err)
	}
}

func TestSafeDiagnosticRedactsCredentialForms(t *testing.T) {
	tests := map[string]string{
		"authorization": "Authorization: Bearer header-secret",
		"cookie":        "Cookie: session=cookie-secret",
		"url userinfo":  "https://user:url-secret@example.test/path",
		"token field":   `{"access_token":"json-secret"}`,
		"aws key":       "AKIA1234567890ABCDEF",
		"jwt":           "eyJheader.payload.signature",
		"private key":   "-----BEGIN PRIVATE KEY-----\nprivate-secret\n-----END PRIVATE KEY-----",
	}
	for name, input := range tests {
		t.Run(name, func(t *testing.T) {
			output := SafeDiagnostic(input)
			for _, secret := range []string{"header-secret", "cookie-secret", "url-secret",
				"json-secret", "AKIA1234567890ABCDEF", "eyJheader.payload.signature", "private-secret"} {
				if strings.Contains(output, secret) {
					t.Fatalf("secret %q survived redaction: %q", secret, output)
				}
			}
			if !strings.Contains(output, "[REDACTED") {
				t.Fatalf("redaction marker missing: %q", output)
			}
		})
	}
}

func TestSafeDiagnosticPreservesWireClassificationSlugs(t *testing.T) {
	for _, err := range []error{ErrDelegateCapacity, ErrDelegateCapacityDeadline, ErrDelegateExecutionDeadline} {
		if got := SafeDiagnostic(err.Error()); got != err.Error() {
			t.Fatalf("wire classification slug changed: got %q want %q", got, err)
		}
	}
}

func TestFailedCanonicalResponseDiagnosticIsNotDiscarded(t *testing.T) {
	output := canonical.TextResponse("partial plan")
	output.StopReason = canonical.StopMaxTokens
	output.Usage.Input, output.Usage.Output = "403", "512"
	encrypted := "inert-signed-thinking"
	missingText := ""
	output.Blocks = append(output.Blocks, canonical.ResponseBlock{Block: canonical.Block{Type: "thinking", Text: &missingText, EncryptedContent: &encrypted}, Raw: json.RawMessage(`{"opaque":true}`)})
	output.Raw = json.RawMessage(`{"provider_metadata":{"retained":true}}`)
	caller := &recordingCaller{result: InvocationResult{Version: WireVersion, Status: "failed", Error: "native execution failed", Response: "forged success", Output: output, CostKnown: true, CostUSD: 0.25}}
	client, _ := NewClient(caller, time.Second)
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "draft", Persona: "engineer", Input: canonical.TaskRequest("engineer", "plan")})
	var execution *DelegateExecutionError
	if !errors.As(err, &execution) || !execution.Dispatched || !execution.CostKnown || execution.CostUSD != 0.25 {
		t.Fatalf("failed execution accounting lost: %v", err)
	}
	if result.Output != nil {
		t.Fatal("failed model response was exposed as a successful result")
	}
	// Reflection lets this loss regression run against the old error shape too;
	// old source must fail at the actual discarded diagnostic, not compilation.
	field := reflect.ValueOf(execution).Elem().FieldByName("Output")
	if !field.IsValid() || field.IsNil() {
		t.Fatal("admitted canonical failed response was discarded")
	}
	diagnostic, ok := field.Interface().(*canonical.Response)
	if !ok {
		t.Fatal("diagnostic is not a typed canonical response")
	}
	before, _ := json.Marshal(output)
	after, _ := json.Marshal(diagnostic)
	if string(before) != string(after) {
		t.Fatalf("canonical diagnostic changed: %s", after)
	}
	if _, err := diagnostic.SemanticText(); err == nil {
		t.Fatal("partial canonical output acquired artifact authority")
	}
}

func TestOutputLimitClassificationRequiresAdmittedCanonicalStop(t *testing.T) {
	for _, kind := range []string{"max", "max-after-turn-cap", "end-with-forged-strings", "unknown", "refused", "error", "tool", "missing", "invalid-max", "done-max", "transport-string"} {
		t.Run(kind, func(t *testing.T) {
			output := canonical.TextResponse("partial plan")
			output.StopReason = canonical.StopMaxTokens
			rawStop := "max_tokens"
			output.RawStopReason = &rawStop
			output.Raw = json.RawMessage(`{"stop_reason":"max_tokens"}`)
			response := InvocationResult{Version: WireVersion, Status: "failed", Error: ErrDelegateOutputLimit.Error(), Response: "max_tokens", Output: output, CostKnown: true, CostUSD: 0.25}
			caller := &recordingCaller{}
			wantLimit, wantOutput := kind == "max" || kind == "max-after-turn-cap", true
			switch kind {
			case "max-after-turn-cap":
				response.Termination = &TerminationDiagnostic{Kind: TerminationTurnCap, MaxTurns: 1, ObservedTurns: 2, ExecutionTimeoutMS: 1000}
			case "end-with-forged-strings":
				output.StopReason = canonical.StopEndTurn
			case "unknown":
				output.StopReason = canonical.StopUnknown
			case "refused":
				output.StopReason = canonical.StopContentFilter
				category, explanation := "reasoning_extraction", "inert diagnostic"
				output.Refusal = &canonical.RefusalDetails{Category: &category, Explanation: &explanation}
			case "error":
				output.StopReason = canonical.StopError
				message := "provider transport failed"
				output.ErrorMessage = &message
			case "tool":
				output.StopReason = canonical.StopToolUse
				output.Blocks = []canonical.ResponseBlock{{Block: canonical.Block{Type: "tool_use", ToolID: "pending", Name: "read_file", Input: json.RawMessage(`{}`)}}}
			case "missing":
				response.Output = nil
				wantOutput = false
			case "invalid-max":
				output.Usage.Output = "-1"
				wantOutput = false
			case "done-max":
				response.Status = "done"
				response.Error = ""
				wantOutput = false
			case "transport-string":
				caller.err = errors.New(ErrDelegateOutputLimit.Error())
				wantOutput = false
			}
			caller.result = response
			client, _ := NewClient(caller, time.Second)
			result, err := client.Delegate(t.Context(), DelegateRequest{Role: "draft", Persona: "engineer", Input: canonical.TaskRequest("engineer", "plan")})
			if err == nil || errors.Is(err, ErrDelegateOutputLimit) != wantLimit {
				t.Fatalf("canonical classification mismatch: kind=%s error=%v", kind, err)
			}
			var execution *DelegateExecutionError
			if !errors.As(err, &execution) || !execution.Dispatched {
				t.Fatalf("dispatch uncertainty lost: %v", err)
			}
			if (execution.Output != nil) != wantOutput {
				t.Fatalf("unadmitted or missing response acquired diagnostics: %s", kind)
			}
			if kind == "max-after-turn-cap" && (!errors.Is(err, ErrDelegateTurnCap) || execution.Termination == nil || execution.Termination.ObservedTurns != 2) {
				t.Fatal("host turn-cap diagnostic lost alongside canonical MAX")
			}
			if wantOutput {
				before, _ := json.Marshal(output)
				after, _ := json.Marshal(execution.Output)
				if string(before) != string(after) || !execution.CostKnown || execution.CostUSD != 0.25 {
					t.Fatal("admitted failed response or billing metadata changed")
				}
			}
			if result.Output != nil {
				t.Fatal("failed envelope acquired success-shaped result")
			}
			if _, err = result.SemanticText(); err == nil {
				t.Fatal("failed envelope acquired business text")
			}
		})
	}
}

func TestDelegateForwardsFullCanonicalInput(t *testing.T) {
	call := &recordingCaller{result: testCanonicalInvocation(InvocationResult{Version: WireVersion, Status: "done", Response: "ok"})}
	client, err := NewClient(call, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	input := canonical.TaskRequest("qa", "Inspect the image")
	input.Messages[0].ContentScalar = false
	input.Messages[0].Blocks = append(input.Messages[0].Blocks, canonical.Block{Type: "image", MediaType: "image/png", MediaRef: "https://fixture.invalid/input.png"})
	input.Metadata = json.RawMessage(`{"opaque":{"number":7,"unicode":"雪"}}`)
	zero := 0.0
	input.Temperature = &zero
	before, err := json.Marshal(input)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: input}); err != nil {
		t.Fatal(err)
	}
	var invocation Invocation
	if err := json.Unmarshal(call.request, &invocation); err != nil {
		t.Fatal(err)
	}
	received, err := json.Marshal(invocation.Input)
	if err != nil || string(received) != string(before) {
		t.Fatalf("canonical input changed: %s", received)
	}
	after, _ := json.Marshal(input)
	if string(after) != string(before) {
		t.Fatal("caller-owned input was mutated")
	}
	for _, empty := range []*canonical.Request{nil, canonical.TaskRequest("qa", "")} {
		call.request = nil
		if _, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Input: empty}); err == nil || call.request != nil {
			t.Fatal("empty task reached transport")
		}
	}
}
