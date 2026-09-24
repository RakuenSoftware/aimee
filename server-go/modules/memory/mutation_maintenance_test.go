package memory

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

// Exercise automatic writers under the packaged runtime grants, not the owner.
func TestMutationMaintenanceAuthority(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("requires packaged PostgreSQL")
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("missing runtime grants")
	}
	exec(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	exec(`SET LOCAL ROLE aimee_store_runtime; SELECT set_config('aimee.memory_scope_all','1',true)`)
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	for _, origin := range []string{"agent_message", "user_stated", "unknown"} {
		for _, kind := range []string{"world_fact", "episode", "experience", "instruction", "policy"} {
			key := "mr02-maintenance-" + origin + "-" + kind
			exec(`INSERT INTO memories(tier,kind,epistemic_kind,key,content,provenance_category,updated_at,source_session) VALUES('L0','fact',$1,$2,'retained evidence',$3,pg_now_text('-100 days'),$2)`, kind, key, origin)
		}
	}
	// A compatibility SQL writer must not erase a protected kind first and
	// then use an ordinary content update to evade the immutable-content guard.
	for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
		nested, err := tx.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		_, err = nested.Exec(ctx, `UPDATE memories SET epistemic_kind='world_fact' WHERE key=$1`, "mr02-maintenance-agent_message-"+kind)
		_ = nested.Rollback(ctx)
		if err == nil {
			t.Fatal("storage allowed protection downgrade", kind)
		}
	}
	if _, err := backend.RunMaintenance(ctx, MaintenanceReplay, true, false); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := backend.Maintenance(ctx, Scope{Type: ScopeGlobal, Value: "_global"}); err != nil {
		t.Fatal(err)
	}
	var changed int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key LIKE 'mr02-maintenance-%' AND lifecycle_state<>'active' AND (provenance_category<>'agent_message' OR epistemic_kind IN ('episode','experience','instruction','policy'))`).Scan(&changed); err != nil || changed != 0 {
		t.Fatalf("automatic maintenance altered %d protected rows: %v", changed, err)
	}
	var retired int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='mr02-maintenance-agent_message-world_fact' AND lifecycle_state='retired'`).Scan(&retired); err != nil || retired != 1 {
		t.Fatal("eligible model maintenance stopped", retired, err)
	}
	for _, origin := range []string{"agent_message", "user_stated", "unknown"} {
		for _, kind := range []string{"world_fact", "episode", "experience", "instruction", "policy"} {
			if origin == "agent_message" && kind == "world_fact" {
				continue
			}
			var id int64
			if err := tx.QueryRow(ctx, `SELECT id FROM memories WHERE key=$1`, "mr02-maintenance-"+origin+"-"+kind).Scan(&id); err != nil {
				t.Fatal(err)
			}
			if changed, err := backend.LifecycleTransition(ctx, id, "archived", "automatic"); err != nil || changed {
				t.Fatal("lifecycle bypassed admission", origin, kind, changed, err)
			}
			if changed, err := backend.LifecyclePending(ctx, id, 1); err != nil || changed {
				t.Fatal("pending bypassed admission", origin, kind, changed, err)
			}
		}
	}

	for _, origin := range []string{"agent_message", "user_stated", "unknown"} {
		for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
			key := "mr02-fold-" + origin + "-" + kind
			exec(`INSERT INTO memories(tier,kind,epistemic_kind,key,content,provenance_category,source_session) VALUES('L0','fact',$1,$2,'protected source',$3,$2)`, kind, key, origin)
			n, _, err := backend.FoldSession(ctx, key)
			if err != nil || n != -1 {
				t.Fatalf("fold destroyed protected %s: %d %v", key, n, err)
			}
		}
	}
	for _, origin := range []string{"user_stated", "unknown"} {
		key := "mr02-fold-" + origin
		exec(`INSERT INTO memories(tier,kind,key,content,provenance_category,source_session) VALUES('L0','scratch',$1,'authoritative source',$2,$1)`, key, origin)
		n, _, err := backend.FoldSession(ctx, key)
		if err != nil || n != -1 {
			t.Fatalf("fold destroyed authoritative source: %d %v", n, err)
		}
	}
}

func TestImportEpistemicKindValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, kind := range []string{`null`, `42`, `{}`, `""`, `"invented"`} {
		r := runPublicCommand(t, client, "store", `{"key":"imported","content":"policy text","epistemic_kind":`+kind+`}`)
		if r["kind"] != "invalid_argument" {
			t.Fatal("import kind silently downgraded", kind, r)
		}
	}
}
