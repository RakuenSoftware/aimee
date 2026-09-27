package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestTaskProjectionAtomicOwnerPostgres(t *testing.T) {
	root := livePool(t)
	ctx := context.Background()
	schema := fmt.Sprintf("mr13_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_guardrail.sql", "schema_conversation.sql", "schema_task_projection.sql"} {
		body, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(body)); e != nil {
			t.Fatal(name, e)
		}
	}
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, e := pool.Exec(ctx, sql, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`INSERT INTO server_sessions(id,principal) VALUES('session','alice'),('foreign','bob');INSERT INTO session_state(session_id,active_task_id) VALUES('session',1),('foreign',1)`)
	handler := GuardrailState.Handler(explorationLiveDB{liveQueryer{pool}})
	call := func(principal string, ref uint32, req map[string]any) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(req)
		frame, _ := wire.EncodeFields(9, []string{principal, "session", string(raw)})
		reply, status := handler(bus.ModuleInvocation{StageID: StageGuardrailState, PrincipalRef: ref}, frame)
		if status != bus.ModuleStatusOK {
			return map[string]any{"status": "denied"}
		}
		code, fields, e := wire.DecodeFields(reply)
		if e != nil {
			t.Fatal(e)
		}
		if code != store.StatusOK || len(fields) != 1 {
			return map[string]any{"status": "denied"}
		}
		var out map[string]any
		if e = json.Unmarshal([]byte(fields[0]), &out); e != nil {
			t.Fatal(e)
		}
		return out
	}
	prepared := func(identity string) map[string]any {
		return map[string]any{"status": "ok", "store": "kb", "view": "briefing", "rendered_context": "evidence", "cache": map[string]any{"identity": "sha256:" + identity, "owner_revalidated": true}, "receipt": map[string]any{"payload_sha256": taskDigest([]byte("evidence")), "sources": []any{map[string]any{"channel": "native_active_context", "stable_id": "42"}}}}
	}
	request := func(op, expected string) map[string]any {
		return map[string]any{"operation": op, "task_id": "1", "expected_revision": expected, "binding": map[string]any{"store": "kb", "project": "project-a", "task": "review deployment", "view": "briefing"}, "ttl_seconds": 60, "items": []any{map[string]any{"kind": "hypothesis", "text": "may require a restart"}, map[string]any{"kind": "planned_action", "text": "edit config"}}, "prepared": prepared("first")}
	}
	if r := call("bob", 0, request("rebuild", "0")); r["status"] != "denied" {
		t.Fatal("foreign principal", r)
	}
	if r := call("alice", 123, request("rebuild", "0")); r["status"] != "denied" {
		t.Fatal("plugin minted state", r)
	}
	bad := request("rebuild", "0")
	bad["items"] = []any{map[string]any{"kind": "execution_observation", "text": "edit succeeded"}}
	if r := call("alice", 0, bad); r["status"] != "denied" {
		t.Fatal("planned text became observation", r)
	}
	created := call("alice", 0, request("rebuild", "0"))
	if created["status"] != "ok" {
		t.Fatal(created)
	}
	if _, e := pool.Exec(ctx, `UPDATE session_state SET task_projection_state=jsonb_set(task_projection_state::jsonb,'{class}','"authoritative"')::text WHERE session_id='session'`); e == nil {
		t.Fatal("SQL relabeled a projection as authoritative")
	}
	projection := created["projection"].(map[string]any)
	if projection["revision"] != "1" || projection["class"] != "derived_non_authoritative" {
		t.Fatal(projection)
	}
	if r := call("alice", 0, request("rebuild", "0")); r["status"] != "conflict" {
		t.Fatal("lost update", r)
	}
	var accepted atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if r := call("alice", 0, request("rebuild", "1")); r["status"] == "ok" {
				accepted.Add(1)
			}
		}()
	}
	wg.Wait()
	if accepted.Load() != 1 {
		t.Fatal("parallel revisions", accepted.Load())
	}
	zero := request("get", "")
	zero["max_context_bytes"] = 0
	if r := call("alice", 0, zero); r["rendered_context"] != "" {
		t.Fatal("zero budget", r)
	}
	changed := request("get", "")
	changed["prepared"] = prepared("changed")
	if r := call("alice", 0, changed); r["status"] != "unavailable" || r["rendered_context"] != nil {
		t.Fatal("stale cache released", r)
	}
	if r := call("alice", 0, request("get", "")); r["status"] != "unavailable" {
		t.Fatal("old evidence resurrected stale state", r)
	}
	if r := call("alice", 0, request("rebuild", "2")); r["status"] != "ok" {
		t.Fatal(r)
	}
	removed := request("get", "")
	revoked := prepared("revoked")
	revoked["receipt"].(map[string]any)["sources"] = []any{}
	removed["prepared"] = revoked
	if r := call("alice", 0, removed); r["reason"] != "projection_blocked" {
		t.Fatal("lost input did not block release", r)
	}
	exec(`INSERT INTO conv_tool_events(id,session_id,tool_name,tool_result) VALUES(1,'session','edit_file','permission denied'),(2,'foreign','read_file','secret')`)
	event := request("rebuild", "3")
	event["events"] = []string{"2"}
	if r := call("alice", 0, event); r["status"] != "unavailable" {
		t.Fatal("cross-session event", r)
	}
	event["events"] = []string{"1"}
	if r := call("alice", 0, event); r["status"] != "ok" {
		t.Fatal(r)
	}
	exec(`UPDATE conv_tool_events SET tool_result='changed' WHERE id=1`)
	if r := call("alice", 0, request("get", "")); r["status"] != "unavailable" {
		t.Fatal("changed event released", r)
	}
	exec(`UPDATE session_state SET active_task_id=2 WHERE session_id='session'`)
	if r := call("alice", 0, request("get", "")); r["reason"] != "active_task_mismatch" {
		t.Fatal("task switch leaked old state", r)
	}
	switched := request("rebuild", "0")
	switched["task_id"] = "2"
	switched["ttl_seconds"] = 1
	fresh := call("alice", 0, switched)
	if fresh["status"] != "ok" || fresh["projection"].(map[string]any)["projection_id"] == projection["projection_id"] {
		t.Fatal("task identity reused", fresh)
	}
	exec(`UPDATE session_state SET task_projection_state=jsonb_set(task_projection_state::jsonb,'{expires_at}',to_jsonb('2000-01-01T00:00:00Z'::text))::text WHERE session_id='session'`)
	switched["operation"] = "get"
	if r := call("alice", 0, switched); r["status"] != "unavailable" {
		t.Fatal("expired state released", r)
	}
	var retained string
	if e = pool.QueryRow(ctx, `SELECT task_projection_state FROM session_state WHERE session_id='session'`).Scan(&retained); e != nil {
		t.Fatal(e)
	}
	var state taskProjectionState
	if e = json.Unmarshal([]byte(retained), &state); e != nil || state.Items != nil || len(state.Prepared) > 0 || state.Replay != "digest_only_projection_not_retained" {
		t.Fatal("expiry retained text", state, e)
	}
	switched["operation"] = "discard"
	switched["expected_revision"] = "1"
	if r := call("alice", 0, switched); r["status"] != "ok" {
		t.Fatal(r)
	}
	var events int
	if e = pool.QueryRow(ctx, `SELECT count(*) FROM conv_tool_events`).Scan(&events); e != nil || events != 2 {
		t.Fatal("discard erased execution receipts", events, e)
	}
}

