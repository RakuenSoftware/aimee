package learning

import (
	"encoding/json"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func executionFixture() (ExecutionRecord, ring.CommandContext) {
	return ExecutionRecord{Version: 1, Session: "private-session", Turn: "turn", Sequence: "9007199254740993", EventID: "turn:9", Actor: "alice", Event: json.RawMessage(`{"kind":"execution","payload":{"status":"completed"}}`)}, ring.CommandContext{Authenticated: true, Principal: "alice", TransportIdentity: "host:canonical-execution"}
}
func TestExecutionObservationUsesCanonicalTerminalWithoutInventingVerification(t *testing.T) {
	for _, status := range []string{"completed", "failed", "cancelled"} {
		record, caller := executionFixture()
		record.Event, _ = json.Marshal(map[string]any{"kind": "execution", "payload": map[string]string{"status": status, "error": "quoted \"error\"\n界"}})
		got, ok := executionObservation(record, caller)
		if !ok || got.ObservedStatus != status || got.Verification != "ungraded" || got.Source.Sequence != record.Sequence || got.Source.Actor != caller.Principal || got.Source.Kind != "canonical_session_log" || got.Error != "quoted \"error\"\n界" {
			t.Fatal("lost canonical observation or fabricated a grade", got, ok)
		}
	}
}
func TestExecutionObservationRejectsUntrustedOrNonCanonicalSources(t *testing.T) {
	for name, mutate := range map[string]func(*ExecutionRecord, *ring.CommandContext){
		"different owner":       func(r *ExecutionRecord, c *ring.CommandContext) { r.Actor = "bob" },
		"model caller":          func(r *ExecutionRecord, c *ring.CommandContext) { c.TransportIdentity = "model" },
		"interactive feedback":  func(r *ExecutionRecord, c *ring.CommandContext) { c.UserAuthority = true },
		"unauthenticated":       func(r *ExecutionRecord, c *ring.CommandContext) { c.Authenticated = false },
		"noncanonical sequence": func(r *ExecutionRecord, c *ring.CommandContext) { r.Sequence = "01" },
		"sequence overflow":     func(r *ExecutionRecord, c *ring.CommandContext) { r.Sequence = "9223372036854775808" },
		"unknown version":       func(r *ExecutionRecord, c *ring.CommandContext) { r.Version = 2 },
		"native output":         func(r *ExecutionRecord, c *ring.CommandContext) { r.Event = json.RawMessage(`{"choices":[]}`) },
		"incomplete task": func(r *ExecutionRecord, c *ring.CommandContext) {
			r.Event = json.RawMessage(`{"kind":"execution","payload":{"status":"running"}}`)
		},
		"self grade": func(r *ExecutionRecord, c *ring.CommandContext) {
			r.Event = json.RawMessage(`{"kind":"execution","payload":{"status":"completed","verified":true}}`)
		},
	} {
		t.Run(name, func(t *testing.T) {
			record, caller := executionFixture()
			mutate(&record, &caller)
			if _, ok := executionObservation(record, caller); ok {
				t.Fatal("accepted untrusted outcome")
			}
		})
	}
}
func TestExecutionObservationProcessContract(t *testing.T) {
	record, caller := executionFixture()
	args, _ := json.Marshal(record)
	wire, err := ring.EncodeCommandWithContext("observe", args, caller)
	if err != nil {
		t.Fatal(err)
	}
	invocation := ring.ModuleInvocation{StageID: StageExecution, PrincipalRef: 0}
	response, status := Handle(invocation, wire)
	if status != ring.ModuleStatusOK || len(response) == 0 {
		t.Fatal("host observation failed", status)
	}
	invocation.PrincipalRef = 8
	if _, status := Handle(invocation, wire); status != ring.ModuleStatusInvalidRequest {
		t.Fatal("non-host observation accepted", status)
	}
}
