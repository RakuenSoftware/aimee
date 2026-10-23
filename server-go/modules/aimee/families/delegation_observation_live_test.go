package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestAgentJobCanonicalObservationLiveOwnerBinding(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	schema := fmt.Sprintf("job_ir_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_delegation.sql"} {
		raw, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(raw)); e != nil {
			t.Fatal(name, e)
		}
	}
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, e := pool.Exec(ctx, sql, args...); e != nil {
			t.Fatal(e)
		}
	}
	parent, actor, turn := "owned-parent", "owner", "delegate-turn"
	sum := sha256.Sum256([]byte(parent + ":91:durable"))
	child := "delegate-" + hex.EncodeToString(sum[:])
	exec(`INSERT INTO agent_jobs(id,role,agent_name,status,result) VALUES(91,'engineer','selected-agent','done','POISONED legacy result')`)
	exec(`INSERT INTO server_sessions(id,principal) VALUES($1,$3),($2,$3)`, parent, child, actor)
	binding := canonical.ChildExecutionPayload{Session: child, Turn: turn, Role: "engineer", Status: "running", LearningBinding: &canonical.DelegateLearningBinding{Version: 1, JobID: "91", ScopeKind: "project", ScopeID: "owned/project", Agent: "selected-agent", Provider: "claude", Model: "selected-model"}}
	parentRaw := mustJSON(map[string]any{"kind": "child_execution", "payload": binding})
	if !canonical.ValidSessionEvent([]byte(parentRaw)) {
		t.Fatal(parentRaw)
	}
	exec(`INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,1,'binding',$2,$3,$4)`, parent, turn, actor, parentRaw)
	// Deliberately malformed legacy rows must neither fail the new index nor bind.
	exec(`INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,2,'legacy',$2,$3,'{not json')`, parent, turn, actor)
	raw, e := schemaFS.ReadFile("schema_agent_job_observation.sql")
	if e != nil {
		t.Fatal(e)
	}
	exec(string(raw))
	events := []string{`{"kind":"execution","payload":{"status":"running"}}`, `{"kind":"message","message_id":"user","payload":{"role":"user","blocks":[{"type":"text","text":"actual user"}]}}`, `{"kind":"message","message_id":"answer","payload":{"role":"assistant","blocks":[{"type":"text","text":"actual canonical answer"}]}}`, `{"kind":"delta","message_id":"answer","role":"assistant","payload":{"type":"TURN_STOP","stop_reason":"end_turn","usage_in":3,"usage_out":4}}`, `{"kind":"execution","payload":{"status":"completed"}}`}
	for i, event := range events {
		if !canonical.ValidSessionEvent([]byte(event)) {
			t.Fatal(event)
		}
		exec(`INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,$2,$3,$4,$5,$6)`, child, i+1, fmt.Sprint("child-", i+1), turn, actor, event)
	}
	run := func(who string) (uint32, []string, error) {
		t.Helper()
		tx, e := pool.Begin(ctx)
		if e != nil {
			t.Fatal(e)
		}
		defer tx.Rollback(ctx)
		s, c, e := Delegation.Ops[opAgentJobCanonicalObservation].Run(ctx, sessionIRLiveTx{tx}, []string{"91", who})
		if e == nil && s == store.StatusOK {
			e = tx.Commit(ctx)
		}
		return s, c, e
	}
	require := func(want uint32) {
		t.Helper()
		s, c, e := run(actor)
		if e != nil || s != want {
			t.Fatal(s, c, e)
		}
	}
	s, c, e := run(actor)
	if e != nil || s != store.StatusOK || len(c) != 1 {
		t.Fatal(s, c, e)
	}
	var got agentJobObservation
	if json.Unmarshal([]byte(c[0]), &got) != nil || got.Child.Session != child || got.LastSequence != "5" || got.Status != "completed" || got.Terminal == nil || got.Terminal.Sequence != "5" || len(got.Events) != 5 || !strings.Contains(string(got.History), "actual canonical answer") || strings.Contains(c[0], "POISONED") {
		t.Fatal(c)
	}

	// Actual append-owner and retained-job custody stay locked until the read
	// transaction completes, so a reply cannot mix a changed owner/profile.
	held, e := pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer held.Rollback(context.Background())
	hs, _, he := agentJobCanonicalObservation(ctx, sessionIRLiveTx{held}, []string{"91", actor})
	if he != nil || hs != store.StatusOK {
		t.Fatal(hs, he)
	}
	for _, statement := range []string{`UPDATE server_sessions SET principal='changed' WHERE id='` + child + `'`, `UPDATE agent_jobs SET agent_name='changed' WHERE id=91`} {
		writer, e := pool.Acquire(ctx)
		if e != nil {
			t.Fatal(e)
		}
		writerTx, e := writer.Begin(ctx)
		if e != nil {
			t.Fatal(e)
		}
		blocked, cancelBlocked := context.WithTimeout(ctx, 200*time.Millisecond)
		_, be := writerTx.Exec(blocked, statement)
		timedOut := blocked.Err() != nil
		cancelBlocked()
		writerTx.Rollback(context.Background()) // no timed-out write is ever committed
		writer.Release()
		if be == nil || !timedOut {
			t.Fatal("source custody did not hold concurrent writer", be)
		}
	}
	held.Rollback(ctx)
	// The lookup must use the job/actor expression index, not a global history scan.
	tx, e := pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = tx.Exec(ctx, "SET LOCAL enable_seqscan=off"); e != nil {
		t.Fatal(e)
	}
	plan, e := tx.Query(ctx, "EXPLAIN "+agentJobBindingSQL, "91", actor)
	if e != nil {
		t.Fatal(e)
	}
	var planText strings.Builder
	for plan.Next() {
		var line string
		if e = plan.Scan(&line); e != nil {
			t.Fatal(e)
		}
		planText.WriteString(line)
	}
	plan.Close()
	if !strings.Contains(planText.String(), "idx_session_ir_delegate_job_actor") {
		t.Fatal("binding lookup lost index", planText.String())
	}
	tx.Rollback(ctx)
	_, again, e := run(actor)
	if e != nil || len(again) != 1 || again[0] != c[0] {
		t.Fatal("read observation changed immutable canonical bytes", e)
	}
	if !Delegation.Ops[opAgentJobCanonicalObservation].HostOnly || !Delegation.Ops[opAgentJobCanonicalObservation].Tx {
		t.Fatal("private transactional source authority lost")
	}
	if s, c, e = run("foreign"); e != nil || s != store.StatusInvalid || len(c) != 0 {
		t.Fatal("foreign actor", s, c, e)
	}
	exec(`UPDATE agent_jobs SET role='tampered' WHERE id=91`)
	require(store.StatusInvalid)
	exec(`UPDATE agent_jobs SET role='engineer',agent_name='tampered' WHERE id=91`)
	require(store.StatusInvalid)
	exec(`UPDATE agent_jobs SET agent_name='selected-agent' WHERE id=91`)
	exec(`UPDATE server_sessions SET principal='foreign' WHERE id=$1`, child)
	require(store.StatusInvalid)
	exec(`UPDATE server_sessions SET principal=$2 WHERE id=$1`, child, actor)
	exec(`UPDATE server_sessions SET principal='foreign' WHERE id=$1`, parent)
	require(store.StatusInvalid)
	exec(`UPDATE server_sessions SET principal=$2 WHERE id=$1`, parent, actor)
	forged := binding
	forged.Session = "delegate-forged"
	exec(`UPDATE session_ir_events SET payload=$2 WHERE session_id=$1 AND sequence=1`, parent, mustJSON(map[string]any{"kind": "child_execution", "payload": forged}))
	s, _, e = run(actor)
	if s != store.StatusInvalid || e != nil {
		t.Fatal("forged identity", s, e)
	}
	exec(`UPDATE session_ir_events SET payload=$2 WHERE session_id=$1 AND sequence=1`, parent, parentRaw)
	// Two independently owned parents claiming a job are ambiguous, never latest-wins.
	exec(`INSERT INTO server_sessions(id,principal) VALUES('other-parent',$1)`, actor)
	sum = sha256.Sum256([]byte("other-parent:91:durable"))
	forged.Session = "delegate-" + hex.EncodeToString(sum[:])
	exec(`INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES('other-parent',1,'other-binding',$1,$2,$3)`, turn, actor, mustJSON(map[string]any{"kind": "child_execution", "payload": forged}))
	s, _, e = run(actor)
	if s != store.StatusInvalid || e != nil {
		t.Fatal("ambiguous parents", s, e)
	}
	exec(`DELETE FROM session_ir_events WHERE session_id='other-parent'`)

	// Preserve typed refusal diagnostics exactly, without interpreting model text.
	declined := `{"kind":"delta","message_id":"answer","role":"assistant","payload":{"type":"TURN_STOP","stop_reason":"content_filter","raw_stop_reason":"refusal","refusal":{"category":"future-category","explanation":"typed diagnostic"}}}`
	exec(`UPDATE session_ir_events SET payload=$2 WHERE session_id=$1 AND sequence=4`, child, declined)
	exec(`UPDATE session_ir_events SET payload='{"kind":"execution","payload":{"status":"failed"}}' WHERE session_id=$1 AND sequence=5`, child)
	s, c, e = run(actor)
	if e != nil || s != store.StatusOK || !strings.Contains(c[0], "future-category") || !strings.Contains(c[0], "typed diagnostic") {
		t.Fatal("refusal diagnostics lost", s, c, e)
	}
	exec(`UPDATE session_ir_events SET payload=$2 WHERE session_id=$1 AND sequence=4`, child, events[3])
	// Running projection includes actual committed messages; no fabricated terminal.
	exec(`DELETE FROM session_ir_events WHERE session_id=$1 AND sequence=5`, child)
	s, c, e = run(actor)
	if e != nil || s != store.StatusOK {
		t.Fatal(s, c, e)
	}
	if json.Unmarshal([]byte(c[0]), &got) != nil || got.Status != "running" || got.Terminal != nil || got.LastSequence != "4" {
		t.Fatal(c)
	}
	exec(`UPDATE session_ir_events SET actor='foreign' WHERE session_id=$1 AND sequence=3`, child)
	require(store.StatusInvalid)
	exec(`UPDATE session_ir_events SET actor=$2 WHERE session_id=$1 AND sequence=3`, child, actor)
	exec(`DELETE FROM session_ir_events WHERE session_id=$1 AND sequence=2`, child)
	require(store.StatusInvalid)
	exec(`DELETE FROM session_ir_events WHERE session_id=$1 AND sequence>1`, child)
	s, c, e = run(actor)
	if e != nil || s != store.StatusOK {
		t.Fatal(s, c, e)
	}
	if json.Unmarshal([]byte(c[0]), &got) != nil || string(got.History) != "[]" || got.Status != "running" || got.Terminal != nil {
		t.Fatal("empty execution lost real source", c)
	}
	exec(`DELETE FROM session_ir_events WHERE session_id=$1`, child)
	require(store.StatusMissing)
	exec(`DELETE FROM server_sessions WHERE id=$1`, child)
	require(store.StatusInvalid)
	exec(`INSERT INTO server_sessions(id,principal) VALUES($1,$2)`, child, actor)
	exec(`DELETE FROM session_ir_events WHERE session_id=$1 AND sequence=1`, parent)
	require(store.StatusMissing)
}

func TestAgentJobCanonicalObservationPrivateAdmission(t *testing.T) {
	frame, _ := wire.EncodeFields(opAgentJobCanonicalObservation, []string{"91", "owner"})
	db := newSessDB()
	_, status := Delegation.Handler(db)(bus.ModuleInvocation{StageID: StageDelegation, PrincipalRef: 7}, frame)
	if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("non-host could observe another actor", status)
	}
	for _, fields := range [][]string{{"0", "owner"}, {"091", "owner"}, {"1", ""}, {"2147483648", "owner"}, {"1", "bad\x00actor"}} {
		status, _, err := agentJobCanonicalObservation(context.Background(), db, fields)
		if err != nil || status != store.StatusInvalid || len(db.executed) != 0 {
			t.Fatal(fields, status, err)
		}
	}
}
