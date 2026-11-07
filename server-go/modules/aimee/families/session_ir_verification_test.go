package families

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// These are schema/store fixtures, not claims that a worker actually executed.
// Only the host verifier can provide the execution custody used by Learning.
func sessionIRVerificationFixture() canonical.ExecutionTestProof {
	skipped := false
	return canonical.ExecutionTestProof{Version: 1, Session: "session-1", Turn: "turn-1", Actor: "actor", ScopeKind: "project", ScopeID: "approved-project", Isolation: "private_source_snapshot",
		SnapshotID: strings.Repeat("a", 32), SnapshotCommit: strings.Repeat("b", 40), FinalCommit: strings.Repeat("b", 40), SnapshotTree: strings.Repeat("c", 40), FinalTree: strings.Repeat("c", 40),
		ConfigurationDigest: strings.Repeat("d", 64), FinalConfigurationDigest: strings.Repeat("d", 64), ProfileDigest: strings.Repeat("e", 64), Completed: true, ObservedAt: "2026-10-20T00:00:00Z",
		Steps: []canonical.ExecutionTestStep{{Name: "test", CommandDigest: strings.Repeat("f", 64), Ran: true, Skipped: &skipped, Termination: "exited", ExitCode: "0"}}}
}
func sessionIRVerificationEvent(t *testing.T, proof canonical.ExecutionTestProof) string {
	t.Helper()
	event := map[string]any{"kind": "execution", "payload": canonical.ExecutionPayload{Status: "completed", VerificationProofs: []canonical.ExecutionTestProof{proof}}}
	encoded, err := json.Marshal(event)
	if err != nil {
		t.Fatal(err)
	}
	return string(encoded)
}
func TestSessionIRVerifiedAppendIsHostOnlyAndOrdinaryAppendCannotForgeProofs(t *testing.T) {
	body := sessionIRVerificationEvent(t, sessionIRVerificationFixture())
	fields := []string{"session-1", "terminal", "turn-1", "actor", body}
	frame, err := wire.EncodeFields(opSessionIRAppendVerified, fields)
	if err != nil {
		t.Fatal(err)
	}
	db := &sessDB{}
	_, wireStatus := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
	if wireStatus != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("caller-authored proof reached host operation", wireStatus, db.executed)
	}
	ordinary := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
	status, _, err := sessionIRAppend(context.Background(), ordinary, fields)
	if err != nil || status != store.StatusInvalid || len(ordinary.reads) != 0 {
		t.Fatal("ordinary append persisted verification authority", status, err, ordinary.reads)
	}
	host := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
	status, cells, err := sessionIRAppendVerified(context.Background(), host, fields)
	if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
		t.Fatal("bound host proof not admitted", status, cells, err)
	}
}
func TestSessionIRVerifiedAppendBindsEveryProofBeforeStorage(t *testing.T) {
	for name, mutate := range map[string]func(*canonical.ExecutionTestProof){
		"session": func(p *canonical.ExecutionTestProof) { p.Session = "foreign" }, "turn": func(p *canonical.ExecutionTestProof) { p.Turn = "foreign" }, "actor": func(p *canonical.ExecutionTestProof) { p.Actor = "foreign" },
		"scope kind": func(p *canonical.ExecutionTestProof) { p.ScopeKind = "global" }, "malformed source": func(p *canonical.ExecutionTestProof) { p.SnapshotCommit = "not-an-object" },
	} {
		t.Run(name, func(t *testing.T) {
			proof := sessionIRVerificationFixture()
			mutate(&proof)
			db := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
			status, _, err := sessionIRAppendVerified(context.Background(), db, []string{"session-1", "terminal", "turn-1", "actor", sessionIRVerificationEvent(t, proof)})
			if err != nil || status != store.StatusInvalid || len(db.reads) != 0 {
				t.Fatal("foreign proof reached storage", status, err, db.reads)
			}
		})
	}
	valid := sessionIRVerificationEvent(t, sessionIRVerificationFixture())
	for _, body := range []string{`{"kind":"execution","payload":{"status":"completed"}}`, strings.Replace(valid, `"status":"completed"`, `"status":"running"`, 1), strings.Replace(valid, `"status":"completed"`, `"status":"failed"`, 1)} {
		db := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
		status, _, err := sessionIRAppendVerified(context.Background(), db, []string{"session-1", "terminal", "turn-1", "actor", body})
		if err != nil || status != store.StatusInvalid || len(db.reads) != 0 {
			t.Fatal("non-terminal proof reached storage", status, err)
		}
	}
	// One correctly bound proof cannot hide another actor's metadata in the array.
	var event map[string]any
	json.Unmarshal([]byte(valid), &event)
	foreign := sessionIRVerificationFixture()
	foreign.Actor = "other"
	event["payload"].(map[string]any)["verification_proofs"] = []canonical.ExecutionTestProof{sessionIRVerificationFixture(), foreign}
	encoded, _ := json.Marshal(event)
	db := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
	status, _, err := sessionIRAppendVerified(context.Background(), db, []string{"session-1", "terminal", "turn-1", "actor", string(encoded)})
	if err != nil || status != store.StatusInvalid || len(db.reads) != 0 {
		t.Fatal("unbound second proof reached storage", status, err)
	}
}

func TestExecutionGradeProofMustEqualDurableTerminalProof(t *testing.T) {
	proof := sessionIRVerificationFixture()
	payload := canonical.ExecutionPayload{Status: "completed", VerificationProofs: []canonical.ExecutionTestProof{proof}}
	raw, _ := json.MarshalIndent(proof, "", "  ")
	if !executionProofInTerminal(raw, payload) {
		t.Fatal("JSON whitespace changed typed proof identity")
	}
	payload.VerificationProofs = nil
	if executionProofInTerminal(raw, payload) {
		t.Fatal("proof absent from durable terminal accepted")
	}
	payload.VerificationProofs = []canonical.ExecutionTestProof{proof}
	changed := proof
	changed.ProfileDigest = strings.Repeat("f", 64)
	foreign, _ := json.Marshal(changed)
	if executionProofInTerminal(foreign, payload) {
		t.Fatal("different full proof matched terminal metadata")
	}
	var fields map[string]any
	json.Unmarshal(raw, &fields)
	fields["foreign_authority"] = true
	foreign, _ = json.Marshal(fields)
	if executionProofInTerminal(foreign, payload) {
		t.Fatal("unknown proof metadata was silently ignored")
	}
}
