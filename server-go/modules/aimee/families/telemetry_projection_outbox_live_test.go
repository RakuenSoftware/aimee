package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	learning "github.com/JBailes/aimee/server-go/modules/learning"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestLearningProjectionHostOnly(t *testing.T) {
	for _, operation := range []uint32{opLearningProjectionClaim, opLearningProjectionAck} {
		args := []string{}
		if operation == opLearningProjectionAck {
			args = []string{"aabb", "delivered"}
		}
		frame, _ := wire.EncodeFields(operation, args)
		db := newDelDB()
		_, status := Telemetry.Handler(db)(bus.ModuleInvocation{StageID: StageTelemetry, PrincipalRef: 7}, frame)
		if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal("untrusted recovery", status, db.executed)
		}
	}
}

type projectionTest struct {
	t    *testing.T
	ctx  context.Context
	pool *pgxpool.Pool
}

func newProjectionTest(t *testing.T) *projectionTest {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	t.Cleanup(cancel)
	schema := fmt.Sprintf("learning_outbox_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { pool.Close(); root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE") })
	f := &projectionTest{t, ctx, pool}
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_telemetry.sql", "schema_delegation.sql", "schema_learning_projection_outbox.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		f.exec(string(body))
	}
	f.exec(`INSERT INTO server_sessions(id,principal) VALUES('s','alice')`)
	return f
}
func (f *projectionTest) exec(sql string, args ...any) {
	f.t.Helper()
	if _, err := f.pool.Exec(f.ctx, sql, args...); err != nil {
		f.t.Fatal(err)
	}
}
func (f *projectionTest) run(op store.OpFunc, fields ...string) (uint32, []string) {
	f.t.Helper()
	tx, err := f.pool.Begin(f.ctx)
	if err != nil {
		f.t.Fatal(err)
	}
	defer tx.Rollback(f.ctx)
	status, result, err := op(f.ctx, sessionIRLiveTx{tx}, fields)
	if err != nil {
		f.t.Fatal(err)
	}
	if status == store.StatusOK {
		if err = tx.Commit(f.ctx); err != nil {
			f.t.Fatal(err)
		}
	}
	return status, result
}
func (f *projectionTest) append(id string) executionGradeSource {
	f.t.Helper()
	status, fields := f.run(sessionIRAppend, "s", id, id, "alice", `{"kind":"execution","payload":{"status":"completed"}}`)
	if status != store.StatusOK {
		f.t.Fatal(status)
	}
	return executionGradeSource{Kind: "canonical_session_log", Session: "s", Sequence: fields[0], Event: id, Turn: id, Actor: "alice"}
}
func (f *projectionTest) claim() []projectionTicket {
	f.t.Helper()
	status, fields := f.run(learningProjectionClaim)
	if status != store.StatusOK {
		f.t.Fatal(status)
	}
	var batch struct {
		Coverage string             `json:"coverage"`
		Cutoff   string             `json:"cutoff"`
		Items    []projectionTicket `json:"items"`
	}
	if json.Unmarshal([]byte(fields[0]), &batch) != nil || batch.Coverage != "canonical_appends_since_cutover" || batch.Cutoff == "" {
		f.t.Fatal(fields)
	}
	return batch.Items
}
func (f *projectionTest) observe(source executionGradeSource) {
	f.t.Helper()
	raw := mustJSON(map[string]any{"schema_version": 1, "source": source, "observed_status": "completed", "verification": "ungraded"})
	status, _ := f.run(interactionEventRecord, source.Session, "task_outcome", "system", raw, "observed")
	if status != store.StatusOK {
		f.t.Fatal(status)
	}
}

