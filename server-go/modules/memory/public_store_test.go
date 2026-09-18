package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestStorePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"key":"x","content":" "}`, `{"key":"x","content":"y","confidence":null}`, `{"key":"x","content":"y","confidence":2}`, `{"key":"x","content":"y","epistemic_kind":"anything"}`} {
		if r := runPublicCommand(t, client, "store", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	if r := runPublicCommand(t, client, "store", `{"key":"x","content":"y"}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}

func TestStorePublicPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL store regression")
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA store_command_test;
CREATE FUNCTION store_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
SET LOCAL search_path TO pg_temp,store_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,key text,content text,tier text,kind text,epistemic_kind text,
 scope_type text,scope_value text,confidence double precision,confidence_ceiling double precision,use_count int DEFAULT 0,
 lifecycle_state text,activation_suppressed int DEFAULT 0,archive_reason text DEFAULT '',use_cases text DEFAULT '',last_used_at text DEFAULT '',source_session text DEFAULT '',provenance_category text DEFAULT '',
 valid_from text DEFAULT '',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
CREATE UNIQUE INDEX memory_key_scope ON memories(kind,key,scope_type,scope_value);
CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text,UNIQUE(memory_id,scope_type,scope_value));
CREATE TEMP TABLE memory_links(id bigserial PRIMARY KEY,source_id bigint,target_id bigint,relation text);
CREATE TEMP TABLE memory_rejection_tombstones(object_kind text,memory_key text,memory_content text,scope_type text,scope_value text,active int DEFAULT 1);
CREATE TEMP TABLE memory_summaries(id bigserial PRIMARY KEY,memory_id bigint,scope text,summary text);
CREATE TEMP TABLE memory_fact_actors(memory_id bigint PRIMARY KEY REFERENCES memories(id) ON DELETE CASCADE,actor_principal text,actor_role text,authority_rank int,authenticated int,transport_identity text,captured_at text DEFAULT pg_now_text());
CREATE TEMP TABLE kb_async_jobs(id bigserial PRIMARY KEY,kind text,document_id bigint,project text,status text,updated_at text,UNIQUE(kind,document_id));`)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	client := clientForHandler(t, handler)
	caller := bus.CommandContext{Authenticated: true, Principal: "user:alice", UserAuthority: true, TransportIdentity: "cert:server"}
	put := func(args string, verified bool) map[string]any {
		t.Helper()
		if !verified {
			return runPublicCommand(t, client, "store", args)
		}
		r, status := invokeContextCommand(t, handler, 0, caller, "store", args)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		return r
	}
	r := put(`{"key":"model-note","content":"a useful note","authority":"user","actor":"user:forged","tier":"L2","confidence":1,"session_id":"session","use_cases":"answer questions"}`, false)
	if r["status"] != "ok" {
		t.Fatal(r)
	}
	record := r["memory"].(map[string]any)
	if r["id"] != record["id"] || record["confidence"] != 0.8 || record["source_session"] != "session" || record["use_cases"] != "answer questions" || record["provenance_category"] != "agent_message" {
		t.Fatal(r)
	}
	checkActor := func(id any, principal string, rank, authenticated int) {
		t.Helper()
		var p string
		var n, a int
		err := tx.QueryRow(ctx, `SELECT actor_principal,authority_rank,authenticated FROM memory_fact_actors WHERE memory_id=$1`, int64(id.(float64))).Scan(&p, &n, &a)
		if err != nil || p != principal || n != rank || a != authenticated {
			t.Fatalf("actor=%s/%d/%d %v", p, n, a, err)
		}
	}
	checkActor(r["id"], "system:model-inference", 10, 0)
	user := put(`{"key":"user-note","content":"verified note","authority":"user","scope_context":true,"project":"app"}`, true)
	if user["status"] != "ok" || user["memory"].(map[string]any)["confidence"] != float64(1) || user["memory"].(map[string]any)["provenance_category"] != "user_stated" {
		t.Fatal(user)
	}
	checkActor(user["id"], "user:alice", 30, 1)
	// A failed model edit must roll back the closed interval, replacement,
	// lineage, and extraction actor together.
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs ADD CONSTRAINT update_enqueue_failure CHECK (document_id<0) NOT VALID`); err != nil {
		t.Fatal(err)
	}
	edit := fmt.Sprintf(`{"id":%.0f,"content":"failed replacement"}`, user["id"])
	if r := runPublicCommand(t, client, "update", edit); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	var active bool
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state='active' AND valid_until='' AND content='verified note' FROM memories WHERE id=$1`, int64(user["id"].(float64))).Scan(&active); err != nil || !active {
		t.Fatal(active, err)
	}
	checkActor(user["id"], "user:alice", 30, 1)
	var leaked int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE content='failed replacement'`).Scan(&leaked); err != nil || leaked != 0 {
		t.Fatal(leaked, err)
	}
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs DROP CONSTRAINT update_enqueue_failure`); err != nil {
		t.Fatal(err)
	}
	// Reusing a key for model text must replace stale, higher extraction authority.
	replaced := put(`{"key":"user-note","content":"model replacement","scope_context":true,"project":"app"}`, true)
	if replaced["status"] != "ok" || replaced["id"] != user["id"] {
		t.Fatal(replaced, user)
	}
	checkActor(replaced["id"], "system:model-inference", 10, 0)
	var jobs int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs WHERE kind='memory_facts' AND status='pending'`).Scan(&jobs); err != nil || jobs != 2 {
		t.Fatal(jobs, err)
	}
	if low := put(`{"key":"hypothesis","content":"tentative","tier":"L5","authority":"user"}`, true); low["status"] != "ok" || low["memory"].(map[string]any)["confidence"] != 0.5 {
		t.Fatal(low)
	}
	// Both failure positions roll back the memory row, actor capture and enqueue.
	for _, tt := range []struct{ table, check, key string }{
		{"memory_fact_actors", "authority_rank<0", "capture-failure"}, {"kb_async_jobs", "document_id<0", "enqueue-failure"},
	} {
		if _, err := tx.Exec(ctx, `ALTER TABLE `+tt.table+` ADD CONSTRAINT injected_failure CHECK (`+tt.check+`) NOT VALID`); err != nil {
			t.Fatal(err)
		}
		args, _ := json.Marshal(map[string]any{"key": tt.key, "content": "must roll back"})
		if r := put(string(args), false); r["status"] != "error" {
			t.Fatal(r)
		}
		var count int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key=$1`, tt.key).Scan(&count); err != nil || count != 0 {
			t.Fatal(count, err)
		}
		if _, err := tx.Exec(ctx, `ALTER TABLE `+tt.table+` DROP CONSTRAINT injected_failure`); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_rejection_tombstones VALUES ('memory','blocked','rejected content','global','_global',1)`); err != nil {
		t.Fatal(err)
	}
	if r := put(`{"key":"blocked","content":"rejected content"}`, true); r["status"] != "error" {
		t.Fatal(r)
	}
	// Store scope is applied by the owner transaction, including import overrides.
	var scope string
	if err := tx.QueryRow(ctx, `SELECT scope_value FROM memories WHERE id=$1`, int64(user["id"].(float64))).Scan(&scope); err != nil || scope != "app" {
		t.Fatal(scope, err)
	}

	// Replacement preserves history and scope while lowering inherited authority.
	original := put(`{"key":"version#v123","content":"original","authority":"user","tier":"L2","use_cases":"context","session_id":"before","scope_context":true,"project":"app"}`, true)
	oldID := int64(original["id"].(float64))
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes VALUES ($1,'workspace','team')`, oldID); err != nil {
		t.Fatal(err)
	}
	replacementArgs := fmt.Sprintf(`{"old_id":%d,"new_content":"replacement","confidence":1,"session_id":"after","scope_context":true,"project":"app"}`, oldID)
	replacement := runPublicCommand(t, client, "supersede", replacementArgs)
	if replacement["status"] != "ok" {
		t.Fatal(replacement)
	}
	fresh := replacement["memory"].(map[string]any)
	newID := int64(fresh["id"].(float64))
	if newID == oldID || fresh["key"] != "version#v123" || fresh["source_session"] != "after" || fresh["use_cases"] != "context" || fresh["confidence"] != 0.8 || fresh["provenance_category"] != "agent_message" {
		t.Fatal(replacement)
	}
	checkActor(fresh["id"], "system:model-inference", 10, 0)
	var boundary, state, oldKey string
	var equal bool
	err = tx.QueryRow(ctx, `SELECT o.key,o.lifecycle_state,o.valid_until,o.valid_until=n.valid_from FROM memories o,memories n WHERE o.id=$1 AND n.id=$2`, oldID, newID).Scan(&oldKey, &state, &boundary, &equal)
	if err != nil || !equal || boundary == "" || state != "superseded" || oldKey != fmt.Sprintf("version#v123#v%d", oldID) {
		t.Fatal(oldKey, state, boundary, equal, err)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_links WHERE source_id=$1 AND target_id=$2 AND relation='supersedes'`, newID, oldID).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_scopes WHERE memory_id=$1 AND scope_value='team'`, newID).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	for _, id := range []int64{oldID, newID} {
		args, _ := json.Marshal(map[string]any{"id": id, "as_of": boundary})
		historical := runPublicCommand(t, client, "get", string(args))
		if historical["status"] != "ok" || historical["valid_at"] != (id == newID) {
			t.Fatal(historical)
		}
	}
	// The same source cannot be superseded twice.
	if r := runPublicCommand(t, client, "supersede", replacementArgs); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
		args, _ := json.Marshal(map[string]any{"key": kind, "content": "immutable", "epistemic_kind": kind})
		item := put(string(args), false)
		args, _ = json.Marshal(map[string]any{"old_id": item["id"], "new_content": "changed"})
		if r := runPublicCommand(t, client, "supersede", string(args)); r["kind"] != "conflict" {
			t.Fatal(kind, r)
		}
	}
	// A failed extraction enqueue must roll back closing the source and all derived rows.
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs ADD CONSTRAINT fail_replacement CHECK (document_id<0) NOT VALID`); err != nil {
		t.Fatal(err)
	}
	args := fmt.Sprintf(`{"old_id":%d,"new_content":"must roll back"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=$1`, newID).Scan(&state); err != nil || state != "active" {
		t.Fatal(state, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE content='must roll back'`).Scan(&count); err != nil || count != 0 {
		t.Fatal(count, err)
	}
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs DROP CONSTRAINT fail_replacement`); err != nil {
		t.Fatal(err)
	}
	// Replacement under a non-owner role cannot reach a different project's source.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_store_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA store_command_test TO memory_store_test;
GRANT SELECT,UPDATE,DELETE,INSERT ON memories,memory_rejection_tombstones,memory_links,memory_scopes,memory_summaries,memory_fact_actors,kb_async_jobs TO memory_store_test;
GRANT USAGE,SELECT ON SEQUENCE memories_id_seq,memory_links_id_seq,kb_async_jobs_id_seq TO memory_store_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_visibility ON memories USING (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_store_test;`)
	if err != nil {
		t.Fatal(err)
	}
	args = fmt.Sprintf(`{"old_id":%d,"new_content":"hidden","scope_context":true,"project":"other"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	args = fmt.Sprintf(`{"old_id":%d,"new_content":"visible","scope_context":true,"project":"app"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["status"] != "ok" {
		t.Fatal(r)
	}
	// The owner enforces screening even when no native pre-send client is used.
	for _, args := range []string{
		`{"key":"password=identity","content":"safe"}`,
		`{"key":"pem-note","content":"-----BEGIN PRIVATE KEY-----\nsecret body\n-----END PRIVATE KEY-----"}`,
	} {
		if r := put(args, false); r["kind"] != "unavailable" {
			t.Fatal("sensitive write accepted", r)
		}
	}
	redacted := put(`{"key":"redacted-note","content":"password=first token=second","use_cases":"secret=third"}`, false)
	if redacted["status"] != "ok" || redacted["memory"].(map[string]any)["content"] != "[REDACTED] [REDACTED]" || redacted["memory"].(map[string]any)["use_cases"] != "[REDACTED]" {
		t.Fatal(redacted)
	}
	editRaw := fmt.Sprintf(`{"id":%.0f,"content":"-----BEGIN PRIVATE KEY-----\nsecret"}`, redacted["id"])
	if r := runPublicCommand(t, client, "update", editRaw); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	editRaw = fmt.Sprintf(`{"old_id":%.0f,"new_content":"password=replacement"}`, redacted["id"])
	if r := runPublicCommand(t, client, "supersede", editRaw); r["status"] != "ok" || r["memory"].(map[string]any)["content"] != "[REDACTED]" {
		t.Fatal(r)
	}

}