func TestTaskProjectionPromotionProof(t *testing.T) {
	state := taskProjectionState{ID: "0123456789abcdef0123456789abcdef", Revision: "3", Principal: "alice", Session: "session", TaskID: "1", Binding: taskProjectionBinding{Store: "user", Project: "p", Task: "review", View: "briefing"}, ExpiresAt: time.Now().Add(time.Hour), Items: []taskProjectionItem{{Kind: "hypothesis", Text: "condition may apply"}, {Kind: "planned_action", Text: "edit the file"}}}
	prepared := taskPreparedView{}
	prepared.Receipt.Sources = json.RawMessage(`[{"channel":"native_active_context","stable_id":"42","source_version":{"record_kind":"user_memory_record","version":{"schema_version":1,"owner_id":"00000000-0000-0000-0000-000000000001","record_id":"42","record_revision":"2"}}}]`)
	index := 0
	request := taskProjectionRequest{Operation: "promotion_preview", ClaimIndex: &index, TargetID: "42"}
	saves := 0
	save := func() error { saves++; return nil }
	invoke := func() map[string]any {
		t.Helper()
		code, cells, e := taskProjectionPromotion(&state, request, prepared, save)
		if e != nil || code != store.StatusOK || len(cells) != 1 {
			t.Fatal(code, e)
		}
		var result map[string]any
		if e = json.Unmarshal([]byte(cells[0]), &result); e != nil {
			t.Fatal(e)
		}
		return result
	}
	preview := invoke()
	if preview["status"] != "ok" || saves != 0 {
		t.Fatal(preview)
	}
	request.Operation = "promote"
	request.PreviewDigest = "stale"
	if got := invoke(); got["status"] != "conflict" || saves != 0 {
		t.Fatal(got)
	}
	request.PreviewDigest = preview["preview_digest"].(string)
	if got := invoke(); got["status"] != "ok" || saves != 1 || len(state.PendingPromotion) == 0 {
		t.Fatal(got)
	}
	if got := invoke(); got["status"] != "ok" || saves != 2 {
		t.Fatal("identical admission retry", got)
	}
	state.Revision = "4"
	if got := invoke(); got["status"] != "conflict" {
		t.Fatal("stale preview admitted new revision", got)
	}
	request.Operation = "promotion_preview"
	index = 1
	if got := invoke(); got["status"] != "error" {
		t.Fatal("plan promoted as claim", got)
	}
}

func TestTaskProjectionAbandonedAdmissionRetainsDigestOnly(t *testing.T) {
	state := taskProjectionState{PendingPromotion: json.RawMessage(`{"preview_digest":"sha256:proof","content":"temporary hypothesis"}`)}
	state.releasePending()
	if len(state.PendingPromotion) != 0 || string(state.LastPromotion) != `{"preview_digest":"sha256:proof","state":"submission_unknown_reconciliation_required"}` {
		t.Fatal(state.LastPromotion)
	}
	state.releasePending()
	if len(state.LastPromotion) == 0 {
		t.Fatal("lost reconciliation receipt")
	}
}
