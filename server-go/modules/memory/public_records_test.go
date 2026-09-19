package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestRecordPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, tt := range []struct{ verb, args string }{
		{"get", `{"id":0}`}, {"get", `{"id":1.5}`}, {"fact_history", `{}`},
		{"get", `{"id":"9007199254740993x","view":"console"}`},
		{"get", `{"id":9007199254740993,"view":"console"}`},
		{"list_session_scope_priority_like", `{"pattern":null}`}, {"search_facts_patterns_by_keyword", `{}`},
		{"scope_visibility_rank", `{"ids":null}`}, {"tag_scope", `{"memory_id":1,"scope_type":"user","scope_value":"alice"}`},
		{"tag_workspace", `{"memory_id":1,"workspace":""}`},
		{"find_facts_visible", `{}`}, {"find_facts_scoped", `{"query":null}`},
		{"find_facts_scoped", `{"query":"cache","scope_type":"project"}`},
	} {
		if r := runPublicCommand(t, client, tt.verb, tt.args); r["kind"] != "invalid_argument" {
			t.Fatalf("%s: %v", tt.verb, r)
		}
	}
	if r := runPublicCommand(t, client, "scope_visibility_rank", `{"ids":[]}`); r["status"] != "ok" || len(r["ranks"].([]any)) != 0 {
		t.Fatal(r)
	}
	for _, args := range []string{`{"key":"missing","view":"console"}`, `{"key":"missing","format":"mcp"}`} {
		if r := runPublicCommand(t, client, "fact_history", args); r["kind"] != "unavailable" || r["output"] != nil {
			t.Fatal(r)
		}
	}
	// A failed module must never look like a successful, empty recall.
	for _, verb := range []string{"list", "get", "fact_history", "find_facts_visible", "find_facts_scoped"} {
		if r := runPublicCommand(t, client, verb, `{"id":1,"key":"missing","query":"anything"}`); r["kind"] != "unavailable" {
			t.Fatalf("%s: %v", verb, r)
		}
	}
	for _, verb := range []string{"get", "list"} {
		if r := runPublicCommand(t, client, verb, `{"id":"9007199254740993","view":"console"}`); r["kind"] != "unavailable" || r["output"] != nil {
			t.Fatalf("%s console failure became output: %v", verb, r)
		}
	}
}

