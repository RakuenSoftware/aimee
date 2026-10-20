package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

func verificationFixture() ExecutionTestProof {
	skipped := false
	return ExecutionTestProof{Version: 1, Session: "session", Turn: "turn", Actor: "actor", ScopeKind: "project", ScopeID: "project", Isolation: "private_source_snapshot", SnapshotID: strings.Repeat("a", 32), SnapshotCommit: strings.Repeat("b", 40), FinalCommit: strings.Repeat("b", 40), SnapshotTree: strings.Repeat("c", 40), FinalTree: strings.Repeat("c", 40), ConfigurationDigest: strings.Repeat("d", 64), FinalConfigurationDigest: strings.Repeat("d", 64), ProfileDigest: strings.Repeat("e", 64), Completed: true, ObservedAt: "2026-10-20T12:00:00Z", Steps: []ExecutionTestStep{{Name: "unit", CommandDigest: strings.Repeat("f", 64), Ran: true, Skipped: &skipped, Termination: "exited", ExitCode: "0"}}}
}
func TestVerificationProofExactBoundedMetadata(t *testing.T) {
	proof := verificationFixture()
	raw, _ := json.Marshal(proof)
	if !ValidVerificationProof(raw) {
		t.Fatal("valid proof rejected")
	}
	var fields map[string]json.RawMessage
	if !Decode(raw, &fields) {
		t.Fatal("fixture decode")
	}
	for key, original := range fields {
		delete(fields, key)
		missing, _ := json.Marshal(fields)
		if ValidVerificationProof(missing) {
			t.Fatalf("missing %s accepted", key)
		}
		fields[key] = json.RawMessage("null")
		invalid, _ := json.Marshal(fields)
		if ValidVerificationProof(invalid) {
			t.Fatalf("null %s accepted", key)
		}
		fields[key] = original
	}
	for _, change := range []func(*ExecutionTestProof){
		func(p *ExecutionTestProof) { p.Version = 2 }, func(p *ExecutionTestProof) { p.ScopeID = strings.Repeat("x", 257) },
		func(p *ExecutionTestProof) { p.Actor = "actor\x00other" }, func(p *ExecutionTestProof) { p.ProfileDigest = "opaque" },
		func(p *ExecutionTestProof) { p.Steps = nil }, func(p *ExecutionTestProof) { p.Steps = append(p.Steps, p.Steps[0]) },
		func(p *ExecutionTestProof) { p.Steps[0].Skipped = nil }, func(p *ExecutionTestProof) { p.Steps[0].ExitCode = "00" },
		func(p *ExecutionTestProof) { p.Steps[0].ExitCode = "256" }, func(p *ExecutionTestProof) { p.Steps[0].ExitCode = "-1" },
	} {
		candidate := verificationFixture()
		change(&candidate)
		bad, _ := json.Marshal(candidate)
		if ValidVerificationProof(bad) {
			t.Fatalf("malformed proof accepted %s", bad)
		}
	}
	fields["prompt"] = json.RawMessage(`"not metadata"`)
	bad, _ := json.Marshal(fields)
	if ValidVerificationProof(bad) {
		t.Fatal("arbitrary extra metadata accepted")
	}
	// Admission does not assert completion, unchanged source, or test success.
	proof.Completed = false
	proof.FinalCommit = strings.Repeat("f", 40)
	proof.Steps[0].Ran = false
	proof.Steps[0].ExitCode = "1"
	raw, _ = json.Marshal(proof)
	if !ValidVerificationProof(raw) {
		t.Fatal("structural admission became grading policy")
	}
}
func TestExecutionProofsOnlyOnCompletedTerminal(t *testing.T) {
	proof := verificationFixture()
	valid := func(status string, proofs any) bool {
		raw, _ := json.Marshal(map[string]any{"kind": "execution", "payload": map[string]any{"status": status, "verification_proofs": proofs}})
		return ValidSessionEvent(raw)
	}
	if !valid("completed", []ExecutionTestProof{proof}) {
		t.Fatal("atomic completed proof rejected")
	}
	for _, status := range []string{"running", "failed", "cancelled"} {
		if valid(status, []ExecutionTestProof{proof}) {
			t.Fatalf("proof on %s accepted", status)
		}
	}
	for _, proofs := range []any{nil, []ExecutionTestProof{}, make([]ExecutionTestProof, 9), map[string]any{"proof": proof}} {
		if valid("completed", proofs) {
			t.Fatal("invalid proof array accepted")
		}
	}
}