func TestLearningProjectionPostgresAtomicRetryRetentionErase(t *testing.T) {
	f := newProjectionTest(t)
	tx, err := f.pool.Begin(f.ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err := sessionIRAppend(f.ctx, sessionIRLiveTx{tx}, []string{"s", "rolled-back", "rolled-back", "alice", `{"kind":"execution","payload":{"status":"completed"}}`})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	tx.Rollback(f.ctx)
	if got := f.claim(); len(got) != 0 {
		t.Fatal("rolled back source queued", got)
	}
	source := f.append("completed")
	tickets := f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusMissing {
		t.Fatal("unpublished source acknowledged", status)
	}
	f.observe(source)
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusOK {
		t.Fatal(status)
	}
	f.exec(`DELETE FROM interaction_events`)
	f.append("completed") // Existing append retry must not requeue retained acknowledgment.
	if got := f.claim(); len(got) != 0 {
		t.Fatal("retention resurrected delivered source", got)
	}
	var state string
	if err = f.pool.QueryRow(f.ctx, `SELECT state FROM learning_projection_outbox WHERE session_id='s'`).Scan(&state); err != nil || state != "delivered" {
		t.Fatal(state, err)
	}
	f.append("erased")
	tickets = f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	f.exec(`DELETE FROM session_ir_events WHERE event_id='erased'`)
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusMissing {
		t.Fatal("erased source acknowledged", status)
	}
	f.append("foreign-owner")
	tickets = f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	f.exec(`UPDATE server_sessions SET principal='bob' WHERE id='s'`)
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "retry"); status != store.StatusOK {
		t.Fatal(status)
	}
	if err = f.pool.QueryRow(f.ctx, `SELECT state FROM learning_projection_outbox WHERE event_id='foreign-owner'`).Scan(&state); err != nil || state != "revoked" {
		t.Fatal(state, err)
	}
}
func TestLearningProjectionPostgresConcurrentLeaseAndBound(t *testing.T) {
	f := newProjectionTest(t)
	f.append("one")
	var wait sync.WaitGroup
	var mu sync.Mutex
	tickets := []projectionTicket{}
	for i := 0; i < 2; i++ {
		wait.Add(1)
		go func() { defer wait.Done(); got := f.claim(); mu.Lock(); tickets = append(tickets, got...); mu.Unlock() }()
	}
	wait.Wait()
	if len(tickets) != 1 {
		t.Fatal("concurrent duplicate lease", tickets)
	}
	old := tickets[0].Lease
	f.exec(`UPDATE learning_projection_outbox SET lease_until=now()-interval '1 second'`)
	tickets = f.claim()
	if len(tickets) != 1 || tickets[0].Lease == old {
		t.Fatal(tickets)
	}
	if status, _ := f.run(learningProjectionAck, old, "retry"); status != store.StatusMissing {
		t.Fatal("stale lease accepted", status)
	}
	if status, _ := f.run(learningProjectionAck, tickets[0].Lease, "retry"); status != store.StatusOK {
		t.Fatal(status)
	}
	for i := 0; i < 7; i++ {
		f.append(fmt.Sprintf("next-%d", i))
	}
	if got := f.claim(); len(got) != 4 {
		t.Fatal("unbounded batch", len(got))
	}
}

func TestLearningProjectionPostgresProtectedChildReadinessAndRevocation(t *testing.T) {
	f := newProjectionTest(t)
	child := delegateBindingFixture()
	f.exec(`INSERT INTO server_sessions(id,principal) VALUES('parent','alice'),($1,'alice')`, child.Session)
	f.exec(`INSERT INTO agent_jobs(id,role,prompt,agent_name,status) VALUES(1,'engineer','unit storage custody fixture','fixture','running')`)
	body := mustJSON(map[string]any{"kind": "child_execution", "payload": child})
	status, fields := f.run(sessionIRAppendVerified, "parent", "running-ref", "turn", "alice", body)
	if status != store.StatusOK {
		t.Fatal(status)
	}
	parent := executionGradeSource{Kind: "canonical_session_log", Session: "parent", Sequence: fields[0], Event: "running-ref", Turn: "turn", Actor: "alice"}
	tickets := f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusMissing {
		t.Fatal("running child delivered", status)
	}
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "retry"); status != store.StatusOK {
		t.Fatal(status)
	}
	status, fields = f.run(sessionIRAppend, child.Session, "child-terminal", "turn", "alice", `{"kind":"execution","payload":{"status":"completed"}}`)
	if status != store.StatusOK {
		t.Fatal(status)
	}
	terminal := executionGradeSource{Kind: "canonical_session_log", Session: child.Session, Sequence: fields[0], Event: "child-terminal", Turn: "turn", Actor: "alice"}
	f.observe(terminal)
	f.exec(`UPDATE learning_projection_outbox SET next_attempt=now()`)
	tickets = f.claim()
	if len(tickets) != 2 {
		t.Fatal(tickets)
	}
	for _, ticket := range tickets {
		if status, _ = f.run(learningProjectionAck, ticket.Lease, "delivered"); status != store.StatusOK {
			t.Fatal(status)
		}
	}
	var count int
	if err := f.pool.QueryRow(f.ctx, `SELECT count(*) FROM delegate_learnings WHERE session_id=$1 AND auto_applied=0 AND confidence=0 AND review_status='pending' AND evidence_json::jsonb->'parent_source'=$2::jsonb AND evidence_json::jsonb->>'qualification'='ungraded'`, child.Session, mustJSON(parent)).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	if err := f.pool.QueryRow(f.ctx, `SELECT count(*) FROM interaction_events WHERE outcome IN ('tests_passed','tests_failed')`).Scan(&count); err != nil || count != 0 {
		t.Fatal("ungraded child acquired grade", count, err)
	}
	// A second source loses its actual job before retry; this is revocation,
	// rather than an invented successful delivery or a forever-ready receipt.
	child.Turn = "other-turn"
	body = mustJSON(map[string]any{"kind": "child_execution", "payload": child})
	status, _ = f.run(sessionIRAppendVerified, "parent", "other-ref", "other-turn", "alice", body)
	if status != store.StatusOK {
		t.Fatal(status)
	}
	tickets = f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	f.exec(`DELETE FROM agent_jobs WHERE id=1`)
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "retry"); status != store.StatusOK {
		t.Fatal(status)
	}
	var state string
	if err := f.pool.QueryRow(f.ctx, `SELECT state FROM learning_projection_outbox WHERE event_id='other-ref'`).Scan(&state); err != nil || state != "revoked" {
		t.Fatal(state, err)
	}
}

