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

func TestRecordPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, tt := range []struct{ verb, args string }{
		{"get", `{"id":0}`}, {"get", `{"id":1.5}`}, {"fact_history", `{}`},
		{"find_facts", `{"query":"key","project":{}}`},
		{"find_facts", `{"query":"key","workspace":null}`},
		{"find_facts", `{"query":"key","scope_context":"yes"}`},
		{"find_facts", `{"query":"key","include_all":1}`},
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
CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
CREATE INDEX memory_units_card_fixture_idx ON memory_units(memory_id);
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
CREATE INDEX memory_lineage_card_fixture_idx ON memory_lineage(object_type,object_id);
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1,key text,content text DEFAULT 'content',tier text DEFAULT 'L2',kind text DEFAULT 'fact',
 epistemic_kind text DEFAULT 'world_fact',scope_type text DEFAULT 'project',scope_value text DEFAULT 'app',confidence double precision DEFAULT 1,use_count int DEFAULT 2,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,use_cases text DEFAULT 'answer questions',last_used_at text DEFAULT '',source_session text DEFAULT 'session-1',provenance_category text DEFAULT 'human',
 valid_from text DEFAULT '2026-01-01',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
CREATE TEMP TABLE derived_memory_dependencies(derived_kind text,derived_memory_id text,input_kind text,input_id text,input_version text,extractor_version text,derivation_policy_version text);
CREATE TEMP TABLE memory_summaries(id bigserial PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1,memory_id bigint,scope text,summary text);
CREATE TEMP TABLE memory_collection_owner(id integer PRIMARY KEY,owner_id uuid);
INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-4000-8000-000000000001');
CREATE TEMP TABLE memory_workspaces(memory_id bigint,workspace text,PRIMARY KEY(memory_id,workspace));
CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text,UNIQUE(memory_id,scope_type,scope_value));
INSERT INTO memories(key,content) VALUES ('release',repeat('memory detail ',700));
INSERT INTO memories(key,lifecycle_state,valid_until) VALUES ('release#v1','superseded','2026-06-01');
INSERT INTO memories(key,scope_value) VALUES ('private-key','private');
INSERT INTO memories(key,content,scope_type,scope_value) VALUES ('global-key','global marker','global','_global'),('workspace-key','workspace marker','workspace','team');
INSERT INTO memory_summaries(memory_id,scope,summary) VALUES (1,'summary','fallback'),(1,'headline','Release headline'),(3,'headline','Private headline');
INSERT INTO derived_memory_dependencies SELECT 'summary',s.id::text,'memory',m.id::text,m.record_revision::text,'go-derived-text-v1','summary-input-v1' FROM memory_summaries s JOIN memories m ON m.id=s.memory_id;
INSERT INTO memories(key) SELECT 'row-'||i FROM generate_series(1,110) i;`)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	client := clientForHandler(t, handler)
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
	for _, sample := range []struct {
		args    string
		allowed map[string]bool
	}{
		{`{"query":"key","project":"private"}`, map[string]bool{"private-key": true, "global-key": true}},
		{`{"query":"key","workspace":"team"}`, map[string]bool{"workspace-key": true, "global-key": true}},
		{`{"query":"key","include_all":false}`, map[string]bool{"global-key": true}},
	} {
		result := run("find_facts", sample.args)
		rows := result["facts"].([]any)
		if len(rows) != len(sample.allowed) {
			t.Fatal("legacy scope argument was ignored", sample.args, result)
		}
		for _, row := range rows {
			if !sample.allowed[row.(map[string]any)["key"].(string)] {
				t.Fatal("legacy scope argument admitted foreign row", sample.args, row)
			}
		}
	}
	// Credential restrictions narrow the audience without turning an implicit
	// audience query into an exact-scope query that loses public/global rows.
	caller := bus.CommandContext{Authenticated: true, Principal: "credential:fixture", ScopeKind: "project", ScopeID: "private"}
	listed, status := invokeContextCommand(t, handler, 0, caller, "list", `{"limit":64}`)
	rows, ok := listed["memories"].([]any)
	if status != bus.ModuleStatusOK || !ok || len(rows) != 2 {
		t.Fatal("verified audience lost global or admitted foreign records", listed, status)
	}
	seen := map[string]bool{}
	for _, row := range rows {
		seen[row.(map[string]any)["key"].(string)] = true
	}
	if !seen["private-key"] || !seen["global-key"] {
		t.Fatal("verified audience changed shared visibility", seen)
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
	// Text views keep full records; only session excerpts have an explicit budget.
	mcpText := run("list", `{"kind":"pattern","tier":"L3","format":"mcp","limit":1}`)["text"].(string)
	if !strings.Contains(mcpText, fullContent) || !strings.Contains(mcpText, longKey) {
		t.Fatal("MCP list truncated")
	}
	toolText := run("find_facts", `{"query":"release","project":"app","scope_context":true,"format":"tool"}`)
	if toolText["count"] != float64(1) || !strings.Contains(toolText["text"].(string), record["content"].(string)) {
		t.Fatal("agent search truncated", toolText)
	}
	for _, section := range []string{"project", "shared"} {
		args := `{"view":"session","section":"` + section + `","scope_context":true,"workspace":"team","project":"app","max":64,"budget_bytes":1200}`
		if section == "shared" {
			args = `{"view":"session","section":"shared","scope_context":true,"max":64,"budget_bytes":1200}`
		}
		view := run("list_session_scope_priority", args)["text"].(string)
		if len(view) > 1200 || strings.Contains(view, "private-key") {
			t.Fatal("session scope/budget", view)
		}
		if section == "shared" && (!strings.Contains(view, "global marker") || strings.Contains(view, "workspace marker")) {
			t.Fatal("shared ranks", view)
		}
	}
	if view := run("top_l2_facts", `{"view":"session","section":"facts","scope_context":true,"project":"app","budget_bytes":1}`)["text"]; view != "" {
		t.Fatal("session exceeded budget", view)
	}
	// Enrichment is another READ COMMITTED statement: mutations between source
	// selection and metadata selection must not create a mixed public record.
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	for _, mutation := range []string{
		"UPDATE memories SET content='changed after selection' WHERE id=1",
		"UPDATE memories SET scope_value='moved after selection' WHERE id=1",
		"UPDATE memories SET confidence=0.25 WHERE id=1",
		"UPDATE memories SET record_revision=record_revision+1 WHERE id=1",
		"UPDATE memory_collection_owner SET owner_id='00000000-0000-4000-8000-000000000002' WHERE id=1",
	} {
		if _, err := tx.Exec(ctx, "SAVEPOINT enrichment_race"); err != nil {
			t.Fatal(err)
		}
		selected, err := backend.getAtVersioned(ctx, Scope{}, 1, false, "", true)
		if err != nil {
			t.Fatal(err)
		}
		if rows, err := backend.publicRecords(ctx, []Record{selected}); err != nil || len(rows) != 1 || rows[0].Content != selected.Content {
			t.Fatal("unchanged enrichment refused", rows, err)
		}
		if _, err := tx.Exec(ctx, mutation); err != nil {
			t.Fatal(err)
		}
		if rows, err := backend.publicRecords(ctx, []Record{selected}); err == nil || rows != nil {
			t.Fatal("mixed enrichment accepted", mutation, rows, err)
		}
		if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT enrichment_race"); err != nil {
			t.Fatal(err)
		}
	}
	selected, err := backend.getAtVersioned(ctx, Scope{}, 1, false, "", false)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, "SAVEPOINT unversioned_enrichment; UPDATE memories SET content='changed unversioned payload' WHERE id=1"); err != nil {
		t.Fatal(err)
	}
	if rows, err := backend.publicRecords(ctx, []Record{selected}); err == nil || rows != nil {
		t.Fatal("mixed unversioned enrichment accepted", rows, err)
	}
	if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT unversioned_enrichment"); err != nil {
		t.Fatal(err)
	}
	// Legacy JSON omits versions, but the in-process read still captures the
	// owner/revision used for selection. Identical payloads do not excuse a
	// revision or lifecycle transition before enrichment.
	loaders := map[string]func() ([]Record, error){
		"get": func() ([]Record, error) { r, e := backend.Get(ctx, Scope{}, 1); return []Record{r}, e },
		"visible": func() ([]Record, error) {
			return backend.SearchVisible(ctx, DataRequest{Query: "release", Project: "app", Limit: 5})
		},
		"scoped": func() ([]Record, error) {
			return backend.Search(ctx, Scope{Type: "project", Value: "app"}, "release", "", "", 5)
		},
		"legacy-query": func() ([]Record, error) { return backend.QueryRecords(ctx, "like", "release", 0, 5) },
	}
	for name, load := range loaders {
		for _, mutation := range []string{
			"UPDATE memories SET record_revision=record_revision+1 WHERE id=1",
			"UPDATE memories SET activation_suppressed=1 WHERE id=1",
			"UPDATE memories SET valid_until=(now()-interval '1 second')::text WHERE id=1",
		} {
			if _, err := tx.Exec(ctx, "SAVEPOINT observed_enrichment"); err != nil {
				t.Fatal(err)
			}
			records, err := load()
			if err != nil || len(records) != 1 || records[0].ID != 1 || records[0].Version != nil || records[0].observedVersion == nil {
				t.Fatal("missing private read observation", name, records, err)
			}
			if rows, err := backend.publicRecords(ctx, records); err != nil || len(rows) != 1 {
				t.Fatal("current observation refused", name, rows, err)
			}
			if _, err := tx.Exec(ctx, mutation); err != nil {
				t.Fatal(err)
			}
			if rows, err := backend.publicRecords(ctx, records); err == nil || rows != nil {
				t.Fatal("stale private observation admitted", name, mutation, rows, err)
			}
			if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT observed_enrichment"); err != nil {
				t.Fatal(err)
			}
		}
	}
	historical, err := backend.FactHistory(ctx, "release", 5)
	if err != nil || len(historical) != 2 {
		t.Fatal("history selection", historical, err)
	}
	if rows, err := backend.publicRecords(ctx, historical); err != nil || len(rows) != 2 {
		t.Fatal("historical enrichment imposed current eligibility", rows, err)
	}
	if _, err := tx.Exec(ctx, "SAVEPOINT historical_enrichment; UPDATE memories SET lifecycle_state='revoked' WHERE id=2"); err != nil {
		t.Fatal(err)
	}
	if rows, err := backend.publicRecords(ctx, historical); err == nil || rows != nil {
		t.Fatal("revocation during historical enrichment admitted", rows, err)
	}
	if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT historical_enrichment"); err != nil {
		t.Fatal(err)
	}
	// A requested history key is an identity, not a SQL wildcard pattern.
	// An unrelated record cannot supply a false predecessor for a '%'/'_' key.
	if _, err := tx.Exec(ctx, `SAVEPOINT literal_history;
 INSERT INTO memories(key,content) VALUES ('history_%','literal current'),('history_%#v1','literal old'),('history_other#v1','unrelated old');
 UPDATE memories SET lifecycle_state='superseded',activation_suppressed=1 WHERE key='history_%#v1';`); err != nil {
		t.Fatal(err)
	}
	literal := run("fact_history", `{"key":"history_%","max":64}`)["history"].([]any)
	if len(literal) != 2 {
		t.Fatal("history interpreted a key as a pattern", literal)
	}
	for _, row := range literal {
		if row.(map[string]any)["key"] == "history_other#v1" {
			t.Fatal("unrelated predecessor admitted", literal)
		}
	}
	if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT literal_history"); err != nil {
		t.Fatal(err)
	}
	// Metadata loss must not silently produce partial success.
	if _, err := (&postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}).publicRecords(ctx, []Record{{ID: 9223372036854775807}}); err == nil {
		t.Fatal("missing metadata accepted")
	}
	previewArgs, _ := json.Marshal(map[string]any{"query": longKey, "scope_context": true, "project": "app", "format": "ingress", "limit": 5})
	previews := run("diagnose_scoped", string(previewArgs))["memories"].([]any)
	if len(previews) == 0 {
		t.Fatal("missing ingress previews")
	}
	for _, item := range previews {
		row := item.(map[string]any)
		if !strings.HasPrefix(row["id"].(string), "900719925474") || row["content"] != fullContent || row["preview"] != fullContent {
			t.Fatal("truncated ingress preview", row["id"])
		}
	}
	explainArgs, _ := json.Marshal(map[string]any{"query": longKey, "memory_id": "9007199254740993", "scope_context": true, "project": "app", "format": "mcp"})
	explained := run("explain_match", string(explainArgs))["output"].(string)
	var explanation struct {
		Memory struct {
			ID      int64
			Content string
		}
		Scores map[string]float64
	}
	if json.Unmarshal([]byte(explained), &explanation) != nil || explanation.Memory.ID != 9007199254740993 || explanation.Memory.Content != fullContent || len(explanation.Scores) != 11 {
		t.Fatal("invalid MCP explanation")
	}
	// Verify visibility with a real non-owner connection, including metadata.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_record_test NOINHERIT NOBYPASSRLS;
GRANT SELECT ON memory_units,memory_lineage,memory_collection_owner TO memory_record_test;
GRANT USAGE ON SCHEMA record_command_test TO memory_record_test;
GRANT SELECT,UPDATE ON memories TO memory_record_test;
GRANT SELECT ON memory_collection_owner,memory_summaries,derived_memory_dependencies TO memory_record_test;
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
	for _, tc := range []struct {
		args string
		keys map[string]bool
	}{
		{`{"scope_context":true,"max":100}`, map[string]bool{"global-key": true}},
		{`{"scope_context":true,"project":"private","max":100}`, map[string]bool{"global-key": true, "private-key": true}},
	} {
		result := run("load_eval_corpus", tc.args)
		rows := result["memories"].([]any)
		if len(rows) != len(tc.keys) {
			t.Fatal("evaluation corpus ignored explicit scope", result)
		}
		for _, row := range rows {
			if !tc.keys[row.(map[string]any)["key"].(string)] {
				t.Fatal("evaluation corpus widened scope", row)
			}
		}
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

func TestSessionTextBudget(t *testing.T) {
	text := strings.Repeat("記憶", 100)
	if got := sessionExcerpt(text, 301); len(got) != 300 || !strings.HasSuffix(got, "憶") {
		t.Fatal(got)
	}
	for input, want := range map[string]string{"Check Deploy example": "example", "  Verify List deployment": "deployment", "plain question": "plain question", "Check ": "Check "} {
		if got := sessionSearchKeyword(input); got != want {
			t.Fatal(input, got)
		}
	}
}
