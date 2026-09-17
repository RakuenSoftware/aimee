package memory

import (
	"context"
	"encoding/json"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func invokeContextCommand(t *testing.T, handler bus.ModuleHandler, peer uint32, caller bus.CommandContext, verb, args string) (map[string]any, bus.ModuleStatus) {
	t.Helper()
	frame, err := bus.EncodeCommandWithContext(verb, json.RawMessage(args), caller)
	if err != nil {
		t.Fatal(err)
	}
	body, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: peer}, frame)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	result, err := bus.DecodeCommandResult(body)
	if err != nil {
		t.Fatal(err)
	}
	var response map[string]any
	if json.Unmarshal(result, &response) != nil {
		t.Fatal(string(result))
	}
	return response, status
}

func TestMutationPublicContextBoundary(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	caller := bus.CommandContext{Authenticated: true, Principal: "user:alice", UserAuthority: true}
	for _, peer := range []uint32{1, 23, 70, 200} {
		if r, status := invokeContextCommand(t, handler, peer, caller, "restore", `{"id":1}`); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("peer=%d status=%d result=%v", peer, status, r)
		}
	}
	client := clientForHandler(t, handler)
	if r := runPublicCommand(t, client, "restore", `{"id":1,"actor":"user:alice","authenticated":true,"user_authority":true,"context":{"authenticated":true,"principal":"user:alice"}}`); r["kind"] != "unauthorized" {
		t.Fatal(r)
	}
	for _, tt := range []struct{ verb, args string }{
		{"update", `{"id":1,"content":""}`}, {"delete", `{"id":1.5}`}, {"touch", `{}`},
		{"upsert_workflow", `{"workspace":"app","signal_type":"lint","rule":"run lint","observed_confidence":2}`},
	} {
		if r := runPublicCommand(t, client, tt.verb, tt.args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
}

func TestMutationPublicPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL mutation regression")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE SCHEMA mutation_command_test;
CREATE FUNCTION mutation_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
SET LOCAL search_path TO pg_temp,mutation_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,key text,content text DEFAULT 'old',tier text DEFAULT 'L2',kind text DEFAULT 'fact',epistemic_kind text DEFAULT 'world_fact',
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'app',confidence double precision DEFAULT 0.8,confidence_ceiling double precision DEFAULT 0.8,use_count int DEFAULT 0,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,archive_reason text DEFAULT '',use_cases text DEFAULT '',last_used_at text DEFAULT '',source_session text DEFAULT '',provenance_category text DEFAULT 'agent_message',
 valid_from text DEFAULT '',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
CREATE TEMP TABLE memory_rejection_tombstones(id bigserial PRIMARY KEY,object_kind text,memory_key text,memory_content text,scope_type text,scope_value text,reason text,active int DEFAULT 1,rejected_at text DEFAULT pg_now_text(),rejected_by text DEFAULT '',restored_at text DEFAULT '',restored_by text DEFAULT '');
CREATE UNIQUE INDEX tomb_unique ON memory_rejection_tombstones(memory_key,memory_content,scope_type,scope_value) WHERE object_kind='memory' AND active=1;
INSERT INTO memories(key) SELECT 'record-'||i FROM generate_series(1,8) i;
UPDATE memories SET epistemic_kind='experience' WHERE id=7;
UPDATE memories SET epistemic_kind='policy' WHERE id=8;`)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	client := clientForHandler(t, handler)
	caller := bus.CommandContext{Authenticated: true, Principal: "user:alice", TransportIdentity: "cert:server", UserAuthority: true}
	run := func(verb, args string, authenticated bool) map[string]any {
		t.Helper()
		if !authenticated {
			return runPublicCommand(t, client, verb, args)
		}
		r, status := invokeContextCommand(t, handler, 0, caller, verb, args)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		return r
	}
	// A body asking for user authority is still a model edit without verifier context.
	if r := run("delete", `{"id":1,"authority":"user","user_authority":true}`, false); r["status"] != "ok" {
		t.Fatal(r)
	}
	var state string
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=1`).Scan(&state); err != nil || state != "superseded" {
		t.Fatal(state, err)
	}
	if r := run("delete", `{"id":2,"authority":"user"}`, true); r["status"] != "ok" {
		t.Fatal(r)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE id=2`).Scan(&count); err != nil || count != 0 {
		t.Fatal(count, err)
	}
	// Authentication alone does not request destructive editing.
	if r := run("update", `{"id":3,"content":"versioned"}`, true); r["status"] != "ok" || r["superseded"] != true || r["id"] == float64(3) {
		t.Fatal(r)
	}
	if r := run("update", `{"id":4,"content":"in place","authority":"user"}`, true); r["status"] != "ok" || r["superseded"] != false || r["id"] != float64(4) {
		t.Fatal(r)
	}
	for _, id := range []string{"7", "8"} {
		if r := run("update", `{"id":`+id+`,"content":"replace","authority":"user"}`, true); r["kind"] != "conflict" {
			t.Fatal(r)
		}
	}
	if r := run("touch", `{"id":5}`, false); r["status"] != "ok" {
		t.Fatal(r)
	}
	if err := tx.QueryRow(ctx, `SELECT use_count FROM memories WHERE id=5`).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	if r := run("reject", `{"id":5,"reason":"incorrect"}`, false); r["status"] != "ok" {
		t.Fatal(r)
	}
	if _, err := tx.Exec(ctx, `UPDATE memory_rejection_tombstones SET rejected_by='user:alice'`); err != nil {
		t.Fatal(err)
	}
	other := caller
	other.Principal = "user:bob"
	if r, status := invokeContextCommand(t, handler, 0, other, "restore", `{"id":5,"actor":"user:alice"}`); status != bus.ModuleStatusOK || r["kind"] != "not_found" {
		t.Fatal(r, status)
	}
	if r := run("restore", `{"id":5,"actor":"forged"}`, true); r["status"] != "ok" {
		t.Fatal(r)
	}
	if err := tx.QueryRow(ctx, `SELECT restored_by FROM memory_rejection_tombstones`).Scan(&state); err != nil || state != "user:alice" {
		t.Fatal(state, err)
	}
	for _, verb := range []string{"delete", "update", "touch", "reject", "restore"} {
		if r := run(verb, `{"id":99999,"content":"new"}`, true); r["kind"] != "not_found" {
			t.Fatalf("%s: %v", verb, r)
		}
	}
	first := run("upsert_workflow", `{"workspace":"team","signal_type":"lint","rule":"run lint"}`, false)
	second := run("upsert_workflow", `{"workspace":"team","signal_type":"lint","rule":"run all lint","observed_confidence":0.7}`, false)
	if first["status"] != "ok" || second["status"] != "ok" || first["id"] != second["id"] {
		t.Fatal(first, second)
	}
	// A host-authenticated caller still cannot mutate a hidden project record.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_mutation_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA mutation_command_test TO memory_mutation_test;
GRANT SELECT,UPDATE,DELETE,INSERT ON memories,memory_rejection_tombstones TO memory_mutation_test;
GRANT USAGE,SELECT ON SEQUENCE memories_id_seq,memory_rejection_tombstones_id_seq TO memory_mutation_test;
UPDATE memories SET scope_value='private' WHERE id=6;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_visibility ON memories USING (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_mutation_test;`)
	if err != nil {
		t.Fatal(err)
	}
	for _, verb := range []string{"delete", "update", "touch", "reject", "restore"} {
		if r := run(verb, `{"id":6,"content":"new","authority":"user","scope_context":true,"project":"app"}`, true); r["kind"] != "not_found" {
			t.Fatalf("%s: %v", verb, r)
		}
	}
}