func TestRecordPublicPostgres(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA record_command_test;
CREATE FUNCTION record_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
SET LOCAL search_path TO pg_temp,record_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,key text,content text DEFAULT 'content',tier text DEFAULT 'L2',kind text DEFAULT 'fact',
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'app',confidence double precision DEFAULT 1,use_count int DEFAULT 2,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,use_cases text DEFAULT 'answer questions',last_used_at text DEFAULT '',source_session text DEFAULT 'session-1',provenance_category text DEFAULT 'human',
 valid_from text DEFAULT '2026-01-01',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
CREATE TEMP TABLE memory_summaries(id bigserial PRIMARY KEY,memory_id bigint,scope text,summary text);
CREATE TEMP TABLE memory_workspaces(memory_id bigint,workspace text,PRIMARY KEY(memory_id,workspace));
CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text,UNIQUE(memory_id,scope_type,scope_value));
INSERT INTO memories(key,content) VALUES ('release',repeat('memory detail ',700));
INSERT INTO memories(key,lifecycle_state,valid_until) VALUES ('release#v1','superseded','2026-06-01');
INSERT INTO memories(key,scope_value) VALUES ('private-key','private');
INSERT INTO memories(key,scope_type,scope_value) VALUES ('global-key','global','_global'),('workspace-key','workspace','team');
INSERT INTO memory_summaries(memory_id,scope,summary) VALUES (1,'summary','fallback'),(1,'headline','Release headline'),(3,'headline','Private headline');
INSERT INTO memories(key) SELECT 'row-'||i FROM generate_series(1,110) i;`)
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
	record := run("get", `{"id":1}`)["memory"].(map[string]any)
	if len(record) != 16 || record["headline"] != "Release headline" || record["content"] != strings.Repeat("memory detail ", 700) || record["use_count"] != float64(2) || record["source_session"] != "session-1" || record["provenance_category"] != "human" {
		t.Fatal(record)
	}
	for _, tt := range []struct {
		verb, args string
		keys       []string
	}{
		{"find_facts_visible", `{"query":"key","workspace":"team","project":"app","include_all":true}`, []string{"workspace-key", "global-key"}},
		{"find_facts_visible", `{"query":"key"}`, []string{"global-key"}},
		{"find_facts_scoped", `{"query":"key","scope_type":"project","scope_value":"private"}`, []string{"private-key"}},
		{"find_facts_scoped", `{"query":"key","scope_type":"workspace","scope_value":"team"}`, []string{"workspace-key"}},
		{"find_facts_scoped", `{"query":"key"}`, []string{"global-key"}},
		{"find_facts_visible", `{"query":"absent","workspace":"team","project":"app"}`, []string{}},
	} {
		r := run(tt.verb, tt.args)
		rows := r["facts"].([]any)
		if len(rows) != len(tt.keys) {
			t.Fatal(tt.verb, tt.args, r)
		}
		for i, key := range tt.keys {
			row := rows[i].(map[string]any)
			if row["key"] != key || len(row) != 16 {
				t.Fatal(row)
			}
		}
	}
	full := run("find_facts_visible", `{"query":"release","project":"app"}`)["facts"].([]any)
	if len(full) != 1 || full[0].(map[string]any)["content"] != record["content"] {
		t.Fatal(full)
	}
	if r := runPublicCommand(t, client, "get", `{"id":2}`); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	for _, tt := range []struct {
		when  string
		valid bool
	}{{"2026-05-31", true}, {"2026-06-01", false}, {"2025-12-31", false}} {
		if r := run("get", `{"id":2,"as_of":"`+tt.when+`"}`); r["valid_at"] != tt.valid || r["as_of"] != tt.when {
			t.Fatal(r)
		}
	}
	for _, tt := range []struct {
		verb, args string
		count      int
	}{
		{"list", `{}`, 20}, {"list", `{"limit":1000}`, 64}, {"list", `{"kind":"missing"}`, 0},
		{"fact_history", `{"key":"release"}`, 2}, {"top_l2_facts", `{}`, 5},
		{"load_eval_corpus", `{}`, 100}, {"load_eval_corpus", `{"max":0}`, 20},
		{"list_session_scope_priority", `{}`, 24}, {"list_session_scope_priority_like", `{"pattern":"release%"}`, 1},
		{"search_facts_patterns_by_keyword", `{"keyword":"release"}`, 1},
	} {
		r := run(tt.verb, tt.args)
		key := "memories"
		if tt.verb == "fact_history" {
			key = "history"
		}
		if rows := r[key].([]any); len(rows) != tt.count {
			t.Fatalf("%s: got %d want %d", tt.verb, len(rows), tt.count)
		}
		if tt.verb == "load_eval_corpus" && r["label"] != "durable L1-L3" {
			t.Fatal(r)
		}
	}
	if r := run("get", `{"id":2,"as_of":"not-a-date"}`); r["valid_at"] != "unknown" {
		t.Fatal(r)
	}
	run("tag_workspace", `{"memory_id":1,"workspace":"team"}`)
	run("tag_scope", `{"memory_id":1,"scope_type":"project","scope_value":"app"}`)
	var tags int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_scopes WHERE memory_id=1`).Scan(&tags); err != nil || tags != 2 {
		t.Fatalf("tags=%d %v", tags, err)
	}
	if r := runPublicCommand(t, client, "tag_workspace", `{"memory_id":9999,"workspace":"team"}`); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	longKey := strings.Repeat("履歴", 180)
	fullContent := strings.Repeat("長い記憶", 1800)
	_, err = tx.Exec(ctx, `INSERT INTO memories(id,key,content,tier,kind) SELECT 9007199254740993+g,$1||'#v'||g::text,$2,'L3','pattern' FROM generate_series(0,69) g`, longKey, fullContent)
	if err != nil {
		t.Fatal(err)
	}
	console := run("get", `{"id":"9007199254740993","view":"console","format":"json"}`)["output"].(string)
	var consoleRecord map[string]json.RawMessage
	if err := json.Unmarshal([]byte(console), &consoleRecord); err != nil || len(consoleRecord) != 12 || string(consoleRecord["id"]) != "9007199254740993" {
		t.Fatal("console record schema/ID", err, console)
	}
	var content string
	if err := json.Unmarshal(consoleRecord["content"], &content); err != nil || content != fullContent {
		t.Fatal("console record content truncated", err)
	}
	console = run("get", `{"id":"9007199254740993","view":"console","format":"json","fields":"id,content,created_at","profile":"compact"}`)["output"].(string)
	consoleRecord = nil
	if err := json.Unmarshal([]byte(console), &consoleRecord); err != nil || len(consoleRecord) != 2 || string(consoleRecord["id"]) != "9007199254740993" {
		t.Fatal("console record selection", err, console)
	}
	console = run("list", `{"tier":"L3","kind":"pattern","limit":1000,"view":"console","format":"json"}`)["output"].(string)
	var consoleRows []struct {
		ID           int64
		Key, Content string
	}
	if err := json.Unmarshal([]byte(console), &consoleRows); err != nil || len(consoleRows) != 64 {
		t.Fatal("console list limit", err, len(consoleRows))
	}
	for _, row := range consoleRows {
		if row.ID < 9007199254740993 || row.ID > 9007199254741062 || row.Content != fullContent || !strings.HasPrefix(row.Key, longKey) {
			t.Fatal("console list truncated/rounded", row.ID)
		}
	}
	if out := run("list", `{"kind":"missing","view":"console"}`)["output"]; out != "[]\n" {
		t.Fatal("console empty list", out)
	}
	for _, verb := range []string{"get", "list"} {
		if out := run(verb, `{"id":"9007199254740993","view":"console","format":"text"}`)["output"]; out != "" {
			t.Fatal("legacy console text contract changed", out)
		}
	}
	args, _ := json.Marshal(map[string]any{"key": longKey, "max": 100, "view": "console", "format": "json"})
	out := run("fact_history", string(args))["output"].(string)
	var history []struct {
		ID                               int64
		Key, Content, ProvenanceCategory string
	}
	if err := json.Unmarshal([]byte(out), &history); err != nil || len(history) != 64 || history[0].ID != 9007199254741062 || history[0].Content != fullContent || history[0].Key != longKey+"#v69" {
		t.Fatal("history truncated/rounded", err, len(history))
	}
	args, _ = json.Marshal(map[string]any{"key": longKey, "max": 1, "view": "console", "fields": "id,key,created_at,content", "profile": "compact"})
	out = run("fact_history", string(args))["output"].(string)
	if strings.Contains(out, "created_at") || !strings.Contains(out, "9007199254741062") || !strings.Contains(out, fullContent) || strings.Contains(out, "confidence") {
		t.Fatal("filtered history", out)
	}
	args, _ = json.Marshal(map[string]any{"key": longKey, "max": 100, "format": "mcp"})
	out = run("fact_history", string(args))["output"].(string)
	var mcp struct {
		Status  string
		Count   int
		History []struct {
			ID      int64
			Content string
		}
	}
	if err := json.Unmarshal([]byte(out), &mcp); err != nil || mcp.Status != "ok" || mcp.Count != 64 || mcp.History[0].ID != 9007199254741062 || mcp.History[0].Content != fullContent {
		t.Fatal("MCP history truncated/rounded", err)
	}
	out = run("fact_history", `{"key":"no-such-key","format":"mcp"}`)["output"].(string)
	if err := json.Unmarshal([]byte(out), &mcp); err != nil || mcp.Status != "empty" || mcp.Count != 0 || len(mcp.History) != 0 {
		t.Fatal(out, err)
	}
	// Verify visibility with a real non-owner connection, including metadata.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_record_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA record_command_test TO memory_record_test;
