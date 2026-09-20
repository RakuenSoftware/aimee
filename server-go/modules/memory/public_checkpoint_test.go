package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestCheckpointValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"action":"delete"}`, `{"action":"restore","checkpoint_id":0,"snapshot":"x"}`, `{"action":"restore","checkpoint_id":1.5,"snapshot":"x"}`, `{"action":"restore","checkpoint_id":"9223372036854775808","snapshot":"x"}`, `{"action":"restore","checkpoint_id":1,"snapshot":null}`} {
		if got := runPublicCommand(t, client, "checkpoint", args); got["kind"] != "invalid_argument" {
			t.Fatal(args, got)
		}
	}
	if got := runPublicCommand(t, client, "checkpoint", `{"action":"facts"}`); got["kind"] != "unavailable" {
		t.Fatal(got)
	}
}

func exerciseCheckpointReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	content := strings.Repeat("🦊", 800)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES
 ('L2','fact','checkpoint-long',$1,'project','checkpoint-private'),
 ('L0','scratch','checkpoint-scratch','not a fact','project','checkpoint-private'),
 ('L2','procedure','checkpoint-procedure','not a fact','project','checkpoint-private'),
 ('L2','fact','checkpoint-other','private','project','checkpoint-other')`, content)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state) VALUES
 ('L2','fact','checkpoint-archived','archived','project','checkpoint-private','archived')`)
	run := func(args map[string]any) map[string]any {
		t.Helper()
		args["scope_context"], args["project"] = true, "checkpoint-private"
		body, _ := json.Marshal(args)
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{Authenticated: true, Principal: "user:checkpoint", UserAuthority: true}, "checkpoint", string(body))
		if status != bus.ModuleStatusOK {
			t.Fatal(result, status)
		}
		return result
	}
	// Port the retired native list regressions: prospective rows are never
	// memory rows, and archived memory stays out of the public list.
	exec(`INSERT INTO prospective_memories(trigger_text,action_text) VALUES('checkpoint-reminder','checkpoint-reminder')`)
	listed := runPublicCommand(t, clientForHandler(t, handler), "list", `{"scope_context":true,"project":"checkpoint-private","limit":64}`)
	if listed["status"] != "ok" {
		t.Fatal(listed)
	}
	for _, row := range listed["memories"].([]any) {
		memory := row.(map[string]any)
		if memory["key"] == "checkpoint-archived" || memory["content"] == "checkpoint-reminder" {
			t.Fatal("non-memory or archived row escaped list", memory)
		}
	}
	got := run(map[string]any{"action": "facts", "tier": "L0", "kind": "scratch", "limit": 1})
	if got["status"] != "ok" {
		t.Fatal(got)
	}
	found := false
	for _, row := range got["facts"].([]any) {
		fact := row.(map[string]any)
		if len(fact) != 2 {
			t.Fatal("snapshot shape changed", fact)
		}
		switch fact["key"] {
		case "checkpoint-long":
			found = true
			if fact["content"] != content {
				t.Fatal("truncated fact")
			}
		case "checkpoint-other", "checkpoint-archived", "checkpoint-scratch", "checkpoint-procedure":
			t.Fatal("ineligible fact", fact)
		}
	}
	if !found {
		t.Fatal("missing scoped fact", got)
	}
	// Fill more than a checkpoint can select; caller input cannot raise its cap.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) SELECT 'L2','fact','checkpoint-many-'||i,'fact '||i,'project','checkpoint-private' FROM generate_series(1,20) i`)
	got = run(map[string]any{"action": "facts", "limit": 200})
	if rows, ok := got["facts"].([]any); !ok || len(rows) != 16 {
		t.Fatal(got)
	}
	snapshot := `{"tasks":[],"facts":[{"key":"restore","content":"checkpoint source"}],"decisions":[]}`
	args := map[string]any{"action": "restore", "checkpoint_id": "9223372036854775807", "snapshot": snapshot, "session_id": "checkpoint-session", "authority": "user", "tier": "L4", "kind": "policy", "key": "forged"}
	got = run(args)
	if got["status"] != "ok" {
		t.Fatal(got)
	}
	id, err := strconv.ParseInt(got["memory_id"].(string), 10, 64)
	if err != nil || id <= 0 {
		t.Fatal(got, err)
	}
	var tier, kind, key, stored, scope, session, role string
	var confidence float64
	err = tx.QueryRow(ctx, `SELECT m.tier,m.kind,m.key,m.content,m.scope_value,m.source_session,m.confidence,a.actor_role FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id WHERE m.id=$1`, id).Scan(&tier, &kind, &key, &stored, &scope, &session, &confidence, &role)
	if err != nil || tier != "L0" || kind != "scratch" || key != "checkpoint_restore:9223372036854775807" || stored != snapshot || scope != "checkpoint-private" || session != "checkpoint-session" || confidence != .8 || role != "model" {
		t.Fatal(tier, kind, key, stored, scope, session, confidence, role, err)
	}
	if again := run(args); again["memory_id"] != got["memory_id"] {
		t.Fatal("replay duplicated checkpoint", again, got)
	}
	// Failure to record extraction provenance rolls back the scratch write too.
	exec(`RESET ROLE; ALTER TABLE memory_fact_actors ADD CONSTRAINT checkpoint_failure CHECK(actor_role<>'model') NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
	args["checkpoint_id"] = "9223372036854775806"
	if failed := run(args); failed["kind"] != "unavailable" {
		t.Fatal(failed)
	}
	exec(`RESET ROLE; ALTER TABLE memory_fact_actors DROP CONSTRAINT checkpoint_failure; SET LOCAL ROLE aimee_store_runtime`)
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='checkpoint_restore:9223372036854775806'`).Scan(&count); err != nil || count != 0 {
		t.Fatal("partial restore", count, err)
	}
}
