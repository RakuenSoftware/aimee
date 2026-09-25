package memory

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

func TestSubjectErasureTransitiveCopiesPostgres(t *testing.T) {
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
	exec(`SET LOCAL jit=off; SELECT set_config('aimee.memory_scope_all','1',true)`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start := strings.Index(string(schema), "CREATE OR REPLACE FUNCTION kb_subject_erasure_begin(")
	end := strings.Index(string(schema), "CREATE OR REPLACE FUNCTION kb_subject_erasure_complete(")
	if start < 0 || end < start {
		t.Fatal("erasure owner function missing")
	}
	exec(string(schema[start:end]))
	start = strings.Index(string(schema), "-- BEGIN memory erasure intents")
	end = strings.Index(string(schema), "-- END memory erasure intents")
	if start < 0 || end < start {
		t.Fatal("erasure intent migration missing")
	}
	exec(string(schema[start:end]))
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN CREATE ROLE aimee_kb_runtime NOLOGIN NOBYPASSRLS; END IF; IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_kb_owner') THEN CREATE ROLE aimee_kb_owner NOLOGIN; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_kb_runtime`)
	grants, err := os.ReadFile("../../../src/modules/db2/c/schema_grants.sql")
	if err != nil {
		t.Fatal(err)
	}
	start = strings.Index(string(grants), "DO $privacy_erasure_grants$")
	end = strings.Index(string(grants), "$privacy_erasure_grants$;")
	if start < 0 || end < start {
		t.Fatal("privacy grants missing")
	}
	exec(string(grants[start : end+len("$privacy_erasure_grants$;")]))
	ids := make([]int64, 4)
	for i, key := range []string{"erased source", "copied child", "copied grandchild", "unrelated survivor"} {
		owner := "mr04-unrelated"
		if i == 0 {
			owner = "mr04-erasure-subject"
		}
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,owner_principal,scope_type,scope_value)
 VALUES('L2','fact',$1,$1,$2,'project',$3) RETURNING id`, key, owner, key).Scan(&ids[i]); err != nil {
			t.Fatal(err)
		}
	}
	for i := 1; i < 3; i++ {
		exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 VALUES('memory',$1,'memory-cognify-input-v1',jsonb_build_object('record_id',$2::bigint::text)::text)`, ids[i], ids[i-1])
	}
	// Cycles must terminate without dropping a descendant from the erase set.
	exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 VALUES('memory',$1,'memory','memory:'||$2::bigint::text)`, ids[1], ids[2])
	exec(`INSERT INTO memory_units(memory_id,unit_type,unit_text) VALUES($1,'fact','copied descendant unit')`, ids[2])
	exec(`INSERT INTO artifacts(id,kind,operator_id,payload) VALUES('mr04-erasure-artifact','session_summary','kb.fold_session',jsonb_build_object('memory_id',$1::bigint,'content','copied descendant artifact'))`, ids[2])
	var copiedRule int64
	if err := tx.QueryRow(ctx, `INSERT INTO rules(polarity,title,description,domain,created_at,updated_at)
 VALUES('positive','erased generated guidance','copied descendant rule','memory-cognify',pg_now_text(),pg_now_text()) RETURNING id`).Scan(&copiedRule); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 VALUES('rule',$1,'memory-cognify-input-v1',jsonb_build_object('record_id',$2::bigint::text)::text)`, copiedRule, ids[2])
	var count, documents int64
	var replay bool
	erase := func() {
		t.Helper()
		exec(`SET LOCAL ROLE aimee_kb_runtime; SET LOCAL search_path=public,pg_catalog`)
		if err := tx.QueryRow(ctx, `SELECT * FROM kb_subject_erasure_begin('mr04-erasure-request-0001','mr04-erasure-subject','[]'::jsonb)`).Scan(&count, &documents, &replay); err != nil {
			t.Fatal(err)
		}
		exec(`RESET ROLE`)
	}
	erase()
	if count != 3 || replay {
		t.Fatal("transitive erasure count", count, replay)
	}
	var remaining int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE id=ANY($1::bigint[])`, ids[:3]).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatal("derived text retained", remaining, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM artifacts WHERE id='mr04-erasure-artifact'`).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatal("derived artifact retained", remaining, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE id=$1`, ids[3]).Scan(&remaining); err != nil || remaining != 1 {
		t.Fatal("unrelated memory erased", remaining, err)
	}

	if err := tx.QueryRow(ctx, `SELECT count(*) FROM rules WHERE id=$1`, copiedRule).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatal("derived rule retained", remaining, err)
	}

	for _, restore := range []struct {
		sql  string
		args []any
	}{
		{`INSERT INTO memories(id,key,content) VALUES($1,'restored id','changed payload')`, []any{ids[0]}},
		{`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref) VALUES('memory',$1,'memory','memory:'||$2::bigint::text)`, []any{ids[3], ids[0]}},
		{`INSERT INTO artifacts(id,kind,operator_id,payload) VALUES('late-artifact','session_summary','kb.fold_session',jsonb_build_object('memory_id',$1::bigint,'content','late payload'))`, []any{ids[0]}},
		{`INSERT INTO memories(key,content,scope_type,scope_value) VALUES('checkpoint_restore:999','copied grandchild','project','copied grandchild')`, nil},
	} {
		exec(`SAVEPOINT restore_attempt`)
		_, err := tx.Exec(ctx, restore.sql, restore.args...)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "23514" {
			t.Fatal("erased payload restored", err)
		}
		exec(`ROLLBACK TO restore_attempt; RELEASE restore_attempt`)
	}
	exec(`SAVEPOINT intent_acl; SET LOCAL ROLE aimee_kb_runtime`)
	if _, err := tx.Exec(ctx, `DELETE FROM memory_erasure_intents`); err == nil {
		t.Fatal("runtime cleared deletion intent")
	}
	exec(`ROLLBACK TO intent_acl; RELEASE intent_acl`)
	// A content-only dump loader can disable table triggers. Startup replay must
	// still suppress restored content using the independently retained control log.
	exec(`ALTER TABLE memories DISABLE TRIGGER memory_erasure_restore_guard`)
	exec(`INSERT INTO memories(id,key,content) VALUES($1,'old content snapshot','old payload')`, ids[0])
	exec(`ALTER TABLE memories ENABLE TRIGGER memory_erasure_restore_guard`)
	start = strings.Index(string(schema), "-- BEGIN memory erasure intents")
	end = strings.Index(string(schema), "-- END memory erasure intents")
	exec(string(schema[start:end]))
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE id=$1`, ids[0]).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatal("restore replay resurrected erased memory", remaining, err)
	}
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	if ok, err := backend.DeleteAs(ctx, ids[3], AuthorityUser); err != nil || !ok {
		t.Fatal("explicit user erase", ok, err)
	}
	var marker string
	if err := tx.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.memory_explicit_erasure',true),'')`).Scan(&marker); err != nil || marker != "" {
		t.Fatal("erasure marker escaped operation", marker, err)
	}
	exec(`SAVEPOINT explicit_restore`)
	_, err = tx.Exec(ctx, `INSERT INTO memories(key,content,scope_type,scope_value) VALUES('renamed snapshot','unrelated survivor','project','unrelated survivor')`)
	var explicitError *pgconn.PgError
	if !errors.As(err, &explicitError) || explicitError.Code != "23514" {
		t.Fatal("explicitly erased payload restored", err)
	}
	exec(`ROLLBACK TO explicit_restore; RELEASE explicit_restore`)
	erase()
	if count != 3 || !replay {
		t.Fatal("erasure replay changed result", count, replay)
	}
}
