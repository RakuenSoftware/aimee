package families

import (
	"context"
	"encoding/json"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	ep "github.com/JBailes/aimee/server-go/modules/execution-policy"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestCleanRetryLiveSharedRoot(t *testing.T) {
	root := livePool(t)
	ctx := context.Background()
	schema := fmt.Sprintf("mr17_%d", time.Now().UnixNano())
	if _, e := root.Exec(ctx, "CREATE SCHEMA "+schema); e != nil {
		t.Fatal(e)
	}
	defer root.Exec(ctx, "DROP SCHEMA "+schema+" CASCADE")
	cfg, e := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if e != nil {
		t.Fatal(e)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, e := pgxpool.NewWithConfig(ctx, cfg)
	if e != nil {
		t.Fatal(e)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_governed_actions.sql", "schema_clean_retry.sql"} {
		body, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(body)); e != nil {
			t.Fatal(e)
		}
	}
	if _, e = pool.Exec(ctx, `CREATE TABLE session_state(session_id TEXT PRIMARY KEY,task_projection_state TEXT NOT NULL DEFAULT '');INSERT INTO server_sessions(id,principal) VALUES('parent','alice'),('child','alice'),('foreign','mallory');INSERT INTO session_state VALUES('parent','{"revision":"projection:1"}'),('child','');`); e != nil {
		t.Fatal(e)
	}
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if e = os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"clean_retry":{"enabled":true,"max_attempts":3,"max_repeated_failures":2,"max_wall_seconds":600,"max_provider_bytes":1000,"nanodollars_per_byte":2,"max_nanodollars":2000,"retain_input_seconds":600}}`), 0600); e != nil {
		t.Fatal(e)
	}
	handler := GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	call := func(principal, sid string, r governedActionRequest) (ep.RetryDecision, uint32) {
		t.Helper()
		body, _ := json.Marshal(r)
		frame, _ := wire.EncodeFields(10, []string{principal, sid, string(body)})
		raw, status := handler(bus.ModuleInvocation{StageID: StageGuardrailState}, frame)
		if status != bus.ModuleStatusOK {
			t.Errorf("status %v", status)
			return ep.RetryDecision{}, store.StatusInvalid
		}
		code, cells, ok := store.DecodeRequest(raw)
		if !ok {
			t.Error("bad owner frame")
		}
		var d ep.RetryDecision
		if code == store.StatusOK && (len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &d) != nil) {
			t.Error("bad decision")
		}
		return d, code
	}
	if d, s := call("alice", "child", governedActionRequest{Operation: "fork", ParentSession: "parent"}); s != store.StatusOK || !d.Allowed {
		t.Fatal(d, s)
	}
	r := governedActionRequest{Operation: "retry_begin", Retry: ep.RetryRequest{Request: "initial", Input: "Current constraints"}}
	d, s := call("alice", "parent", r)
	if s != store.StatusOK || !d.Allowed {
		t.Fatal(d, s)
	}
	attempt := d.Attempt
	if _, s := call("mallory", "parent", r); s != store.StatusInvalid {
		t.Fatal("foreign principal")
	}
	// Two descendants contend on the same durable provider-byte reservation.
	var admitted atomic.Int64
	var wg sync.WaitGroup
	for n := 0; n < 24; n++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			sid := "parent"
			if n%2 == 1 {
				sid = "child"
			}
			r := governedActionRequest{Operation: "retry_send", Retry: ep.RetryRequest{Attempt: attempt, Call: ep.RetryCall{ID: fmt.Sprint(n), Bytes: 600, Payload: strings.Repeat("a", 64)}}}
			d, s := call("alice", sid, r)
			if s != store.StatusOK {
				t.Errorf("send %v", s)
			}
			if d.Allowed {
				admitted.Add(1)
			}
		}(n)
	}
	wg.Wait()
	if admitted.Load() != 1 {
		t.Fatal("shared ceiling", admitted.Load())
	}
	if d, s := call("alice", "parent", governedActionRequest{Operation: "retry_finish", Retry: ep.RetryRequest{Attempt: attempt, Failure: "execution_failure"}}); s != store.StatusOK || !d.Allowed {
		t.Fatal(d, s)
	}
	// Replace the owner process and disposable projection; neither refunds spend.
	handler = GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	if _, e = pool.Exec(ctx, `UPDATE session_state SET task_projection_state=''`); e != nil {
		t.Fatal(e)
	}
	r = governedActionRequest{Operation: "retry_begin", Retry: ep.RetryRequest{Request: "retry", Previous: attempt, Input: "Replacement constraints", ReplaceConstraints: true}}
	d, s = call("alice", "child", r)
	if s != store.StatusOK || !d.Allowed || d.RemainingBytes != 400 || d.RemainingNanodollars != 800 || d.RemainingAttempts != 1 {
		t.Fatal(d, s)
	}
	// A new client ID cannot reparent an already-bound child and reset its root.
	if d, s := call("alice", "child", governedActionRequest{Operation: "fork", ParentSession: "foreign"}); s == store.StatusOK && d.Allowed {
		t.Fatal("foreign lineage")
	}
	var raw string
	if e = pool.QueryRow(ctx, `SELECT retry_journal FROM governed_action_roots WHERE principal='alice' AND root_id='session:parent'`).Scan(&raw); e != nil {
		t.Fatal(e)
	}
	var j ep.RetryJournal
	if json.Unmarshal([]byte(raw), &j) != nil || j.Attempts[0].Projection != "projection:1" || j.Attempts[1].Projection != "" {
		t.Fatal("projection revision binding")
	}
	j.Cost = 0
	bad, _ := json.Marshal(j)
	if _, e = pool.Exec(ctx, `UPDATE governed_action_roots SET retry_journal=$1`, string(bad)); e != nil {
		t.Fatal(e)
	}
	if _, s := call("alice", "child", governedActionRequest{Operation: "retry_inspect", Retry: ep.RetryRequest{Attempt: d.Attempt}}); s != store.StatusInvalid {
		t.Fatal("corrupt reservation accepted")
	}
}
