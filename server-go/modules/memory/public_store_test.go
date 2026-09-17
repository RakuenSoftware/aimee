package memory

import (
	"context"
	"encoding/json"
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
}
