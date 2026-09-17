package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestDirectivePublicPostgresLifecycle(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA directive_test;
 CREATE FUNCTION directive_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 SET LOCAL search_path TO pg_temp,directive_test,public;
 CREATE TEMP TABLE epistemic_directives(id bigserial PRIMARY KEY, question text, topic text DEFAULT '',
 anchor_entity text DEFAULT '',anchor_file text DEFAULT '',cause text,priority bigint DEFAULT 50,
 state text DEFAULT 'open',memory_a_id bigint DEFAULT 0,memory_b_id bigint DEFAULT 0,
 resolution_memory_id bigint DEFAULT 0,evidence text DEFAULT '',source_session text DEFAULT '',
 surfaced_count bigint DEFAULT 0,last_surfaced_at text DEFAULT '',resolved_at text DEFAULT '',
 valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
 CREATE UNIQUE INDEX ON epistemic_directives(cause,topic) WHERE topic<>'' AND cause IN ('retrieval_failure','missing_config');
 CREATE UNIQUE INDEX ON epistemic_directives(memory_a_id,memory_b_id) WHERE cause='contradiction' AND memory_a_id<>0 AND memory_b_id<>0;`)
	if err != nil {
		t.Fatal(err)
	}
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	for _, verb := range []string{"directive_list", "directive_briefing", "directive_dashboard"} {
		if result := runPublicCommand(t, client, verb, `{}`); result["status"] != "ok" {
			t.Fatal(result)
		}
	}
	create := func(args string) map[string]any {
		result := runPublicCommand(t, client, "directive_create", args)
		if result["status"] != "ok" || result["dedup"] != false {
			t.Fatal(result)
		}
		row, ok := result["directive"].(map[string]any)
		if !ok || len(row) != 17 {
			t.Fatal("public directive envelope changed", result)
		}
		if _, exists := row["source_session"]; exists {
			t.Fatal("internal session leaked")
		}
		return row
	}
	if result := runPublicCommand(t, client, "directive_list", `{"state":"bogus"}`); result["status"] != "ok" || len(result["directives"].([]any)) != 0 {
		t.Fatal(result)
	}
	before := directiveMetrics().Created
	topic := create(`{"question":"Which backend?", "topic":"db", "cause":"missing_config","priority":0,"session":"s1","memory_a_id":99,"evidence":"untrusted"}`)
	if topic["priority"] != float64(0) || topic["memory_a_id"] != float64(0) || topic["evidence"] != "" {
		t.Fatal("defaults/selected fields lost", topic)
	}
	duplicate := runPublicCommand(t, client, "directive_create", `{"question":"Again?","topic":"db","cause":"missing_config"}`)
	if duplicate["dedup"] != true || duplicate["directive"] != nil || directiveMetrics().Created != before+1 {
		t.Fatal("dedup changed records or inflated telemetry", duplicate)
	}
	follow := create(`{"question":"Review design?"}`)
	if follow["priority"] != float64(50) || follow["cause"] != "user_follow_up" {
		t.Fatal(follow)
	}
	high := create(`{"question":"Urgent?","priority":999}`)
	if high["priority"] != float64(100) {
		t.Fatal(high)
	}
	block := runPublicCommand(t, client, "directive_briefing", `{"limit":1}`)["block"].(string)
	if block != "# Open Questions\n- [p100 · user_follow_up] Urgent?\n\n" {
		t.Fatal(block)
	}
	args, _ := json.Marshal(map[string]any{"id": topic["id"], "with_memory": 42, "note": "must not overwrite evidence"})
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["id"] != topic["id"] {
		t.Fatal(result)
	}
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["status"] != "error" {
		t.Fatal("resolved twice", result)
	}
	resolved, err := s.DirectiveGet(ctx, int64(topic["id"].(float64)))
	if err != nil || resolved.ResolutionMemoryID != 42 || resolved.Evidence != "" || resolved.SourceSession != "s1" {
		t.Fatalf("resolved=%+v %v", resolved, err)
	}
	args, _ = json.Marshal(map[string]any{"id": high["id"]})
	if result := runPublicCommand(t, client, "directive_suppress", string(args)); result["id"] != high["id"] {
		t.Fatal(result)
	}
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["status"] != "error" {
		t.Fatal("suppressed row resolved", result)
	}
	create(`{"question":"Expired?","valid_until":"2000-01-01"}`)
	if result := runPublicCommand(t, client, "directive_sweep_expired", `{}`); result["expired"] != float64(1) {
		t.Fatal(result)
	}
	dashboard := runPublicCommand(t, client, "directive_dashboard", `{}`)["dashboard"].(map[string]any)
	counts := dashboard["counts"].(map[string]any)
	if counts["open"] != float64(1) || counts["total"] != float64(4) || counts["suppressed"] != float64(1) || counts["resolved"] != float64(1) || counts["expired"] != float64(1) {
		t.Fatal(dashboard)
	}
	block = runPublicCommand(t, client, "directive_briefing", `{}`)["block"].(string)
	if strings.Contains(block, "Urgent?") || !strings.Contains(block, "Review design?") {
		t.Fatal(block)
	}
	_, err = tx.Exec(ctx, `INSERT INTO epistemic_directives(question,cause) SELECT 'extra','user_follow_up' FROM generate_series(1,300)`)
	if err != nil {
		t.Fatal(err)
	}
	list := runPublicCommand(t, client, "directive_list", `{"limit":999}`)
	if len(list["directives"].([]any)) != 256 {
		t.Fatal("list cap changed")
	}
}

func TestDirectivePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct{ verb, args string }{
		{"directive_create", `{}`}, {"directive_create", `{"question":"x","cause":"bogus"}`},
		{"directive_resolve", `{"id":-1}`}, {"directive_suppress", `{"id":1.5}`},
	} {
		if result := runPublicCommand(t, client, test.verb, test.args); result["kind"] != "invalid_argument" {
			t.Fatal(result)
		}
	}
}
