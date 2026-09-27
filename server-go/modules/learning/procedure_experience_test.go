package learning

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"testing"
)

func procedureFixture() (ProcedureEvent, ProcedureExposure, bus.CommandContext) {
	v := ProcedureVersion{Owner: "kb", ID: "procedure:1", Revision: "3"}
	e := ProcedureEvent{SchemaVersion: 1, ID: "event-1", Trial: "trial-1", Task: "task", Attempt: "attempt-1", Procedure: v, Receipt: "receipt:1", State: "verified_success", Authority: "user_feedback", Actor: "alice", Environment: "linux", Model: "model", TaskClass: "navigation", Actions: []string{"action:1"}, Verifier: "explicit_user_evaluation", Verification: "feedback:1", ObservedAt: "2026-09-27T01:00:00Z"}
	x := ProcedureExposure{Receipt: e.Receipt, Request: "request", Attempt: e.Attempt, Procedure: v, State: "delivered", Producer: "local_host_ledger"}
	return e, x, bus.CommandContext{Authenticated: true, Principal: "alice", UserAuthority: true}
}
func TestProcedureAdmissionRejectsOmittedUnusedAndModelSuccess(t *testing.T) {
	e, x, c := procedureFixture()
	if err := ValidateProcedureEvent(e, x, c); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*ProcedureEvent, *ProcedureExposure, *bus.CommandContext){
		"omitted":             func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { x.State = "retrieved" },
		"different version":   func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { x.Procedure.Revision = "4" },
		"different attempt":   func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { x.Attempt = "retry" },
		"different actor":     func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { e.Actor = "bob" },
		"unused":              func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { e.Actions = nil },
		"self reward":         func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { e.Authority = "model" },
		"self signed receipt": func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { x.Producer = "model" },
		"unbound tests": func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) {
			e.Verifier = "tests_bound_revision"
		},
		"no user authority": func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { c.UserAuthority = false },
		"forged host":       func(e *ProcedureEvent, x *ProcedureExposure, c *bus.CommandContext) { e.Authority = "host_execution" },
	} {
		t.Run(name, func(t *testing.T) {
			e, x, c := procedureFixture()
			mutate(&e, &x, &c)
			if ValidateProcedureEvent(e, x, c) == nil {
				t.Fatal("admitted unverified application")
			}
		})
	}
}
func TestProcedureProjectionTrialsVersionsConflictAndRepair(t *testing.T) {
	success, _, _ := procedureFixture()
	failure := success
	failure.ID = "failure"
	failure.State = "verified_failure"
	failure.Verification = "failed-test"
	retry := success
	retry.ID = "retry-success"
	retry.Trial = "trial-2"
	retry.Attempt = "attempt-2"
	retry.TerminalTask = true
	unknown := success
	unknown.ID = "unknown"
	unknown.Trial = "trial-3"
	unknown.Attempt = "attempt-3"
	unknown.State = "delivered"
	newVersion := success
	newVersion.ID = "version-4"
	newVersion.Trial = "trial-4"
	newVersion.Procedure.Revision = "4"
	model := success
	model.ID = "model-success"
	model.Trial = "trial-5"
	model.Authority = "model"
	model.TerminalTask = true
	out, err := ProjectProcedureExperience([]ProcedureEvent{success, success, failure, retry, unknown, newVersion, model})
	if err != nil || len(out) != 2 {
		t.Fatalf("%+v %v", out, err)
	}
	old := out[0]
	if old.Procedure.Revision != "3" {
		old = out[1]
	}
	if old.Trials != 4 || old.Conflicting != 1 || old.Success != 1 || old.Unknown != 2 || old.Applied != 2 || old.VerifiedSample != 1 || old.TerminalTasks != 1 || old.TerminalSuccess != 1 || old.SuccessInterval == nil || old.SuccessInterval[0] <= 0 || old.SuccessInterval[0] >= 1 || len(old.Counterexamples) != 1 {
		t.Fatalf("wrong cohort: %+v", old)
	}
	changed := success
	changed.State = "verified_failure"
	if _, err := ProjectProcedureExperience([]ProcedureEvent{success, changed}); err == nil {
		t.Fatal("overwrote immutable event")
	}
	changed = success
	changed.ID = "changed-binding"
	changed.Receipt = "other"
	if _, err := ProjectProcedureExperience([]ProcedureEvent{success, changed}); err == nil {
		t.Fatal("rebound trial")
	}
}
func TestProcedureExperienceRequiresAuthenticatedHostStage(t *testing.T) {
	e, x, c := procedureFixture()
	args, _ := json.Marshal(map[string]any{"event": e, "exposure": x})
	wire, _ := bus.EncodeCommandWithContext("admit", args, c)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExperience}, wire); status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExperience, PrincipalRef: 73}, wire); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("feature module forged host receipt")
	}
	wire, _ = bus.EncodeCommand("admit", args)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExperience}, wire); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("missing authority accepted")
	}
}

func TestProcedureLatencyDoesNotDoubleCountRepeatedOrConflictingReports(t *testing.T) {
	e, _, _ := procedureFixture()
	e.ReportedLatencyMS = "120"
	out, err := ProjectProcedureExperience([]ProcedureEvent{e, e})
	if err != nil || len(out) != 1 || len(out[0].ReportedLatencies) != 1 || out[0].UnknownLatency != 0 {
		t.Fatal(out, err)
	}
	second := e
	second.ID = "conflicting-latency"
	second.ReportedLatencyMS = "200"
	out, err = ProjectProcedureExperience([]ProcedureEvent{e, second})
	if err != nil || len(out[0].ReportedLatencies) != 0 || out[0].ConflictingLatency != 1 {
		t.Fatal(out, err)
	}
}
