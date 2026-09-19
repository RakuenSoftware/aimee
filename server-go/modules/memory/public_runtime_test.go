package memory

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestRuntimePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, tt := range []struct{ verb, args string }{
		{"assemble_context", `{"explain":"yes"}`}, {"assemble_context", `{"explain":null}`}, {"query_edges", `{}`}, {"query_edges", `{"entity":""}`}, {"check_drift", `{"task_id":0}`},
	} {
		if r := runPublicCommand(t, client, tt.verb, tt.args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	for _, verb := range []string{"briefing", "alerts", "assemble_context", "compact_windows"} {
		if r := runPublicCommand(t, client, verb, `{}`); r["kind"] != "unavailable" {
			t.Fatalf("%s: %v", verb, r)
		}
	}
}

func TestRuntimePublicPostgres(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA runtime_command_test;
CREATE FUNCTION runtime_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
CREATE FUNCTION runtime_command_test.aimee_utc_text_timestamptz(t text) RETURNS timestamptz LANGUAGE sql AS $$ SELECT t::timestamptz $$;
SET LOCAL search_path TO pg_temp,runtime_command_test,public;
CREATE TEMP TABLE memories(id bigint PRIMARY KEY,key text,content text DEFAULT 'content',tier text DEFAULT 'L2',kind text DEFAULT 'fact',
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'app',confidence double precision DEFAULT 1,use_count int DEFAULT 2,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,use_cases text DEFAULT '',source_session text DEFAULT '',ttl_at text DEFAULT '',
 sensitivity text DEFAULT 'normal',evidence_strength double precision DEFAULT 0.5,observation_count int DEFAULT 1,last_used_at text,
 created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text(),valid_from text DEFAULT '',valid_until text DEFAULT '');
CREATE TEMP TABLE memory_episodes(id bigint PRIMARY KEY,memory_id bigint,source_session text,episode_text text,reference_time text,created_at text DEFAULT pg_now_text());
CREATE TEMP TABLE memory_entities(memory_id bigint,entity text);
CREATE TEMP TABLE memory_conflicts(id bigint,memory_a bigint,memory_b bigint,detected_at text,resolved int,resolution text);
CREATE TEMP TABLE memory_relations(id bigserial PRIMARY KEY,memory_id bigint,episode_id bigint,src_entity text,relation text,dst_entity text,fact_text text DEFAULT '',valid_at text DEFAULT '',invalid_at text DEFAULT '',weight double precision DEFAULT 1.5,created_at text DEFAULT pg_now_text());
CREATE TEMP TABLE tasks(id bigint PRIMARY KEY,parent_id bigint,title text);
INSERT INTO memories(id,key,content) VALUES (1,'release','release the app');
INSERT INTO memories(id,key,content,scope_type,scope_value) VALUES (2,'common','common conventions','global','_global'),(3,'private','secret project plan','project','private');
INSERT INTO memory_episodes(id,memory_id,source_session,episode_text,reference_time) VALUES (1,1,'app-session','release summary','2026-09-01'),(2,3,'private-session','secret summary','2026-09-02');
INSERT INTO memory_entities(memory_id,entity) VALUES (1,'app'),(3,'secret');
INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity) SELECT 1,'app','uses','tool-'||i FROM generate_series(1,280) i;
INSERT INTO tasks(id,parent_id,title) VALUES (1,0,'release app'),(2,1,'update changelog');
CREATE ROLE memory_runtime_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA runtime_command_test TO memory_runtime_test;
GRANT SELECT ON memories,memory_episodes,memory_entities,memory_conflicts,memory_relations,tasks TO memory_runtime_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_memory_visibility ON memories USING
 (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR
  (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_runtime_test;`)
	if err != nil {
		t.Fatal(err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB})))
	run := func(verb, args string) map[string]any {
		t.Helper()
		r := runPublicCommand(t, client, verb, args)
		if r["status"] != "ok" {
			t.Fatalf("%s: %v", verb, r)
		}
		return r
	}
	briefing := run("briefing", `{"scope_context":true,"project":"app"}`)["briefing"].(map[string]any)
	activities := briefing["recent_activity"].([]any)
	if len(activities) != 1 || activities[0].(map[string]any)["summary"] != "release summary" || len(briefing["key_facts"].([]any)) != 2 || len(briefing["active_entities"].([]any)) != 1 {
		t.Fatal(briefing)
	}
	if b := run("briefing", `{"limit_tokens":99999}`)["briefing"].(map[string]any); b["limit_tokens"] != float64(1024) || len(b["recent_activity"].([]any)) != 2 {
		t.Fatal(b)
	}
	block := run("assemble_context", `{"scope_context":true,"project":"app"}`)["context"].(string)
	if !strings.Contains(block, "release the app") || !strings.Contains(block, "common conventions") || strings.Contains(block, "secret") {
		t.Fatal(block)
	}
	explained := run("assemble_context", `{"scope_context":true,"project":"app","explain":true}`)
	if explained["context"] != block || explained["candidate_scope"] != "returned_rows" || strings.Contains(explained["explain_text"].(string), "secret") {
		t.Fatal(explained)
	}
	candidates := explained["candidates"].([]any)
	if len(candidates) != 2 {
		t.Fatal(explained)
	}
	for _, value := range candidates {
		candidate := value.(map[string]any)
		id, ok := candidate["id"].(string)
		if !ok || (id != "1" && id != "2") || candidate["selected"] != true || candidate["tokens"].(float64) <= 0 {
			t.Fatal(candidate)
		}
	}
	empty := run("assemble_context", `{"scope_context":true,"project":"app","task_hint":"absent-sentinel","explain":true}`)
	if len(empty["candidates"].([]any)) != 0 || empty["context"] != "# Memory Context\n" {
		t.Fatal(empty)
	}
	if block := run("assemble_context", `{"scope_context":true,"task_hint":"common"}`)["context"].(string); !strings.Contains(block, "common conventions") || strings.Contains(block, "release the app") {
		t.Fatal(block)
	}
	for _, tt := range []struct {
		args  string
		count int
	}{{`{"entity":"app"}`, 128}, {`{"entity":"app","max":999}`, 256}, {`{"entity":"missing"}`, 0}} {
		rows := run("query_edges", tt.args)["edges"].([]any)
		if len(rows) != tt.count {
			t.Fatalf("edges=%d want=%d", len(rows), tt.count)
		}
		if len(rows) > 0 {
			r := rows[0].(map[string]any)
			if len(r) != 5 || r["weight"] != 1.5 || r["source"] != "app" {
				t.Fatal(r)
			}
		}
	}
	for _, tt := range []struct {
		args    string
		drifted bool
	}{
		{`{"task_id":1,"file_path":"CHANGELOG.md"}`, false}, {`{"task_id":1,"command":"release app"}`, false}, {`{"task_id":1}`, false}, {`{"task_id":1,"file_path":"unrelated.go"}`, true},
	} {
		if r := run("check_drift", tt.args); r["drifted"] != tt.drifted || r["task_title"] != "release app" || r["task_id"] != float64(1) {
			t.Fatal(r)
		}
	}
	if r := runPublicCommand(t, client, "check_drift", `{"task_id":999}`); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	if r := run("compact_windows", `{}`); r["summaries"] != float64(0) || r["facts"] != float64(0) {
		t.Fatal(r)
	}
	alerts := run("alerts", `{"scope_context":true,"project":"app"}`)["alerts"].(map[string]any)
	if len(alerts) != 4 || alerts["elapsed_ms"].(float64) < 0 {
		t.Fatal(alerts)
	}
	for _, key := range []string{"stale_pending", "unresolved_contradictions", "newly_superseded"} {
		if len(alerts[key].([]any)) != 0 {
			t.Fatal(alerts)
		}
	}
}

func TestServerSearchValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{
		`{"view":"server"}`, `{"view":"server","keywords":[]}`,
		`{"view":"server","keywords":[null]}`, `{"view":"server","keywords":[" "]}`,
		`{"view":"server","keywords":["x"],"limit":1.5}`,
		`{"view":"server","keywords":["x"],"limit":33}`,
		`{"view":"server","keywords":["x"],"limit":null}`,
	} {
		if r := runPublicCommand(t, client, "search", args); r["kind"] != "invalid_argument" {
			t.Fatal(args, r)
		}
	}
	if r := runPublicCommand(t, client, "search", `{"view":"server","keywords":["x"]}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}
