package memory

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestCognifiedRuleInputsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
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
	exec(`SET LOCAL jit=off; SELECT set_config('aimee.memory_scope_all','1',true); DELETE FROM rules`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("runtime grants missing")
	}
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	exec(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	ids := make([]int64, 3)
	for i, key := range []string{"mr04-rule-root", "mr04-rule-claim", "mr04-rule-hidden"} {
		scope, value := "global", "_global"
		if i == 2 {
			scope, value = "project", "rule-hidden"
		}
		if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,scope_type,scope_value) VALUES($1,$1,$2,$3) RETURNING id`, key, scope, value).Scan(&ids[i]); err != nil {
			t.Fatal(err)
		}
	}
	bind := func(parent int64) {
		t.Helper()
		exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',child.id,'memory-cognify-input-v1',jsonb_build_object('schema_version',1,
 'owner_id',o.owner_id::text,'record_id',parent.id::text,'record_revision',parent.record_revision::text,
 'derived_revision',child.record_revision::text)::text FROM memories child,memories parent,memory_collection_owner o
 WHERE child.id=$1 AND parent.id=$2 AND o.id=1`, ids[1], parent)
	}
	bind(ids[0])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	if err := s.writeCognifyRule(ctx, ids[0], ids[1], "generated guidance", "Bound copied guidance"); err != nil {
		t.Fatal(err)
	}
	var rule int64
	if err := tx.QueryRow(ctx, `SELECT id FROM rules WHERE title='generated guidance'`).Scan(&rule); err != nil {
		t.Fatal(err)
	}
	check := func(want bool) {
		t.Helper()
		var got bool
		if err := tx.QueryRow(ctx, `SELECT (`+currentRuleInputsSQL("r.")+`) FROM rules r WHERE r.id=$1`, rule).Scan(&got); err != nil || got != want {
			t.Fatal("generated rule eligibility", got, want, err)
		}
		view, err := s.ruleView(ctx, DataRequest{Kind: "generate", Limit: 128})
		if err != nil || strings.Contains(string(view), "Bound copied guidance") != want {
			t.Fatal("generated rule markdown", string(view), want, err)
		}
		path := filepath.Join(t.TempDir(), "rules.jsonl")
		if _, err := s.exportCurrentRules(ctx, path); err != nil {
			t.Fatal(err)
		}
		data, err := os.ReadFile(path)
		if err != nil || strings.Contains(string(data), "Bound copied guidance") != want {
			t.Fatal("generated rule export", string(data), want, err)
		}
	}
	check(true)
	handler := NewHandler(nil, WithDataStore(PlacementKB, s))
	for _, operation := range []string{"rules-list", "rules-generate"} {
		out := runHostRuntime(t, handler, `{"operation":"`+operation+`","limit":1}`)
		raw, ok := out["json"].(string)
		if !ok || !strings.Contains(raw, "Bound copied guidance") {
			t.Fatal("runtime rules projection", operation, out)
		}
	}
	exportPath := filepath.Join(t.TempDir(), "runtime-rules.jsonl")
	exportArgs, _ := json.Marshal(map[string]any{"operation": "rules-export", "path": exportPath})
	exportReply := runHostRuntime(t, handler, string(exportArgs))
	if raw, ok := exportReply["json"].(string); !ok || !strings.Contains(raw, `"count":1`) {
		t.Fatal("runtime rules export", exportReply)
	}
	// Reinforcement and reviewed promotion preserve the unchanged claim's input
	// binding; they still change its canonical rule revision for cached release.
	exec(`UPDATE rules SET weight=95,directive_type='hard' WHERE id=$1`, rule)
	check(true)
	hard, _, err := s.recallHardRules(ctx, 8192)
	if err != nil || len(hard) != 1 {
		t.Fatal("promoted rule lost valid evidence", hard, err)
	}
	for _, change := range []string{
		`UPDATE rules SET description='changed unsupported payload' WHERE id=$1`,
		`DELETE FROM memory_lineage WHERE object_type='rule' AND object_id=$1`,
	} {
		exec(`SAVEPOINT changed_rule`)
		exec(change, rule)
		check(false)
		exec(`ROLLBACK TO changed_rule; RELEASE changed_rule`)
	}
	exec(`SAVEPOINT hidden_ancestor; RESET ROLE`)
	bind(ids[2])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	// This deliberately retains include_all=1: even a privileged extraction
	// caller cannot turn a scoped ancestor into globally published guidance.
	check(false)
	exec(`ROLLBACK TO hidden_ancestor; RELEASE hidden_ancestor`)
	exec(`SAVEPOINT revoked_ancestor; RESET ROLE`)
	exec(`UPDATE memories SET lifecycle_state='revoked' WHERE id=$1`, ids[0])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	check(false)
	hard, _, err = s.recallHardRules(ctx, 8192)
	if err != nil || len(hard) != 0 {
		t.Fatal("revoked promoted rule released", hard, err)
	}
	exec(`ROLLBACK TO revoked_ancestor; RELEASE revoked_ancestor`)
	// Both kinds of authored collision must retain their payload and ownership.
	for _, kind := range []string{"soft", "hard"} {
		title := "authored " + kind
		exec(`INSERT INTO rules(polarity,title,description,weight,directive_type,created_at,updated_at)
 VALUES('positive',$1,'authored guidance',1,$2,pg_now_text(),pg_now_text())`, title, kind)
		if err := s.writeCognifyRule(ctx, ids[0], ids[1], title, "model overwrite"); err != nil {
			t.Fatal(err)
		}
		var text string
		if err := tx.QueryRow(ctx, `SELECT description FROM rules WHERE title=$1`, title).Scan(&text); err != nil || text != "authored guidance" {
			t.Fatal("authored collision", text, err)
		}
	}
	// Filtering precedes the list limit, so a stale highest-weight derivative
	// cannot hide independently authored guidance behind it.
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET content='corrected source' WHERE id=$1`, ids[0])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	rows, err := s.currentRules(ctx, 0, 1, true)
	if err != nil || len(rows) != 1 || rows[0].Description != "authored guidance" {
		t.Fatal("rule limit applied before eligibility", rows, err)
	}
	view, err := s.ruleView(ctx, DataRequest{Kind: "list", Limit: 1})
	var envelope map[string]any
	if err != nil || json.Unmarshal(view, &envelope) != nil || envelope["status"] != "ok" {
		t.Fatal("rule list envelope", string(view), err)
	}
}
