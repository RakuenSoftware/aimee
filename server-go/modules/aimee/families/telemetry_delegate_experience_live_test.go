package families

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	memory "github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
)

func delegateBindingFixture() canonical.ChildExecutionPayload {
	return canonical.ChildExecutionPayload{Session: "delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", Turn: "turn", Role: "engineer", Status: "running", LearningBinding: &canonical.DelegateLearningBinding{Version: 1, JobID: "1", ScopeKind: "project", ScopeID: "project-a", Agent: "fixture", Provider: "provider", Model: "model"}}
}
func TestDelegateExperienceHostCustodyAndCanonicalSchema(t *testing.T) {
	child := delegateBindingFixture()
	raw := mustJSON(map[string]any{"kind": "child_execution", "payload": child})
	if !canonical.ValidSessionEvent([]byte(raw)) {
		t.Fatal("invalid shared binding")
	}
	fields := []string{"parent", "child-running", "turn", "operator", raw}
	db := &irAppendFixture{sessDB: &sessDB{}, inserted: 1}
	status, _, err := sessionIRAppend(context.Background(), db, fields)
	if err != nil || status != store.StatusInvalid || len(db.reads) != 0 {
		t.Fatal("ordinary append granted custody", status, err)
	}
	if !sessionIRProofBindings([]byte(raw), "parent", "turn", "operator", true) || sessionIRProofBindings([]byte(raw), "parent", "foreign", "operator", true) {
		t.Fatal("host binding turn fence")
	}
	for _, bad := range []string{strings.Replace(raw, `"job_id":"1"`, `"job_id":"2147483648"`, 1), strings.Replace(raw, `"agent":"fixture"`, `"agent":"`+strings.Repeat("a", 64)+`"`, 1), strings.Replace(raw, `"learning_binding":`+mustJSON(child.LearningBinding), `"learning_binding":null`, 1)} {
		if canonical.ValidSessionEvent([]byte(bad)) {
			t.Fatal("invalid metadata accepted", bad)
		}
	}
	for _, operation := range []uint32{opDelegateExecutionExperienceRecord, opDelegateExecutionTerminalLookup} {
		frame, _ := wire.EncodeFields(operation, []string{"operator", "{}"})
		other := newDelDB()
		_, wireStatus := Telemetry.Handler(other)(ring.ModuleInvocation{StageID: StageTelemetry, PrincipalRef: 8}, frame)
		if wireStatus != ring.ModuleStatusInvalidRequest || len(other.executed) != 0 {
			t.Fatal("foreign module publisher/lookup", operation, wireStatus)
		}
	}
	status, _, err = interactionEventRecord(context.Background(), nil, []string{"delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", "delegate_experience", "system", "{}", "observed"})
	if err != nil || status != store.StatusInvalid {
		t.Fatal("generic publisher forged experience", status, err)
	}
}
func TestDelegateExperiencePostgresNextRunReplayAndSourceFences(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	schema := fmt.Sprintf("delegate_experience_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql", "schema_telemetry.sql", "schema_delegation.sql"} {
		body, _ := schemaFS.ReadFile(name)
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := pool.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`INSERT INTO server_sessions(id,principal) VALUES('parent','operator'),('delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c','operator')`)
	exec(`INSERT INTO agent_jobs(id,role,agent_name,status) VALUES(1,'engineer','fixture','running')`)
	appendEvent := func(session, id, body string, private bool) string {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		var status uint32
		var fields []string
		if private {
			status, fields, err = sessionIRAppendVerified(ctx, sessionIRLiveTx{tx}, []string{session, id, "turn", "operator", body})
		} else {
			status, fields, err = sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{session, id, "turn", "operator", body})
		}
		if err != nil || status != store.StatusOK {
			t.Fatal("source append", status, err, body)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		return fields[0]
	}
	parentRaw := mustJSON(map[string]any{"kind": "child_execution", "payload": delegateBindingFixture()})
	parent := executionGradeSource{Kind: "canonical_session_log", Session: "parent", Turn: "turn", Sequence: appendEvent("parent", "child-running", parentRaw, true), Event: "child-running", Actor: "operator"}
	actionRaw := `{"kind":"message","message_id":"call","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"call-1","name":"git_verify","input":{"action":"run"}}]}}`
	appendEvent("delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", "action", actionRaw, false)
	// Cross a decimal boundary within this exact child turn. The wire source
	// locator is text, but observed action order must remain numeric.
	for n := 2; n <= 8; n++ {
		appendEvent("delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", fmt.Sprintf("filler-%d", n), fmt.Sprintf(`{"kind":"message","message_id":"filler-%d","payload":{"role":"user","blocks":[{"type":"text","text":"observation"}]}}`, n), false)
	}
	for _, n := range []int{9, 10} {
		body := fmt.Sprintf(`{"kind":"message","message_id":"call-%d","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"call-%d","name":"git_verify","input":{"action":"run"}}]}}`, n, n)
		if seq := appendEvent("delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", fmt.Sprintf("action-%d", n), body, false); seq != fmt.Sprint(n) {
			t.Fatal("decimal boundary setup", seq, n)
		}
	}
	// Exact recovery must not depend on a bounded replay scan. These are
	// unrelated earlier turns, not evidence for this attempt's projection.
	exec(`INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload)
          SELECT $1,n,'older-'||n,'older-turn','operator','{"kind":"execution","payload":{"status":"completed"}}'
          FROM generate_series(11,17001) n`, "delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c")
	proof := sessionIRVerificationFixture()
	proof.Session = "delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c"
	proof.Turn = "turn"
	proof.Actor = "operator"
	proof.ScopeID = "project-a"
	terminal := sessionIRVerificationEvent(t, proof)
	source := executionGradeSource{Kind: "canonical_session_log", Session: "delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", Turn: "turn", Sequence: appendEvent("delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c", "terminal", terminal, true), Event: "terminal", Actor: "operator"}
	txLookup, _ := pool.Begin(ctx)
	lookupStatus, lookupFields, lookupErr := delegateExecutionTerminalLookup(ctx, sessionIRLiveTx{txLookup}, []string{"operator", mustJSON(parent)})
	txLookup.Rollback(ctx)
	var lookedUp struct {
		Version  int             `json:"version"`
		Session  string          `json:"session_id"`
		Turn     string          `json:"turn_id"`
		Sequence string          `json:"sequence"`
		EventID  string          `json:"event_id"`
		Actor    string          `json:"actor"`
		Event    json.RawMessage `json:"event"`
	}
	if lookupErr != nil || lookupStatus != store.StatusOK || len(lookupFields) != 1 || !canonical.Decode([]byte(lookupFields[0]), &lookedUp) || lookedUp.Sequence != "17002" || lookedUp.EventID != "terminal" || lookedUp.Session != source.Session || lookedUp.Actor != "operator" || lookedUp.Version != 1 || lookedUp.Turn != "turn" || !canonical.ValidSessionEvent(lookedUp.Event) {
		t.Fatal("exact terminal lookup beyond replay cap", lookupStatus, lookupErr, lookupFields)
	}
	publish := func(principal string) (uint32, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback(ctx)
		status, _, err := delegateExecutionExperienceRecord(ctx, sessionIRLiveTx{tx}, []string{principal, mustJSON(parent)})
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, err
	}
	if status, err := publish("foreign"); err != nil || status != store.StatusInvalid {
		t.Fatal("foreign publication", status, err)
	}
	if status, err := publish("operator"); err != nil || status != store.StatusMissing {
		t.Fatal("proof without derived grade was misqualified", status, err)
	}
	proofRaw, _ := json.Marshal(proof)
	grade := localExecutionTestGrade{SchemaVersion: 1, Source: source, ObservedStatus: "completed", Verification: "tests_bound_revision", Qualification: "configured_tests_only", Verdict: "passed", VerificationID: "sha256:" + strings.Repeat("a", 64), Proof: proofRaw}
	tx, _ := pool.Begin(ctx)
	status, _, err := executionTestGradeRecord(ctx, sessionIRLiveTx{tx}, []string{"operator", mustJSON(grade)})
	if err != nil || status != store.StatusOK {
		t.Fatal("grade publication", status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	failures := make(chan string, 8)
	for n := 0; n < 8; n++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if status, err := publish("operator"); err != nil || status != store.StatusOK {
				failures <- fmt.Sprint(status, err)
			}
		}()
	}
	wg.Wait()
	close(failures)
	for failure := range failures {
		t.Fatal(failure)
	}
	counts := func() {
		t.Helper()
		var events, learnings, unsafe int
		if err = pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='delegate_experience'`).Scan(&events); err != nil {
			t.Fatal(err)
		}
		if err = pool.QueryRow(ctx, `SELECT count(*),count(*) FILTER(WHERE auto_applied<>0 OR confidence<>0 OR review_status<>'pending') FROM delegate_learnings`).Scan(&learnings, &unsafe); err != nil {
			t.Fatal(err)
		}
		if events != 1 || learnings != 1 || unsafe != 0 {
			t.Fatal("replay counts/authority", events, learnings, unsafe)
		}
	}
	counts()
	var experienceJSON string
	if err = pool.QueryRow(ctx, `SELECT payload FROM interaction_events WHERE event_type='delegate_experience'`).Scan(&experienceJSON); err != nil {
		t.Fatal(err)
	}
	var ordered delegateExperience
	if !canonical.Decode([]byte(experienceJSON), &ordered) || len(ordered.Actions) != 3 || ordered.Actions[0].Sequence != "1" || ordered.Actions[1].Sequence != "9" || ordered.Actions[2].Sequence != "10" {
		t.Fatal("canonical action sequence must remain numeric across 9 to 10", experienceJSON)
	}
	// Simulate the lost post-terminal grade projection. Parent replay must
	// recover that authoritative terminal's grade before publishing experience.
	exec(`DELETE FROM interaction_events WHERE event_type='task_outcome'`)
	if status, err := publish("operator"); err != nil || status != store.StatusMissing {
		t.Fatal("missing grade silently downgraded", status, err)
	}
	tx, _ = pool.Begin(ctx)
	status, _, err = executionTestGradeRecord(ctx, sessionIRLiveTx{tx}, []string{"operator", mustJSON(grade)})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if status, err := publish("operator"); err != nil || status != store.StatusOK {
		t.Fatal("grade replay repair", status, err)
	}
	counts()
	// Losing either independently derived projection must be repaired once.
	exec(`DELETE FROM interaction_events WHERE event_type='delegate_experience' AND outcome='tests_passed'`)
	if status, err := publish("operator"); err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	counts()
	exec(`DELETE FROM delegate_learnings WHERE failure_mode='tests_passed'`)
	if status, err := publish("operator"); err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	counts()
	recall := func(actor, project, wanted string) string {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := executionTestGradeRecall(ctx, sessionIRLiveTx{tx}, []string{actor, project, wanted})
		if err != nil || status != store.StatusOK {
			t.Fatal("recall", status, err)
		}
		return cells[0]
	}
	raw := recall("operator", "project-a", "")
	if !strings.Contains(raw, `"delegate_experience"`) || !strings.Contains(raw, `"name":"git_verify"`) || !strings.Contains(raw, `"action_coverage":"bounded_canonical_tool_calls"`) {
		t.Fatal("qualified history missing actual calls", raw)
	}
	for _, foreign := range []string{recall("operator", "project-b", ""), recall("foreign", "project-a", "")} {
		if strings.Contains(foreign, `"delegate_experience"`) {
			t.Fatal("foreign experience", foreign)
		}
	}
	// Exercise the real prepare/check path with the complete projected receipt.
	handler := memory.NewHandler(nil, memory.WithDataStore(memory.PlacementServer, nil))
	runtime := func(args map[string]any) map[string]any {
		t.Helper()
		body, _ := json.Marshal(args)
		frame, _ := ring.EncodeCommand("runtime", body)
		response, status := handler(ring.ModuleInvocation{StageID: memory.StageCommand}, frame)
		body, err := ring.DecodeCommandResult(response)
		var out map[string]any
		if status != ring.ModuleStatusOK || err != nil || json.Unmarshal(body, &out) != nil {
			t.Fatal(status, err, string(response))
		}
		return out
	}
	assembled := runtime(map[string]any{"operation": "ingress-assemble", "project": "project-a", "budget": 20000, "execution_test_grades": json.RawMessage(raw), "prepare_source_release": true, "request_id": "later", "principal": "operator"})
	if !strings.Contains(fmt.Sprint(assembled["envelope"]), "git_verify") || !strings.Contains(fmt.Sprint(assembled["envelope"]), "configured_tests_only") {
		t.Fatal("next-run context", assembled)
	}
	planArgs := map[string]any{"operation": "source-release-plan", "request_id": "later", "principal": "operator", "project": "project-a", "source_release_ticket": assembled["source_release_ticket"]}
	plan := runtime(planArgs)
	wanted := mustJSON(plan["execution_grade_request"])
	// Parent ownership changes after preparation must revoke the joined experience.
	exec(`UPDATE server_sessions SET principal='foreign' WHERE id='parent'`)
	changed := recall("operator", "project-a", wanted)
	if strings.Contains(changed, `"delegate_experience"`) || !strings.Contains(changed, grade.VerificationID) {
		t.Fatal("parent revocation must retain independently valid grade only", changed)
	}
	answer := map[string]any{}
	for k, v := range planArgs {
		answer[k] = v
	}
	answer["operation"] = "source-release-result"
	answer["execution_grade_response"] = json.RawMessage(changed)
	if admitted := runtime(answer); admitted["status"] != "error" || admitted["kind"] != "stale_context" {
		t.Fatal("provider admitted revoked experience", admitted)
	}
	exec(`UPDATE server_sessions SET principal='operator' WHERE id='parent'`)
	for _, check := range []struct{ name, erase, restore string }{
		{"parent", `DELETE FROM session_ir_events WHERE session_id='parent'`, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES('parent',1,'child-running','turn','operator',$1)`},
		{"action", `DELETE FROM session_ir_events WHERE session_id='delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c' AND event_id='action'`, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES('delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c',1,'action','turn','operator',$1)`},
		{"job", `DELETE FROM agent_jobs WHERE id=1`, `INSERT INTO agent_jobs(id,role,agent_name) VALUES(1,'engineer','fixture')`},
	} {
		t.Run(check.name, func(t *testing.T) {
			exec(check.erase)
			if got := recall("operator", "project-a", ""); strings.Contains(got, `"delegate_experience"`) {
				t.Fatal("erased source recalled", got)
			}
			switch check.name {
			case "parent":
				exec(check.restore, parentRaw)
			case "action":
				exec(check.restore, actionRaw)
			default:
				exec(check.restore)
			}
		})
	}
	// A separate real canonical proof-less execution remains ungraded even
	// though its process/execution completed normally.
	exec(`INSERT INTO server_sessions(id,principal) VALUES('delegate-8d13f23515d52e055d585cba72acf62aac1338219dc5c53109af1e663b006e0e','operator')`)
	unverified := delegateBindingFixture()
	unverified.Session = "delegate-8d13f23515d52e055d585cba72acf62aac1338219dc5c53109af1e663b006e0e"
	unverified.LearningBinding.JobID = "2"
	exec(`INSERT INTO agent_jobs(id,role,agent_name) VALUES(2,'engineer','fixture')`)
	unverifiedParent := executionGradeSource{Kind: "canonical_session_log", Session: "parent", Turn: "turn", Sequence: appendEvent("parent", "unverified-running", mustJSON(map[string]any{"kind": "child_execution", "payload": unverified}), true), Event: "unverified-running", Actor: "operator"}
	appendEvent("delegate-8d13f23515d52e055d585cba72acf62aac1338219dc5c53109af1e663b006e0e", "unverified-terminal", `{"kind":"execution","payload":{"status":"completed"}}`, false)
	tx, _ = pool.Begin(ctx)
	status, _, err = delegateExecutionExperienceRecord(ctx, sessionIRLiveTx{tx}, []string{"operator", mustJSON(unverifiedParent)})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	var observed string
	if err = pool.QueryRow(ctx, `SELECT evidence_json FROM delegate_learnings WHERE session_id='delegate-8d13f23515d52e055d585cba72acf62aac1338219dc5c53109af1e663b006e0e'`).Scan(&observed); err != nil || !strings.Contains(observed, `"qualification":"ungraded"`) || strings.Contains(observed, `"verdict"`) {
		t.Fatal("proof-less completion promoted", err, observed)
	}
	// Projection integrity only: one observed failed job has one protected
	// canonical parent/child source and stays ungraded on every replay. The C
	// exit-metric fixture independently prohibits the retired unbound writer.
	failed := delegateBindingFixture()
	failed.Session = fmt.Sprintf("delegate-%x", sha256.Sum256([]byte("parent:3:durable")))
	failed.LearningBinding.JobID = "3"
	exec(`INSERT INTO server_sessions(id,principal) VALUES($1,'operator')`, failed.Session)
	exec(`INSERT INTO agent_jobs(id,role,agent_name,status) VALUES(3,'engineer','fixture','failed')`)
	failedSource := executionGradeSource{Kind: "canonical_session_log", Session: "parent", Turn: "turn", Event: "failed-child-running", Actor: "operator"}
	failedSource.Sequence = appendEvent("parent", failedSource.Event, mustJSON(map[string]any{"kind": "child_execution", "payload": failed}), true)
	appendEvent(failed.Session, "failed-running", `{"kind":"execution","payload":{"status":"running"}}`, false)
	appendEvent(failed.Session, "failed-terminal", `{"kind":"execution","payload":{"status":"failed","error":"proxy unavailable"}}`, false)
	for replay := 0; replay < 3; replay++ {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		status, _, err := delegateExecutionExperienceRecord(ctx, sessionIRLiveTx{tx}, []string{"operator", mustJSON(failedSource)})
		if err != nil || status != store.StatusOK {
			_ = tx.Rollback(ctx)
			t.Fatal("failed observation", status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	var failedRows, unboundRows int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM delegate_learnings WHERE session_id=$1 AND evidence_json::jsonb->>'qualification'='ungraded' AND evidence_json::jsonb->>'observed_status'='failed' AND review_status='pending' AND confidence=0 AND auto_applied=0`, failed.Session).Scan(&failedRows); err != nil || failedRows != 1 {
		t.Fatal("one failed job must project exactly one ungraded observation", failedRows, err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM delegate_learnings WHERE session_id='parent' OR evidence_json::jsonb->>'kind' IS DISTINCT FROM 'delegate_execution_experience'`).Scan(&unboundRows); err != nil || unboundRows != 0 {
		t.Fatal("unbound legacy learning must not be a second publisher", unboundRows, err)
	}

	exec(`DELETE FROM session_ir_events WHERE session_id='delegate-78fd1d63652c1015a4c0273c7a7385f20d967b1cfdca0699cde6a010c820444c' AND event_id='terminal'`)
	if got := recall("operator", "project-a", ""); strings.Contains(got, grade.VerificationID) {
		t.Fatal("deleted terminal recalled", got)
	}
}
