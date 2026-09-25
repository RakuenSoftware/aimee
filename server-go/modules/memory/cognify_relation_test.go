package memory

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestCognifyRelationObservedInputsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
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
		if _, e := tx.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`SET LOCAL jit=off;
 CREATE ROLE cognify_relation_test;
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,scope_value text DEFAULT 'visible',lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '', scope_type text DEFAULT 'project');
 CREATE TEMP TABLE memory_relations(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,memory_id bigint,record_revision bigint DEFAULT 1,src_entity text,relation text,dst_entity text,fact_text text,invalid_at text DEFAULT '');
 CREATE TEMP TABLE memory_links(id bigint,source_id bigint,target_id bigint,relation text);
 CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
 CREATE TEMP TABLE rules(id bigint,record_revision bigint DEFAULT 1,domain text DEFAULT '',expires_at text DEFAULT '');
GRANT SELECT ON rules TO PUBLIC;
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
 CREATE TEMP TABLE memory_collection_owner(id int,owner_id text);
 INSERT INTO memory_collection_owner VALUES(1,'fixture-owner');
 INSERT INTO memories(id) VALUES(1),(2);
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY fixture_scope ON memories USING(scope_value='visible');
 GRANT SELECT ON memories,memory_links,memory_units,memory_collection_owner TO cognify_relation_test;
 GRANT SELECT,INSERT,UPDATE,DELETE ON memory_lineage,memory_relations TO cognify_relation_test;
 GRANT USAGE ON SEQUENCE memory_relations_id_seq TO cognify_relation_test`)
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	extracted := cognifyRelation{Subject: "subject", Relation: "uses", Object: "object", FactText: "observed claim"}
	write := func() {
		t.Helper()
		exec(`SET LOCAL ROLE cognify_relation_test`)
		err := backend.writeCognifyRelation(ctx, 1, extracted)
		exec(`RESET ROLE`)
		if err != nil {
			t.Fatal(err)
		}
	}
	check := func(want int) {
		t.Helper()
		exec(`SET LOCAL ROLE cognify_relation_test`)
		var n int
		err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations r WHERE `+currentRelationInputsSQL("r")).Scan(&n)
		exec(`RESET ROLE`)
		if err != nil || n != want {
			t.Fatalf("eligible relations %d, want %d: %v", n, want, err)
		}
	}
	write()
	check(1)
	for _, mutation := range []string{
		`DELETE FROM memory_lineage WHERE source_kind='memory-relation-input-v2'`,
		`UPDATE memory_lineage SET source_ref=jsonb_set(source_ref::jsonb,'{owner_id}','"different-owner"')::text WHERE source_kind='memory-relation-input-v2'`,
		`UPDATE memory_relations SET record_revision=2`,
		`UPDATE memories SET record_revision=2 WHERE id=1`,
		`UPDATE memories SET scope_value='hidden' WHERE id=1`,
		`UPDATE memories SET lifecycle_state='revoked' WHERE id=1`,
	} {
		exec(`SAVEPOINT changed_input`)
		exec(mutation)
		check(0)
		exec(`ROLLBACK TO changed_input; RELEASE changed_input`)
	}
	exec(`UPDATE memories SET record_revision=2 WHERE id=1`)
	check(0)
	write()
	check(1)
	var n int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations`).Scan(&n); err != nil || n != 1 {
		t.Fatal("refresh duplicated relation", n, err)
	}
	// The same observed source may itself require another memory.
	exec(`INSERT INTO memory_lineage VALUES('memory',1,'memory','memory:2'),('memory',1,'memory-cognify-input-v1',
 '{"schema_version":1,"owner_id":"fixture-owner","record_id":"2","record_revision":"1","derived_revision":"2"}')`)
	check(1)
	exec(`UPDATE memories SET lifecycle_state='revoked' WHERE id=2`)
	check(0)
	exec(`UPDATE memories SET lifecycle_state='active' WHERE id=2`)
	check(1)
	// An identical authored record is not adopted by model extraction.
	exec(`DELETE FROM memory_lineage WHERE object_type='relation'`)
	write()
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_lineage WHERE object_type='relation'`).Scan(&n); err != nil || n != 0 {
		t.Fatal("authored relation adopted", n, err)
	}
}

// Exercise the actual writer and runtime role against the migrated shared store.
func TestCognifyRelationProducerReplay(t *testing.T) {
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
	if _, err = tx.Exec(ctx, `SET LOCAL jit=off; DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("runtime grant migration missing")
	}
	if _, err = tx.Exec(ctx, string(schema[start:end+len("END\n$memory_store_grants$;")])); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, `SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	exerciseCognifyReplay(t, ctx, tx, backend)
}
