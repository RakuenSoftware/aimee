package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	ep "github.com/JBailes/aimee/server-go/modules/execution-policy"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestGovernedActionLiveAtomicOwner(t *testing.T) {
	root := livePool(t)
	ctx := context.Background()
	schema := fmt.Sprintf("mr16_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(ctx, "DROP SCHEMA "+schema+" CASCADE")
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
	for _, name := range []string{"schema_sessions.sql", "schema_governed_actions.sql"} {
		body, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(body)); e != nil {
			t.Fatal(e)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('parent','alice'),('child','alice'),('foreign','mallory'),('race','alice'),('race-child','alice')`); err != nil {
		t.Fatal(err)
	}
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if err = os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"actions":{"max_calls":1,"max_work_units":1000}}`), 0600); err != nil {
		t.Fatal(err)
	}
	handler := GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	call := func(principal, sid string, req governedActionRequest) (ep.ActionDecision, uint32) {
		t.Helper()
		body, _ := json.Marshal(req)
		frame, _ := wire.EncodeFields(10, []string{principal, sid, string(body)})
		raw, status := handler(bus.ModuleInvocation{StageID: StageGuardrailState}, frame)
		if status != bus.ModuleStatusOK {
			t.Errorf("module status %v", status)
			return ep.ActionDecision{}, store.StatusInvalid
		}
		code, cells, ok := store.DecodeRequest(raw)
		if !ok {
			t.Error("invalid frame")
			return ep.ActionDecision{}, store.StatusInvalid
		}
		var d ep.ActionDecision
		if code == store.StatusOK && (len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &d) != nil) {
			t.Error("invalid decision")
		}
		return d, code
	}
	if d, status := call("alice", "child", governedActionRequest{Operation: "fork", ParentSession: "parent"}); status != store.StatusOK || !d.Allowed {
		t.Fatal("fork", status, d)
	}
	if _, status := call("mallory", "child", governedActionRequest{Operation: "root"}); status != store.StatusInvalid {
		t.Fatal("foreign identity")
	}
	if _, status := call("alice", "foreign", governedActionRequest{Operation: "fork", ParentSession: "parent"}); status != store.StatusInvalid {
		t.Fatal("foreign fork")
	}
	generation, err := ep.ActionPolicyGeneration()
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	intent := ep.ActionIntent{SchemaVersion: 1, ID: "action", Request: "request", Task: "task", Attempt: "attempt", Principal: "alice", Root: "session:parent", Tool: "write_file", Class: "file_write", Destination: "file:/workspace/result", PayloadDigest: actionTestHash([]byte(`{"path":"/workspace/result"}`)), Purpose: "requested edit", PolicyGeneration: generation, RevocationGeneration: "revocation:1", IdempotencyKey: "key", WorkUnits: "28", Expires: now.Add(time.Minute)}
	raw, _ := json.Marshal(intent)
	h := sha256.Sum256(raw)
	fresh := ep.ActionFreshness{IntentDigest: hex.EncodeToString(h[:]), PolicyGeneration: intent.PolicyGeneration, RevocationGeneration: intent.RevocationGeneration, Checked: now, Expires: now.Add(2 * time.Second), Authorized: true}
	req := governedActionRequest{Operation: "prepare", Intent: intent, RegistryClass: "file_write", Freshness: fresh, Arguments: json.RawMessage(`{"path":"/workspace/result"}`)}
	if d, status := call("alice", "parent", req); status != store.StatusOK || !d.Allowed {
		t.Fatal("prepare", status, d)
	}
	req.Operation = "admit"
	if d, status := call("alice", "child", req); status != store.StatusOK || !d.Allowed {
		t.Fatal("admit", status, d)
	}
	req.Operation = "dispatch"
	var dispatched atomic.Int64
	var wg sync.WaitGroup
	for n := 0; n < 24; n++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			sid := "parent"
			if n%2 == 0 {
				sid = "child"
			}
			d, status := call("alice", sid, req)
			if status != store.StatusOK {
				t.Error("dispatch status", status)
			}
			if d.DispatchAllowed {
				dispatched.Add(1)
			}
		}(n)
	}
	wg.Wait()
	if dispatched.Load() != 1 {
		t.Fatal("duplicate dispatch count", dispatched.Load())
	}
	// Recreate the module handler and remove disposable session projections:
	// the same durable dispatch remains unknown and cannot be replayed.
	handler = GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	if d, status := call("alice", "child", req); status != store.StatusOK || d.DispatchAllowed || d.Reason != "outcome_unknown" {
		t.Fatal("restart replay", status, d)
	}
	req.Arguments = json.RawMessage(`{"path":"/workspace/another"}`)
	req.Intent.PayloadDigest = actionTestHash(req.Arguments)
	req.Intent.WorkUnits = "29"
	req.Intent.Destination = "file:/workspace/another"
	req.Intent.ID = "second"
	req.Intent.IdempotencyKey = "second"
	req.Operation = "prepare"
	if d, status := call("alice", "child", req); status != store.StatusOK || !d.Allowed {
		t.Fatal("second prepare", status, d)
	}
	raw, _ = json.Marshal(req.Intent)
	h = sha256.Sum256(raw)
	req.Freshness.IntentDigest = hex.EncodeToString(h[:])
	req.Operation = "admit"
	if d, status := call("alice", "child", req); status != store.StatusOK || d.Allowed || d.Reason != "task_resource_ceiling" {
		t.Fatal("fork reset budget", status, d)
	}
	if d, status := call("alice", "race-child", governedActionRequest{Operation: "fork", ParentSession: "race"}); status != store.StatusOK || !d.Allowed {
		t.Fatal("race fork", status, d)
	}
	requests := make([]governedActionRequest, 24)
	for n := range requests {
		r := req
		r.Operation = "prepare"
		r.Intent = intent
		r.Intent.ID = fmt.Sprintf("race-%d", n)
		r.Intent.IdempotencyKey = r.Intent.ID
		r.Intent.Root = "session:race"
		path := fmt.Sprintf("/workspace/race-%d", n)
		r.Arguments, _ = json.Marshal(map[string]string{"path": path})
		r.Intent.Destination = "file:" + path
		r.Intent.PayloadDigest = actionTestHash(r.Arguments)
		r.Intent.WorkUnits = strconv.Itoa(len(r.Arguments))
		if d, status := call("alice", "race", r); status != store.StatusOK || !d.Allowed {
			t.Fatal("race prepare", status, d)
		}
		r.Operation = "admit"
		requests[n] = r
	}
	var reserved atomic.Int64
	for n, r := range requests {
		wg.Add(1)
		go func(n int, r governedActionRequest) {
			defer wg.Done()
			sid := "race"
			if n%2 == 0 {
				sid = "race-child"
			}
			r.Freshness.Checked = time.Now().UTC()
			r.Freshness.Expires = r.Freshness.Checked.Add(time.Second)
			d, status := call("alice", sid, r)
			if status != store.StatusOK {
				t.Error("race reservation", status)
			}
			if d.Allowed {
				reserved.Add(1)
			}
		}(n, r)
	}
	wg.Wait()
	if reserved.Load() != 1 {
		t.Fatal("parallel fork budget bypass", reserved.Load())
	}

	// A broken reservation cannot silently recover as unspent allowance.
	if _, err = pool.Exec(ctx, `UPDATE governed_action_roots SET journal=jsonb_set(journal::jsonb,'{reserved_calls}','0')::text`); err != nil {
		t.Fatal(err)
	}
	if _, status := call("alice", "parent", req); status != store.StatusInvalid {
		t.Fatal("corrupt journal accepted")
	}
}

func actionTestHash(raw []byte) string { h := sha256.Sum256(raw); return hex.EncodeToString(h[:]) }
