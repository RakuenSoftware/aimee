package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestPersonalReviewOwnerPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL review regression")
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
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE user_memories(id bigint PRIMARY KEY,tier text,kind text,key text,content text,confidence double precision,lifecycle_state text,created_at timestamptz,updated_at timestamptz);
 INSERT INTO user_memories SELECT 9007199254740993+g,'L2','fact','private-review',repeat('長い記憶',2000),.8,
 CASE WHEN g=70 THEN 'archived' ELSE 'active' END,now(),now() FROM generate_series(1,70) g`)
	if err != nil {
		t.Fatal(err)
	}
	data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementServer, data))
	call := func(args string) []byte {
		t.Helper()
		frame, err := bus.EncodeCommand("runtime", []byte(args))
		if err != nil {
			t.Fatal(err)
		}
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct {
			JSON string `json:"json"`
		}
		if err := json.Unmarshal(body, &envelope); err != nil || envelope.JSON == "" {
			t.Fatal(string(body), err)
		}
		return []byte(envelope.JSON)
	}
	body := call(`{"operation":"user-review-list","project":"forged","scope":{"type":"project","value":"other"}}`)
	var response struct {
		Status   string `json:"status"`
		Store    string `json:"store"`
		Memories []struct {
			ReviewRecord
			Lifecycle string `json:"lifecycle"`
		} `json:"memories"`
	}
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatal(err)
	}
	if response.Status != "ok" || response.Store != "user" || len(response.Memories) != 64 {
		t.Fatal(string(body))
	}
	first := response.Memories[0]
	if first.ID != 9007199254741063 || first.Lifecycle != "archived" || first.LifecycleState != "archived" || first.ScopeType != "user" || first.ScopeValue != "_user" || first.Content != strings.Repeat("長い記憶", 2000) {
		t.Fatal("review lost identity, scope, lifecycle or full content")
	}
	body = call(`{"operation":"user-review-list","state":"archived","limit":1}`)
	if err := json.Unmarshal(body, &response); err != nil || len(response.Memories) != 1 || response.Memories[0].ID != first.ID {
		t.Fatal(string(body), err)
	}
	body = call(`{"operation":"user-review-list","state":"nonexistent"}`)
	if err := json.Unmarshal(body, &response); err != nil || len(response.Memories) != 0 || !strings.Contains(string(body), `"memories":[]`) {
		t.Fatal(string(body), err)
	}
	body = call(`{"operation":"user-review-list","store":"kb"}`)
	if !strings.Contains(string(body), `"kind":"invalid_argument"`) {
		t.Fatal(string(body))
	}
	if _, err := tx.Exec(ctx, `DROP TABLE user_memories`); err != nil {
		t.Fatal(err)
	}
	body = call(`{"operation":"user-review-list"}`)
	if !strings.Contains(string(body), `"kind":"unavailable"`) {
		t.Fatal("missing store reported as empty review", string(body))
	}
}

func TestQueryPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct{ verb, args string }{
		{"key_exists", `{}`}, {"find_id_by_key_kind", `{"key":"name"}`},
		{"set_artifact", `{"memory_id":1,"artifact_type":"code"}`},
		{"set_artifact", `{"memory_id":1.5,"artifact_type":"code","artifact_ref":"file"}`},
		{"list_low_effectiveness", `{"threshold":2}`},
	} {
		r := runPublicCommand(t, client, test.verb, test.args)
		if r["status"] != "error" || r["kind"] != "invalid_argument" {
			t.Fatalf("%s: %v", test.verb, r)
		}
	}
}

func TestQueryPublicPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL command regression")
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA query_command_test;
CREATE FUNCTION query_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
SET LOCAL search_path TO pg_temp,query_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,key text,content text DEFAULT 'content',tier text DEFAULT 'L2',kind text DEFAULT 'fact',
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'app',confidence double precision DEFAULT 1,effectiveness double precision DEFAULT 0.2,
 use_count int DEFAULT 0,lifecycle_state text DEFAULT 'active',archive_reason text DEFAULT '',artifact_type text DEFAULT '',artifact_ref text DEFAULT '',artifact_hash text DEFAULT '',
 created_at text DEFAULT pg_now_text('-20 days'),updated_at text DEFAULT pg_now_text());
INSERT INTO memories(key) SELECT 'record-'||i FROM generate_series(1,300) i;
INSERT INTO memories(key) VALUES ('release#v1'),('release#v2'),('release#v3');
CREATE TEMP TABLE memory_rejection_tombstones(id bigserial PRIMARY KEY,object_kind text DEFAULT 'memory',memory_key text,memory_content text,scope_type text,scope_value text,reason text,active int DEFAULT 1);
INSERT INTO memories(key,lifecycle_state,scope_value) VALUES ('same-key','rejected','project-a'),('same-key','rejected','project-b');
INSERT INTO memory_rejection_tombstones(memory_key,memory_content,scope_type,scope_value,reason) VALUES
 ('same-key','content','project','project-a','reason-a'),('same-key','content','project','project-b','reason-b');`)
	if err != nil {
		t.Fatal(err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB})))
	run := func(verb, args string) map[string]any {
		t.Helper()
		r := runPublicCommand(t, client, verb, args)
		if r["status"] != "ok" {
			t.Fatalf("%s: %v", verb, r)
		}
		return r
	}
	if r := run("key_exists", `{"key":"record-1","operation":"delete","id":1}`); r["exists"] != true {
		t.Fatal(r)
	}
	if r := run("key_exists", `{"key":"missing"}`); r["exists"] != false {
		t.Fatal(r)
	}
	if r := run("find_id_by_key_kind", `{"key":"record-1","kind":"fact","scope_context":true}`); r["id"] != float64(1) || r["active_context_missing"] != true {
		t.Fatal(r)
	}
	if r := run("find_id_by_key_kind", `{"key":"record-1","kind":"episode"}`); r["id"] != float64(0) {
		t.Fatal(r)
	}
	for _, test := range []struct {
		verb, args   string
		want, fields int
	}{
		{"list_low_effectiveness", `{}`, 50, 6},
		{"list_low_effectiveness", `{"limit":256}`, 256, 6},
		{"list_unused_l2", `{}`, 64, 5},
		{"list_unused_l2", `{"max":999}`, 256, 5},
	} {
		rows := run(test.verb, test.args)["rows"].([]any)
		if len(rows) != test.want || len(rows[0].(map[string]any)) != test.fields {
			t.Fatalf("%s: rows=%d first=%v", test.verb, len(rows), rows[0])
		}
	}
	for _, test := range []struct{ verb, args string }{
		{"list_low_effectiveness", `{"threshold":0.1}`}, {"list_unused_l2", `{"days":30}`}, {"list_superseded_keys", `{"min_versions":4}`},
	} {
		if rows := run(test.verb, test.args)["rows"].([]any); len(rows) != 0 {
			t.Fatal(rows)
		}
	}
	superseded := run("list_superseded_keys", `{}`)["rows"].([]any)
	if len(superseded) != 1 || superseded[0].(map[string]any)["base_key"] != "release" || superseded[0].(map[string]any)["versions"] != float64(3) {
		t.Fatal(superseded)
	}
	reviewed := run("review_list", `{"state":"rejected"}`)["memories"].([]any)
	if len(reviewed) != 2 {
		t.Fatal(reviewed)
	}
	for _, r := range reviewed {
		row := r.(map[string]any)
		want := map[string]string{"project-a": "reason-a", "project-b": "reason-b"}[row["scope_value"].(string)]
		if row["review_reason"] != want || row["lifecycle"] != "rejected" || len(row) != 12 {
			t.Fatal(row)
		}
	}
	console := run("review_console", `{"limit":999,"state":"rejected","scope_context":true}`)
	if console["schema"] != "console.memories.v1" || console["count"] != float64(32) || len(console["memories"].([]any)) != 32 {
		t.Fatal(console)
	}
	run("set_artifact", `{"memory_id":1,"artifact_type":"file","artifact_ref":"source.go","artifact_hash":"abc","operation":"delete"}`)
	var kind, ref, hash string
	if err := tx.QueryRow(ctx, `SELECT artifact_type,artifact_ref,artifact_hash FROM memories WHERE id=1`).Scan(&kind, &ref, &hash); err != nil || kind != "file" || ref != "source.go" || hash != "abc" {
		t.Fatalf("artifact=%s/%s/%s %v", kind, ref, hash, err)
	}
	run("set_artifact", `{"memory_id":1,"artifact_type":"","artifact_ref":""}`)
	if err := tx.QueryRow(ctx, `SELECT artifact_hash FROM memories WHERE id=1`).Scan(&hash); err != nil || hash != "" {
		t.Fatalf("artifact not cleared: %q %v", hash, err)
	}
	if r := runPublicCommand(t, client, "set_artifact", `{"memory_id":99999,"artifact_type":"file","artifact_ref":"source.go"}`); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	stats := run("effectiveness_stats", `{}`)["stats"].(map[string]any)
	if stats["low_effectiveness_count"] != float64(305) || stats["never_surfaced_l2"] != float64(305) || len(stats) != 4 {
		t.Fatal(stats)
	}
	_, err = tx.Exec(ctx, `CREATE ROLE memory_query_scope_test NOINHERIT NOBYPASSRLS;
 GRANT USAGE ON SCHEMA query_command_test TO memory_query_scope_test;
 GRANT SELECT ON memories TO memory_query_scope_test;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY query_scope_test ON memories USING(scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR
 (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
 SET LOCAL ROLE memory_query_scope_test;`)
	if err != nil {
		t.Fatal(err)
	}
	client = clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB})))
	for _, project := range []string{"app", "other"} {
		got := run("key_exists", `{"key":"record-1","scope_context":true,"project":"`+project+`"}`)
		if got["exists"] != (project == "app") {
			t.Fatalf("scoped import duplicate check: %v", got)
		}
	}

}
