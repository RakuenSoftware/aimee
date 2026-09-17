package memory

import (
	"context"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestMaintenancePublicPostgres(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA maintenance_test;
 CREATE FUNCTION maintenance_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 CREATE FUNCTION maintenance_test.pg_now_text(shift text) RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
 SET LOCAL search_path TO pg_temp,maintenance_test,public;
 CREATE TEMP TABLE kb_meta(key text PRIMARY KEY,value text);
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,tier text,kind text,key text,lifecycle_state text DEFAULT 'active',artifact_ref text DEFAULT '',artifact_hash text DEFAULT '',confidence double precision DEFAULT 0.8,use_count bigint DEFAULT 0,
 updated_at text DEFAULT pg_now_text(),activation_suppressed bigint DEFAULT 0,valid_until text DEFAULT '');
 CREATE TEMP TABLE memory_provenance(memory_id bigint);
 CREATE TEMP TABLE memory_links(source_id bigint,target_id bigint);
 CREATE TEMP TABLE memory_scopes(memory_id bigint);
 CREATE TEMP TABLE memory_entities(memory_id bigint);
 CREATE TEMP TABLE memory_relations(memory_id bigint);
 INSERT INTO memories(id,tier,kind,key,artifact_ref) VALUES(1,'L0','fact','',''),(2,'L2','preference','artifact','file');`)
	if err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB, settings: func() (map[string]any, error) {
		return map[string]any{"memory_maintenance_enabled": true, "memory_maintenance_interval_seconds": float64(600), "memory_maintenance_summarize_enabled": true}, nil
	}}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	client := clientForHandler(t, handler)
	before := runHostRuntime(t, handler, `{"operation":"maintenance-dashboard"}`)
	if before["last"] != nil || before["config"].(map[string]any)["interval_seconds"] != float64(600) {
		t.Fatal(before)
	}
	beforeMetrics := before["metrics"].(map[string]any)
	lint := runPublicCommand(t, client, "lint", `{}`)
	if lint["issue_count"] != float64(3) || len(lint["issues"].([]any)) != 3 {
		t.Fatal(lint)
	}
	for _, issue := range lint["issues"].([]any) {
		if _, ok := issue.(map[string]any)["key"]; !ok {
			t.Fatal("empty key omitted from public lint envelope")
		}
	}
	dry := runPublicCommand(t, client, "maintenance_run", `{"dry_run":true}`)["summary"].(map[string]any)
	if dry["dry_run"] != true || dry["memory_count_before"] != float64(2) || dry["memory_count_after"] != float64(2) {
		t.Fatal(dry)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM kb_meta`).Scan(&count); err != nil || count != 0 {
		t.Fatal("dry run changed the maintenance schedule", count, err)
	}
	// A replay cycle with no eligible changes still persists its report. A second
	// ordinary call is throttled; force and dry-run retain distinct meanings.
	first := runPublicCommand(t, client, "maintenance_run", `{"modes":1}`)["summary"].(map[string]any)
	if first["skipped"] != false || first["dry_run"] != false {
		t.Fatal(first)
	}
	if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM kb_meta`).Scan(&count); err != nil || count != 2 {
		t.Fatal(count, err)
	}
	second := runPublicCommand(t, client, "maintenance_run", `{"modes":1}`)["summary"].(map[string]any)
	if second["skipped"] != true {
		t.Fatal("recent cycle not throttled", second)
	}
	forced := runPublicCommand(t, client, "maintenance_run", `{"modes":1,"force":true,"dry_run":true}`)["summary"].(map[string]any)
	if forced["skipped"] != false || forced["dry_run"] != true {
		t.Fatal(forced)
	}
	dashboard := runHostRuntime(t, handler, `{"operation":"maintenance-dashboard"}`)
	last := dashboard["last"].(map[string]any)
	if last["modes_run"] != float64(1) || last["dry_run"] != false || last["skipped"] != false {
		t.Fatal(dashboard)
	}
	metrics := dashboard["metrics"].(map[string]any)
	if metrics["runs_total"].(float64)-beforeMetrics["runs_total"].(float64) != 3 || metrics["skips_total"].(float64)-beforeMetrics["skips_total"].(float64) != 1 || metrics["ms_max"].(float64) < 0 {
		t.Fatal(metrics)
	}
	_, err = tx.Exec(ctx, `TRUNCATE memories`)
	if err != nil {
		t.Fatal(err)
	}
	lint = runPublicCommand(t, client, "lint", `{}`)
	if lint["issue_count"] != float64(0) || len(lint["issues"].([]any)) != 0 {
		t.Fatal(lint)
	}
}
