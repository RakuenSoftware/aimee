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

func TestMemoryEvidenceProjectionPostgres(t *testing.T) {
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
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SET LOCAL jit=off; CREATE ROLE evidence_projection_test;
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,scope_value text DEFAULT 'visible',lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '');
 CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
 CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
 CREATE TEMP TABLE memory_collection_owner(id int,owner_id text);
 CREATE TEMP TABLE memory_collection_generations(scope_type text,scope_value text,generation bigint);
 CREATE TEMP TABLE memory_projection_generations(scope_type text,scope_value text,memory_id bigint,generation bigint);
 INSERT INTO memory_collection_generations VALUES('project','visible',10),('project','hidden',1000);
 INSERT INTO memory_projection_generations VALUES('project','visible',1,2),('project','hidden',9007199254740993,2000);
 CREATE TEMP TABLE memory_evidence_events(event_id text,object_kind text,object_id text,operation text);
 INSERT INTO memory_collection_owner VALUES(1,'fixture-owner');
 INSERT INTO memories(id) SELECT generate_series(1,32);
 INSERT INTO memories(id,scope_value) VALUES(9007199254740993,'hidden');
 INSERT INTO memory_evidence_events SELECT 'host-event-'||id,'memory',id::text,'assert' FROM memories;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY fixture_scope ON memories USING(scope_value='visible');
 GRANT SELECT ON memories,memory_units,memory_lineage,memory_collection_owner,memory_collection_generations,memory_projection_generations,memory_evidence_events TO evidence_projection_test`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start := strings.Index(string(schema), "CREATE OR REPLACE FUNCTION memory_row_scope_visible(")
	end := strings.Index(string(schema)[start:], "ALTER TABLE memories ENABLE ROW LEVEL SECURITY;")
	if start < 0 || end < 0 {
		t.Fatal("scope owner missing")
	}
	exec(string(schema)[start : start+end])
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project','visible',true)`)
	parent := func(child, source int64) {
		exec(`INSERT INTO memory_lineage VALUES('memory',$1,'memory','memory:'||$2::bigint::text),
 ('memory',$1,'memory-cognify-input-v1',jsonb_build_object('schema_version',1,'owner_id','fixture-owner','record_id',$2::bigint::text,'record_revision','1','derived_revision','1')::text)`, child, source)
	}
	for i := int64(2); i <= 31; i++ {
		parent(i, 1)
	}
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	read := func(id int64) map[string]any {
		t.Helper()
		exec(`SET LOCAL ROLE evidence_projection_test`)
		p, err := s.memoryEvidence(ctx, id)
		exec(`RESET ROLE`)
		if err != nil {
			t.Fatal(err)
		}
		return p
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	exec(`SET LOCAL ROLE evidence_projection_test`)
	public := runPublicCommand(t, client, "evidence", `{"id":"1","scope_context":true,"project":"visible","view":"server"}`)
	exec(`RESET ROLE`)
	if public["status"] != "ok" || public["store"] != "kb" || public["record_id"] != "1" {
		t.Fatal("public evidence envelope", public)
	}
	original := read(1)
	if original["lineage_generation"] != json.Number("13") || original["generation_state"] != "observed" {
		t.Fatal("scoped lineage generation", original)
	}
	if original["source_family_count"] != 1 || original["independence_state"] != "unknown" {
		t.Fatal(original)
	}
	for i := int64(2); i <= 31; i++ {
		p := read(i)
		if p["source_family_count"] != 1 || p["independence_state"] != "unknown" || fmt.Sprint(p["origin_families"]) != fmt.Sprint(original["origin_families"]) {
			t.Fatalf("copy %d changed origin: %v", i, p)
		}
	}
	parent(32, 1)
	parent(32, 9007199254740993)
	hidden := read(32)
	raw, _ := json.Marshal(hidden)
	if strings.Contains(string(raw), "9007199254740993") || hidden["source_family_count"] != nil || hidden["support_count"] != nil || hidden["lineage_state"] != "partial" {
		t.Fatal("hidden ancestry escaped", string(raw))
	}
	exec(`DELETE FROM memories WHERE id=9007199254740993`)
	missing := read(32)
	other, _ := json.Marshal(missing)
	if string(other) != string(raw) {
		t.Fatalf("hidden and missing differ: %s / %s", raw, other)
	}
	exec(`SAVEPOINT no_creation_event; DELETE FROM memory_evidence_events WHERE object_id='1'`)
	unknown := read(2)
	if unknown["source_family_count"] != 0 || unknown["independence_state"] != "unknown" {
		t.Fatal("missing event minted an origin", unknown)
	}
	exec(`ROLLBACK TO no_creation_event; RELEASE no_creation_event`)
	for _, observation := range []struct{ kind, ref string }{
		{"document", "document:unresolved"},
		{"memory-cognify-input-v1", strings.Repeat("x", 8193)},
	} {
		exec(`SAVEPOINT unresolved_input`)
		exec(`INSERT INTO memory_lineage VALUES('memory',1,$1,$2)`, observation.kind, observation.ref)
		p := read(2)
		if p["source_family_count"] != nil || p["lineage_state"] != "partial" {
			t.Fatal("unresolved input certified", p)
		}
		exec(`ROLLBACK TO unresolved_input; RELEASE unresolved_input`)
	}
	parent(1, 2)
	cycle := read(2)
	if cycle["lineage_state"] != "partial" || cycle["independence_state"] != "unknown" {
		t.Fatal("cycle became complete", cycle)
	}
}

func TestMemoryEvidencePublicValidation(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		for _, args := range []string{`{}`, `{"id":0}`, `{"id":1.5}`, `{"id":9007199254740993}`, `{"id":1,"independent_support_count":999}`, `{"id":1,"project":null}`, `{"id":1,"scope":null}`, `{"id":1,"include_all":"yes"}`, `{"id":1,"view":"forged"}`} {
			if result := runPublicCommand(t, client, "evidence", args); result["kind"] != "invalid_argument" {
				t.Fatal(placement, args, result)
			}
		}
		if result := runPublicCommand(t, client, "evidence", `{"id":"9007199254740993"}`); result["kind"] != "unavailable" {
			t.Fatal("exact string identity did not reach owner", placement, result)
		}
	}
}