GRANT SELECT,UPDATE ON memories TO memory_record_test;
GRANT SELECT ON memory_summaries TO memory_record_test;
GRANT SELECT,INSERT ON memory_scopes,memory_workspaces TO memory_record_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_memory_visibility ON memories USING
 (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR
  (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)) OR
  (scope_type='workspace' AND scope_value=current_setting('aimee.memory_workspace',true)));
SET LOCAL ROLE memory_record_test;`)
	if err != nil {
		t.Fatal(err)
	}
	if r := runPublicCommand(t, client, "get", `{"id":3,"scope_context":true,"project":"app"}`); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	if r := runPublicCommand(t, client, "get", `{"id":"3","scope_context":true,"project":"app","view":"console"}`); r["kind"] != "not_found" || r["output"] != nil {
		t.Fatal("console disclosed hidden record", r)
	}
	console = run("list", `{"scope_context":true,"limit":64,"view":"console"}`)["output"].(string)
	if err := json.Unmarshal([]byte(console), &consoleRows); err != nil || len(consoleRows) != 1 || consoleRows[0].Key != "global-key" {
		t.Fatal("console list crossed scope", err, console)
	}
	ranks := run("scope_visibility_rank", `{"ids":[1,3,4,5,1,9999,null],"workspace":"team","project":"app"}`)["ranks"].([]any)
	for i, want := range []float64{3, 0, 1, 2, 3, 0, 0} {
		if ranks[i] != want {
			t.Fatal(ranks)
		}
	}
	if r := runPublicCommand(t, client, "tag_scope", `{"memory_id":3,"scope_context":true,"project":"app","scope_type":"project","scope_value":"private"}`); r["kind"] != "not_found" {
		t.Fatal("tag destination widened source visibility", r)
	}
	if r := run("list", `{"scope_context":true,"limit":64}`); len(r["memories"].([]any)) != 1 || r["active_context_missing"] != true {
		t.Fatal(r)
	}
}
