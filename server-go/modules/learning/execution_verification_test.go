package learning

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func executionTestFixture() (ExecutionRecord, ExecutionTestProof, ring.CommandContext) {
	record, caller := executionFixture()
	caller.TransportIdentity = "host:isolated-project-verification"
	skipped := false
	proof := ExecutionTestProof{
		Version: 1, Session: record.Session, Turn: record.Turn, Actor: record.Actor,
		ScopeKind: "project", ScopeID: "approved-project", Isolation: "private_source_snapshot",
		SnapshotID: strings.Repeat("a", 32), SnapshotCommit: strings.Repeat("f", 40), FinalCommit: strings.Repeat("f", 40), SnapshotTree: strings.Repeat("b", 40), FinalTree: strings.Repeat("b", 40),
		ConfigurationDigest: strings.Repeat("c", 64), FinalConfigurationDigest: strings.Repeat("c", 64),
		ProfileDigest: strings.Repeat("d", 64), Completed: true, ObservedAt: "2026-10-20T12:00:00Z",
		Steps: []ExecutionTestStep{{Name: "configured-tests", CommandDigest: strings.Repeat("e", 64),
			Ran: true, Skipped: &skipped, Termination: "exited", ExitCode: "0"}},
	}
	return record, proof, caller
}

func TestExecutionTestGradeBindsExactTerminalAndLimitedQualification(t *testing.T) {
	record, proof, caller := executionTestFixture()
	grade, ok := executionTestGrade(record, proof, caller)
	if !ok || grade.Source.Sequence != record.Sequence || grade.Source.Actor != record.Actor ||
		grade.Source.EventID != record.EventID || grade.Source.Turn != record.Turn ||
		grade.Verification != "tests_bound_revision" || grade.Qualification != "configured_tests_only" ||
		grade.Verdict != "passed" || len(grade.VerificationID) != 71 {
		t.Fatal("lost source or overstated verification", grade, ok)
	}
	repeated, _ := executionTestGrade(record, proof, caller)
	if repeated.VerificationID != grade.VerificationID {
		t.Fatal("identical verification changed identity")
	}
	proof.Steps[0].ExitCode = "1"
	failed, ok := executionTestGrade(record, proof, caller)
	if !ok || failed.Verdict != "failed" || failed.VerificationID == grade.VerificationID {
		t.Fatal("a real failed test became a passing or identical proof", failed, ok)
	}
}

func TestExecutionTestGradeRefusesUnownedIncompleteOrChangedEvidence(t *testing.T) {
	for name, mutate := range map[string]func(*ExecutionRecord, *ExecutionTestProof, *ring.CommandContext){
		"model assertion": func(_ *ExecutionRecord, _ *ExecutionTestProof, c *ring.CommandContext) { c.TransportIdentity = "model" },
		"ordinary completion": func(_ *ExecutionRecord, _ *ExecutionTestProof, c *ring.CommandContext) {
			c.TransportIdentity = "host:canonical-execution"
		},
		"unauthenticated":  func(_ *ExecutionRecord, _ *ExecutionTestProof, c *ring.CommandContext) { c.Authenticated = false },
		"human evaluation": func(_ *ExecutionRecord, _ *ExecutionTestProof, c *ring.CommandContext) { c.UserAuthority = true },
		"other actor":      func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Actor = "bob" },
		"other session":    func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Session = "other" },
		"other turn":       func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Turn = "other" },
		"cancelled task": func(r *ExecutionRecord, _ *ExecutionTestProof, _ *ring.CommandContext) {
			r.Event = json.RawMessage(`{"kind":"execution","payload":{"status":"cancelled"}}`)
		},
		"failed capture": func(r *ExecutionRecord, _ *ExecutionTestProof, _ *ring.CommandContext) {
			r.Event = json.RawMessage(`{"kind":"execution","payload":{"status":"failed","error":"capture failed"}}`)
		},
		"no scope":     func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.ScopeID = "" },
		"global scope": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.ScopeKind = "global" },
		"shared worktree": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.Isolation = "shared_worktree"
		},
		"unjoined worker": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Completed = false },
		"changed commit": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.FinalCommit = strings.Repeat("0", 40)
		},
		"changed source": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.FinalTree = strings.Repeat("0", 40)
		},
		"changed configuration": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.FinalConfigurationDigest = strings.Repeat("0", 64)
		},
		"missing snapshot owner":    func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.SnapshotID = "" },
		"unknown execution profile": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.ProfileDigest = "" },
		"no tests":                  func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps = nil },
		"cached test":               func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { *p.Steps[0].Skipped = true },
		"unknown skip state":        func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].Skipped = nil },
		"unexecuted test":           func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].Ran = false },
		"duplicate step": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.Steps = append(p.Steps, p.Steps[0])
		},
		"timeout": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.Steps[0].Termination = "timed_out"
		},
		"signal": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.Steps[0].Termination = "signalled"
		},
		"unknown exit":      func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].ExitCode = "" },
		"rounded exit":      func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].ExitCode = "0.0" },
		"noncanonical exit": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].ExitCode = "00" },
		"transport failure": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.Steps[0].ExitCode = "-1" },
		"bad revision": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) {
			p.SnapshotTree = "HEAD"
			p.FinalTree = "HEAD"
		},
		"unknown timestamp": func(_ *ExecutionRecord, p *ExecutionTestProof, _ *ring.CommandContext) { p.ObservedAt = "" },
	} {
		t.Run(name, func(t *testing.T) {
			record, proof, caller := executionTestFixture()
			mutate(&record, &proof, &caller)
			if _, ok := executionTestGrade(record, proof, caller); ok {
				t.Fatal("unqualified evidence became a grade")
			}
		})
	}
}

func TestExecutionTestGradeHostProcessContract(t *testing.T) {
	record, proof, caller := executionTestFixture()
	args, _ := json.Marshal(ExecutionTestGradeRequest{Record: record, Proof: proof})
	wire, err := ring.EncodeCommandWithContext("grade", args, caller)
	if err != nil {
		t.Fatal(err)
	}
	invocation := ring.ModuleInvocation{StageID: StageExecution}
	response, status := Handle(invocation, wire)
	if status != ring.ModuleStatusOK || len(response) == 0 {
		t.Fatal("isolated host verification refused", status)
	}
	invocation.PrincipalRef = 1
	if _, status = Handle(invocation, wire); status != ring.ModuleStatusInvalidRequest {
		t.Fatal("foreign principal reference accepted", status)
	}
	invocation.PrincipalRef = 0
	caller.TransportIdentity = "host:canonical-execution"
	wire, _ = ring.EncodeCommandWithContext("grade", args, caller)
	if _, status = Handle(invocation, wire); status != ring.ModuleStatusInvalidRequest {
		t.Fatal("process completion became grading authority", status)
	}
	caller.TransportIdentity = "host:isolated-project-verification"
	args = append(args[:len(args)-1], []byte(`,"model_verified":true}`)...)
	wire, _ = ring.EncodeCommandWithContext("grade", args, caller)
	if _, status = Handle(invocation, wire); status != ring.ModuleStatusInvalidRequest {
		t.Fatal("opaque assertion accepted", status)
	}
}
