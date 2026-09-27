package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestProspectivePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct{ verb, args string }{
		{"prospective_create", `{"trigger_text":"", "action_text":"do"}`},
		{"prospective_create", `{"trigger_text":"when", "action_text":""}`},
		{"prospective_create", `{"trigger_text":"when", "action_text":"do", "recurrence":"daily"}`},
		{"prospective_list", `{"state":"bogus"}`},
		{"prospective_complete", `{"id":1.2}`},
		{"prospective_mark_triggered", `{"id":0}`},
		{"prospective_mark_triggered", `{"id":"01"}`},
		{"prospective_mark_triggered", `{"id":"9223372036854775808"}`},
		{"prospective_complete", `{"id":9223372036854775807}`},
		{"prospective_match", `{"native_context_bytes":null}`},
		{"prospective_match", `{"native_context_bytes":-1}`},
		{"prospective_list", `{"native_context_bytes":0}`},
	} {
		result := runPublicCommand(t, client, test.verb, test.args)
		if result["status"] != "error" || result["kind"] != "invalid_argument" {
			t.Fatalf("%s: %v", test.verb, result)
		}
	}
}

func TestProspectivePublicPostgresLifecycle(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA prospective_test;
 CREATE FUNCTION prospective_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 SET LOCAL search_path TO pg_temp,prospective_test,public;
 CREATE TEMP TABLE prospective_memories(id bigserial PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1,trigger_text text NOT NULL,
 action_text text NOT NULL,anchor_entity text DEFAULT '',anchor_file text DEFAULT '',
 recurrence text DEFAULT 'once',state text DEFAULT 'armed',valid_until text DEFAULT '',
 source_session text DEFAULT '',trigger_count bigint DEFAULT 0,last_triggered_at text DEFAULT '',
 created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());`)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}))
	exerciseDeadlineReplay(t, ctx, tx, handler, "prospective")
	client := clientForHandler(t, handler)
	list := runPublicCommand(t, client, "prospective_list", `{}`)
	if rows, ok := list["prospectives"].([]any); !ok || len(rows) != 0 {
		t.Fatalf("empty list=%v", list)
	}
	create := func(recurrence string) (string, map[string]any) {
		args, _ := json.Marshal(map[string]any{"trigger_text": "release", "action_text": "review deployment", "recurrence": recurrence,
			"anchor_entity": "app", "operation": "delete", "session_id": "must-not-enter-public-record"})
		result := runPublicCommand(t, client, "prospective_create", string(args))
		if result["status"] != "ok" {
			t.Fatal(result)
		}
		row, ok := result["prospective"].(map[string]any)
		if !ok || row["state"] != "armed" || row["recurrence"] != recurrence || len(row) != 12 {
			t.Fatalf("create=%v", result)
		}
		if _, exists := row["source_session"]; exists {
			t.Fatal("session leaked into public envelope")
		}
		body, _ := json.Marshal(map[string]any{"id": row["id"]})
		return string(body), row
	}
	if result := runHostRuntime(t, handler, `{"operation":"prospective-briefing"}`); result["block"] != "" {
		t.Fatal(result)
	}
	once, onceRow := create("once")
	repeat, _ := create("repeat")
	matches := runPublicCommand(t, client, "prospective_match", `{"turn_text":"unrelated", "active_entity":"app", "max":1}`)
	if rows, ok := matches["matches"].([]any); !ok || len(rows) != 1 {
		t.Fatal(matches)
	}
	for _, budget := range []int{0, 1024} {
		result := runPublicCommand(t, client, "prospective_match", fmt.Sprintf(`{"turn_text":"release","max":2,"native_context_bytes":%d}`, budget))
		projection, ok := result["native_context"].(map[string]any)
		if !ok || projection["max_context_bytes"] != float64(budget) {
			t.Fatal(result)
		}
		ids := projection["retained_reminder_ids"].([]any)
		if budget == 0 && (len(ids) != 0 || projection["text"] != "" || len(result["matches"].([]any)) != 0) {
			t.Fatal("prospective allocation bypassed", result)
		}
		if budget == 1024 && (len(ids) != 2 || !strings.Contains(projection["text"].(string), "review deployment")) {
			t.Fatal("whole reminders lost", result)
		}
	}
	briefing := runHostRuntime(t, handler, `{"operation":"prospective-briefing","limit":1}`)
	if briefing["block"] != "# Open Commitments\n- when `release` → review deployment\n\n" {
		t.Fatal(briefing)
	}
	// The native matching fixtures included both exact file anchors and
	// morphological overlap; whole-turn substring matching loses both stems.
	_, err = tx.Exec(ctx, `INSERT INTO prospective_memories(trigger_text,action_text,anchor_file)
 VALUES ('when rotation policy discussed','remind about weekly rotation','src/tests/Rules.mk')`)
	if err != nil {
		t.Fatal(err)
	}
	for _, args := range []string{`{"turn_text":"routine edit","active_file":"src/tests/Rules.mk"}`, `{"turn_text":"we were discussing policies earlier"}`} {
		result := runPublicCommand(t, client, "prospective_match", args)
		if rows, ok := result["matches"].([]any); !ok || len(rows) != 1 {
			t.Fatal("anchor/stem recall lost", result)
		}
	}
	for _, args := range []string{once, repeat, repeat} {
		if result := runPublicCommand(t, client, "prospective_mark_triggered", args); result["status"] != "ok" {
			t.Fatal(result)
		}
	}
	if result := runPublicCommand(t, client, "prospective_mark_triggered", once); result["status"] != "error" {
		t.Fatal("one-shot retriggered", result)
	}
	for _, args := range []string{once, repeat} {
		if result := runPublicCommand(t, client, "prospective_complete", args); result["status"] != "ok" {
			t.Fatal(result)
		}
		if result := runPublicCommand(t, client, "prospective_complete", args); result["status"] != "error" {
			t.Fatal("terminal completed twice", result)
		}
	}
	var source string
	if err := tx.QueryRow(ctx, `SELECT source_session FROM prospective_memories WHERE id=$1`, int64(onceRow["id"].(float64))).Scan(&source); err != nil || source != "" {
		t.Fatalf("public session override: %q %v", source, err)
	}
	_, err = tx.Exec(ctx, `INSERT INTO prospective_memories(trigger_text,action_text,valid_until)
 VALUES ('expired','do','2000-01-01');
 INSERT INTO prospective_memories(trigger_text,action_text) SELECT 'release','do' FROM generate_series(1,300);`)
	if err != nil {
		t.Fatal(err)
	}
	if result := runPublicCommand(t, client, "prospective_sweep_expired", `{}`); result["expired"] != float64(1) {
		t.Fatal(result)
	}
	dashboard := runHostRuntime(t, handler, `{"operation":"prospective-dashboard"}`)["dashboard"].(map[string]any)
	counts := dashboard["counts"].(map[string]any)
	if counts["armed"] != float64(301) || counts["completed"] != float64(2) || counts["expired"] != float64(1) || counts["total"] != float64(304) {
		t.Fatal(dashboard)
	}
	if len(dashboard["recent"].([]any)) != 20 {
		t.Fatal("dashboard cap lost")
	}
	if _, ok := dashboard["metrics"].(map[string]any)["triggered_since_start"]; !ok {
		t.Fatal("metrics lost")
	}
	for _, limit := range []string{"256", "999"} {
		result := runPublicCommand(t, client, "prospective_list", `{"limit":`+limit+`}`)
		if rows, ok := result["prospectives"].([]any); !ok || len(rows) != 256 {
			t.Fatalf("list limit lost: %v", result)
		}
	}
	matches = runPublicCommand(t, client, "prospective_match", `{"turn_text":"release","max":999}`)
	if rows, ok := matches["matches"].([]any); !ok || len(rows) != 8 {
		t.Fatal("match cap lost", matches)
	}
	// Preserve exact IDs through public row serialization and the native
	// projection. A float64 map round trip silently changes this identity.
	if _, err := tx.Exec(ctx, `INSERT INTO prospective_memories(id,trigger_text,action_text)
        VALUES (9223372036854775807,'exactidentityfixture','retain exact reminder')`); err != nil {
		t.Fatal(err)
	}
	raw, err := client.Command(ctx, 73, "prospective_match", json.RawMessage(`{"turn_text":"exactidentityfixture","native_context_bytes":1024}`))
	if err != nil {
		t.Fatal(err)
	}
	var exact struct {
		Matches []struct {
			ID int64 `json:"id"`
		} `json:"matches"`
		Projection nativeRecallProjection `json:"native_context"`
	}
	if err := json.Unmarshal(raw, &exact); err != nil {
		t.Fatal(err)
	}
	if len(exact.Matches) != 1 || exact.Matches[0].ID != 9223372036854775807 ||
		len(exact.Projection.Reminders) != 1 || exact.Projection.Reminders[0] != "9223372036854775807" {
		t.Fatalf("rounded reminder identity: %s", raw)
	}
	if result := runPublicCommand(t, client, "prospective_mark_triggered", `{"id":"9223372036854775807"}`); result["status"] != "ok" {
		t.Fatal(result)
	}
	if result := runPublicCommand(t, client, "prospective_complete", `{"id":"9223372036854775807"}`); result["status"] != "ok" {
		t.Fatal(result)
	}

}
