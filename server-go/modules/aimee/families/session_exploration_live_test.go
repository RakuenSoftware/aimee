package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type explorationLiveDB struct{ liveQueryer }

func (d explorationLiveDB) Begin(ctx context.Context) (store.Tx, error) {
	tx, e := d.pool.Begin(ctx)
	if e != nil {
		return nil, e
	}
	return explorationLiveTx{tx}, nil
}

type explorationLiveTx struct{ pgx.Tx }

func (q explorationLiveTx) Exec(ctx context.Context, s string, a ...any) (store.Tag, error) {
	tag, e := q.Tx.Exec(ctx, s, a...)
	return store.RowsAffected(tag.RowsAffected()), e
}
func (q explorationLiveTx) Query(ctx context.Context, s string, a ...any) (store.Rows, error) {
	r, e := q.Tx.Query(ctx, s, a...)
	if e != nil {
		return nil, e
	}
	return liveRows{r}, nil
}
func (q explorationLiveTx) QueryRow(ctx context.Context, s string, a ...any) store.Row {
	return q.Tx.QueryRow(ctx, s, a...)
}

func TestSessionExplorationLiveAtomicOwner(t *testing.T) {
	root := livePool(t)
	ctx := context.Background()
	schema := fmt.Sprintf("mr07_%d", time.Now().UnixNano())
	if _, e := root.Exec(ctx, "CREATE SCHEMA "+schema); e != nil {
		t.Fatal(e)
	}
	defer root.Exec(ctx, "DROP SCHEMA "+schema+" CASCADE")
	config, e := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if e != nil {
		t.Fatal(e)
	}
	config.ConnConfig.RuntimeParams["search_path"] = schema
	pool, e := pgxpool.NewWithConfig(ctx, config)
	if e != nil {
		t.Fatal(e)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_guardrail.sql", "schema_session_exploration.sql"} {
		if name == "schema_session_exploration.sql" {
			// Populate the pre-migration shape before applying the additive change.
			if _, e = pool.Exec(ctx, `INSERT INTO session_state(session_id,hook_call_count) VALUES('session',77)`); e != nil {
				t.Fatal(e)
			}
		}
		body, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(body)); e != nil {
			t.Fatal(name, e)
		}
	}
	var legacyCount int
	var defaultState string
	if e = pool.QueryRow(ctx, `SELECT hook_call_count,exploration_state FROM session_state WHERE session_id='session'`).Scan(&legacyCount, &defaultState); e != nil || legacyCount != 77 || defaultState != "" {
		t.Fatal("migration changed legacy state", e)
	}
	if _, e = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('session','alice')`); e != nil {
		t.Fatal(e)
	}
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if e = os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"exploration":{"raw_scans":3}}`), 0600); e != nil {
		t.Fatal(e)
	}
	handler := GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	call := func(principal string, req map[string]any) ([]byte, bus.ModuleStatus) {
		body, _ := json.Marshal(req)
		frame, _ := wire.EncodeFields(8, []string{principal, "session", string(body)})
		return handler(bus.ModuleInvocation{StageID: StageGuardrailState}, frame)
	}
	now := time.Now().UTC()
	bindings := []map[string]any{}
	for i := 0; i < 2; i++ {
		b := map[string]any{"principal": "alice", "session": "session", "task": []string{"session-task", "job:1"}[i], "project": "project", "worktree_generation": "w1", "index_generation": "i1", "memory_owner": "m1"}
		bindings = append(bindings, b)
		c := map[string]any{"id": fmt.Sprint(i), "revision": 1, "binding": b, "plan_digest": "plan", "source_versions_digest": "versions", "query_class": "symbol", "coverage_complete": true, "confidence_provenance": "uncalibrated", "supported_classes": []string{"raw_scan"}, "created": now.Add(-time.Second), "expires": now.Add(time.Hour), "limits": map[string]any{"enabled": true, "raw_scans": 0}, "tier": "enforce"}
		reply, status := call("alice", map[string]any{"operation": "issue", "binding": b, "contract": c})
		if status != bus.ModuleStatusOK || len(reply) < 8 || storeStatus(reply) != store.StatusOK {
			t.Fatalf("issue %v %v", status, reply)
		}
	}
	var wg sync.WaitGroup
	var admitted atomic.Int64
	for i := 0; i < 24; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			b := bindings[i%2]
			req := map[string]any{"operation": "check", "binding": b, "attempt_id": fmt.Sprint(i), "tool": "bash", "side_effect": "filesystem", "tool_arguments": map[string]any{"command": "rg symbol"}, "bytes": 0, "tokens": 0}
			if i%2 == 0 {
				req["operation"] = "check_session"
				delete(req, "binding")
			}
			reply, status := call("alice", req)
			if status != bus.ModuleStatusOK || storeStatus(reply) != store.StatusOK {
				t.Errorf("reserve failed %v", status)
				return
			}
			_, cells, ok := store.DecodeRequest(reply)
			if !ok || len(cells) != 1 {
				t.Error("invalid reply")
				return
			}
			var d map[string]any
			json.Unmarshal([]byte(cells[0]), &d)
			if d["allowed"] == true {
				admitted.Add(1)
			}
		}(i)
	}
	wg.Wait()
	if admitted.Load() != 3 {
		t.Fatalf("cross-task allowance overspent: %d", admitted.Load())
	}
	// An old client's unchanged save frame cannot erase newly committed counters.
	var beforeSave, afterSave string
	if e = pool.QueryRow(ctx, `SELECT exploration_state FROM session_state WHERE session_id='session'`).Scan(&beforeSave); e != nil {
		t.Fatal(e)
	}
	legacyFrame, _ := wire.EncodeFields(opSessionStateSave, saveFrame("session"))
	legacyReply, legacyStatus := handler(bus.ModuleInvocation{StageID: StageGuardrailState}, legacyFrame)
	if legacyStatus != bus.ModuleStatusOK || storeStatus(legacyReply) != store.StatusOK {
		t.Fatal("legacy save failed")
	}
	if e = pool.QueryRow(ctx, `SELECT exploration_state FROM session_state WHERE session_id='session'`).Scan(&afterSave); e != nil || beforeSave != afterSave {
		t.Fatal("legacy client overwrote exploration state", e)
	}
	// Recreate the handler, proving accounting lives in PostgreSQL, not its heap.
	handler = GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	req := map[string]any{"operation": "inspect", "binding": bindings[0]}
	restored, restoredStatus := call("alice", req)
	if restoredStatus != bus.ModuleStatusOK || storeStatus(restored) != store.StatusOK {
		t.Fatal("restored inspection unavailable")
	}
	_, cells, ok := store.DecodeRequest(restored)
	if !ok || len(cells) != 1 {
		t.Fatal("invalid restored reply")
	}
	var snapshot map[string]any
	json.Unmarshal([]byte(cells[0]), &snapshot)
	if snapshot["session_usage"].(map[string]any)["raw_scans"] != float64(3) {
		t.Fatal("lost committed session allowance")
	}
	for i := 0; i < 24; i++ {
		b := bindings[i%2]
		retry := map[string]any{"operation": "check", "binding": b, "attempt_id": fmt.Sprint(i), "tool": "bash", "side_effect": "filesystem", "tool_arguments": map[string]any{"command": "rg symbol"}}
		if i%2 == 0 {
			retry["operation"] = "check_session"
			delete(retry, "binding")
		}
		r, st := call("alice", retry)
		if st != bus.ModuleStatusOK || storeStatus(r) != store.StatusOK {
			t.Fatal("restart retry failed")
		}
	}
	r, st := call("alice", req)
	if st != bus.ModuleStatusOK {
		t.Fatal(st)
	}
	_, cells, ok = store.DecodeRequest(r)
	if !ok {
		t.Fatal("bad inspection")
	}
	json.Unmarshal([]byte(cells[0]), &snapshot)
	if snapshot["session_usage"].(map[string]any)["raw_scans"] != float64(3) {
		t.Fatal("retries double charged")
	}

	reply, status := call("mallory", req)
	if status != bus.ModuleStatusOK || storeStatus(reply) == store.StatusOK {
		t.Fatal("foreign session accepted")
	}
	raw, _ := json.Marshal(req)
	frame, _ := wire.EncodeFields(8, []string{"alice", "session", string(raw)})
	if _, status = handler(bus.ModuleInvocation{StageID: StageGuardrailState, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("non-host caller accepted")
	}
}
func storeStatus(b []byte) uint32 {
	if len(b) < 4 {
		return 99
	}
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}