func TestLearningProjectionPostgresCompleteEnvelopeByteBudget(t *testing.T) {
	f := newProjectionTest(t)
	empty, err := encodeSessionIR(map[string]any{"kind": "execution", "payload": map[string]any{"status": "failed", "error": ""}})
	if err != nil {
		t.Fatal(err)
	}
	text := strings.Repeat("<", 1048576-len(empty))
	body, err := encodeSessionIR(map[string]any{"kind": "execution", "payload": map[string]any{"status": "failed", "error": text}})
	if err != nil || len(body) != 1048576 {
		t.Fatal("fixture must exercise exact canonical event maximum", len(body), err)
	}
	for _, id := range []string{"large-one", "large-two"} {
		status, _ := f.run(sessionIRAppend, "s", id, id, "alice", string(body))
		if status != store.StatusOK {
			t.Fatal(status)
		}
	}
	status, fields := f.run(learningProjectionClaim)
	if status != store.StatusOK || len(fields) != 1 || len(fields[0]) >= 2097152 {
		t.Fatal("batch exceeded actual C reply capacity", status, len(fields[0]))
	}
	var batch struct {
		Items []projectionTicket `json:"items"`
	}
	if err = json.Unmarshal([]byte(fields[0]), &batch); err != nil || len(batch.Items) != 1 {
		t.Fatal("whole bounded record missing", err, len(batch.Items))
	}
	var record struct {
		Event struct {
			Payload struct {
				Error string `json:"error"`
			} `json:"payload"`
		} `json:"event"`
	}
	if err = json.Unmarshal(batch.Items[0].Record, &record); err != nil || record.Event.Payload.Error != text {
		t.Fatal("large source truncated", err)
	}
	if tickets := f.claim(); len(tickets) != 1 || tickets[0].Lease == batch.Items[0].Lease {
		t.Fatal("byte budget starved next whole source", tickets)
	}
}

// Schema/custody fixtures exercise real Learning qualification and PostgreSQL
// delivery integrity; actual configured execution belongs to the daemon journey.
func TestLearningProjectionPostgresWaitsForEveryOwnedGrade(t *testing.T) {
	f := newProjectionTest(t)
	first := sessionIRVerificationFixture()
	first.Session = "s"
	first.Turn = "proofs"
	first.Actor = "alice"
	second := first
	second.SnapshotID = strings.Repeat("9", 32)
	second.Steps = append([]canonical.ExecutionTestStep(nil), first.Steps...)
	second.Steps[0].ExitCode = "1"
	body := mustJSON(map[string]any{"kind": "execution", "payload": canonical.ExecutionPayload{Status: "completed", VerificationProofs: []canonical.ExecutionTestProof{first, second}}})
	status, fields := f.run(sessionIRAppendVerified, "s", "proof-terminal", "proofs", "alice", body)
	if status != store.StatusOK {
		t.Fatal(status)
	}
	source := executionGradeSource{Kind: "canonical_session_log", Session: "s", Turn: "proofs", Event: "proof-terminal", Sequence: fields[0], Actor: "alice"}
	f.observe(source)
	tickets := f.claim()
	if len(tickets) != 1 {
		t.Fatal(tickets)
	}
	for index, proof := range []canonical.ExecutionTestProof{first, second} {
		if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusMissing {
			t.Fatal("incomplete proof set acknowledged", index, status)
		}
		record := learning.ExecutionRecord{Version: 1, Session: "s", Turn: "proofs", Sequence: source.Sequence, EventID: source.Event, Actor: "alice", Event: json.RawMessage(body)}
		args, _ := json.Marshal(learning.ExecutionTestGradeRequest{Record: record, Proof: proof})
		command, err := bus.EncodeCommandWithContext("grade", args, bus.CommandContext{Authenticated: true, Principal: "alice", TransportIdentity: "host:isolated-project-verification"})
		if err != nil {
			t.Fatal(err)
		}
		reply, moduleStatus := learning.Handle(bus.ModuleInvocation{StageID: learning.StageExecution}, command)
		if moduleStatus != bus.ModuleStatusOK {
			t.Fatal("Learning refused owned fixture", moduleStatus)
		}
		raw, err := bus.DecodeCommandResult(reply)
		if err != nil {
			t.Fatal(err)
		}
		var result struct {
			Grade json.RawMessage `json:"grade"`
		}
		if err = json.Unmarshal(raw, &result); err != nil || len(result.Grade) == 0 {
			t.Fatal(string(raw), err)
		}
		if status, _ = f.run(executionTestGradeRecord, "alice", string(result.Grade)); status != store.StatusOK {
			t.Fatal(status)
		}
	}
	if status, _ = f.run(learningProjectionAck, tickets[0].Lease, "delivered"); status != store.StatusOK {
		t.Fatal("all owned grades not acknowledged", status)
	}
}
