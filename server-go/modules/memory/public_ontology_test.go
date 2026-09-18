package memory

import (
	"context"
	"fmt"
	"github.com/jackc/pgx/v5"
	"os"
	"strings"
	"testing"
)

func TestPublicOntologyValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	list := runPublicCommand(t, client, "ontology", `{}`)
	if list["status"] != "ok" || len(list["node_kinds"].([]any)) != 18 || len(list["relation_kinds"].([]any)) != 15 || len(list["schema_rules"].([]any)) != 18 {
		t.Fatal(list)
	}
	for _, field := range []string{"node_kinds", "relation_kinds"} {
		previous := -1
		for _, value := range list[field].([]any) {
			row := value.(map[string]any)
			id := int(row["id"].(float64))
			if id <= previous {
				t.Fatal("unordered labels", list)
			}
			previous = id
		}
	}
	if !strings.Contains(list["text"].(string), "99  other") {
		t.Fatal(list)
	}
	for _, args := range []string{`{"action":"bad"}`, `{"action":"walk"}`, `{"action":"walk","entity":"e","hops":-1}`, `{"action":"walk","entity":"e","hops":129}`, `{"action":"walk","entity":"e","hops":1.5}`, `{"action":"walk","entity":"e","relations":null}`, `{"action":"walk","entity":"e","relations":[4]}`, `{"action":"walk","entity":"e","relations":[""]}`} {
		if r := runPublicCommand(t, client, "ontology", args); r["kind"] != "invalid_argument" {
			t.Fatal(args, r)
		}
	}
	if r := runPublicCommand(t, client, "ontology", `{"action":"walk","entity":"e"}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}

func TestPublicOntologyWalkPostgres(t *testing.T) {
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
	sql := func(q string) {
		t.Helper()
		if _, err := tx.Exec(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	sql(`CREATE SCHEMA ontology_walk_test;
CREATE FUNCTION ontology_walk_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
SET LOCAL search_path TO pg_temp,ontology_walk_test,public;
CREATE TEMP TABLE memories(id bigint PRIMARY KEY,scope_type text,scope_value text,tier text DEFAULT 'L2',kind text DEFAULT 'fact',key text DEFAULT '',content text DEFAULT '',confidence float8 DEFAULT 1,lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0);
CREATE TEMP TABLE entity_edges(id bigint PRIMARY KEY,source text,relation text,target text,relation_id int DEFAULT 5,subject_kind int DEFAULT 1,object_kind int DEFAULT 1,weight int DEFAULT 10,edge_class text DEFAULT 'associative',suppressed int DEFAULT 0,superseded_at text DEFAULT '',invalidated_at text DEFAULT '',lifecycle_state text DEFAULT 'persistent',valid_from text DEFAULT '',valid_until text DEFAULT '',edge_origin text DEFAULT '',projection_generation_id bigint);
CREATE TEMP TABLE fact_evidence(assertion_id bigint,source_id text,source_kind text DEFAULT 'memory',invalidated_at text DEFAULT '',stance text DEFAULT 'supports');
CREATE TEMP TABLE projects(name text,lifecycle_state text DEFAULT 'current');
CREATE TEMP TABLE code_projection_generations(id bigint,project text,state text DEFAULT 'visible');
INSERT INTO memories(id,scope_type,scope_value) VALUES(1,'project','app'),(2,'project','private'),(3,'global','_global');
INSERT INTO memories(id,scope_type,scope_value,lifecycle_state) VALUES(4,'project','app','archived');
INSERT INTO memories(id,scope_type,scope_value,activation_suppressed) VALUES(5,'project','app',1);
INSERT INTO entity_edges(id,source,relation,target,weight) VALUES(1,'root','calls','b',100),(2,'b','calls','c',90),(3,'b','calls','root',80),(4,'root','fixes','d',95),(5,'root','co_discussed','e',70);
UPDATE entity_edges SET relation_id=2 WHERE id=4;
UPDATE entity_edges SET relation_id=NULL WHERE id=5;
INSERT INTO entity_edges(id,source,relation,target,weight,edge_class) SELECT i,'root','calls','hidden-'||i,1000+i,'semantic' FROM generate_series(6,12)i;
INSERT INTO fact_evidence(assertion_id,source_id) VALUES(6,'memory:2'),(7,'memory:4'),(11,'memory:1'),(12,'memory:5');
UPDATE fact_evidence SET stance='refutes' WHERE assertion_id=11;
UPDATE entity_edges SET suppressed=1 WHERE id=8;
UPDATE entity_edges SET invalidated_at='2020-01-01' WHERE id=9;
UPDATE entity_edges SET valid_from='9999-01-01' WHERE id=10;
INSERT INTO entity_edges(id,source,relation,target,weight,edge_origin,projection_generation_id) VALUES(13,'root','calls','retired-code',991,'code_projection',1),(14,'root','calls','current-code',89,'code_projection',2),(15,'root','calls','foreign-code',992,'code_projection',3);
INSERT INTO projects VALUES('app','current'),('private','current');
INSERT INTO code_projection_generations VALUES(1,'app','retired'),(2,'app','visible'),(3,'private','visible');
INSERT INTO entity_edges(id,source,relation,target,relation_id,weight,edge_class) VALUES(16,'root','works_for','company',2,88,'semantic'),(17,'root','calls','expired-evidence',5,999,'semantic'),(18,'root','calls','expired-fact',5,999,'semantic');
INSERT INTO fact_evidence(assertion_id,source_id,invalidated_at) VALUES(17,'memory:1','2020-01-01');
UPDATE entity_edges SET valid_until='2000-01-01' WHERE id=18;
INSERT INTO entity_edges(id,source,relation,target,weight) VALUES(19,'root','calls','shared',85);
INSERT INTO fact_evidence(assertion_id,source_id) VALUES(1,'memory:1'),(19,'memory:3');
INSERT INTO entity_edges(id,source,relation,target,weight) SELECT i,'root','calls','private-'||i,2000+i FROM generate_series(100,160)i;
INSERT INTO fact_evidence(assertion_id,source_id) SELECT i,'memory:2' FROM generate_series(100,160)i;
INSERT INTO entity_edges(id,source,relation,target,weight) SELECT 1000+i,'wide','calls','wide-'||i,100-i FROM generate_series(1,50)i;
INSERT INTO entity_edges(id,source,relation,target) SELECT 2000+i*3+j,'wide-'||i,'calls','leaf-'||i||'-'||j FROM generate_series(1,50)i CROSS JOIN generate_series(1,3)j;
CREATE ROLE memory_ontology_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA ontology_walk_test TO memory_ontology_test;
GRANT SELECT ON memories,entity_edges,fact_evidence,projects,code_projection_generations TO memory_ontology_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY visible ON memories USING(scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR (scope_type='project' AND scope_value=current_setting('aimee.memory_project',true)));
SET LOCAL ROLE memory_ontology_test;`)
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB})))
	run := func(entity, extra string) map[string]any {
		t.Helper()
		r := runPublicCommand(t, client, "ontology", fmt.Sprintf(`{"action":"walk","entity":%q,"scope_context":true,"project":"app"%s}`, entity, extra))
		if r["status"] != "ok" {
			t.Fatal(r)
		}
		return r
	}
	result := run("root", "")
	entries := result["entries"].([]any)
	if len(entries) != 7 {
		t.Fatalf("walk: %v", result)
	}
	for _, bad := range []string{"hidden-", "private-", "expired-", "retired-code", "foreign-code"} {
		if strings.Contains(result["text"].(string), bad) {
			t.Fatal(result)
		}
	}
	foundC, foundCompany := false, false
	for _, value := range entries {
		row := value.(map[string]any)
		switch row["target"] {
		case "c":
			foundC = true
			if row["hop"] != float64(2) {
				t.Fatal(row)
			}
		case "company":
			foundCompany = true
			if row["relation_kind"] != "other" {
				t.Fatal("semantic table id confused with graph enum", row)
			}
		}
	}
	if !foundC || !foundCompany {
		t.Fatal(result)
	}
	for _, tt := range []struct {
		entity, extra string
		count         int
	}{
		{"root", `,"hops":1`, 6}, {"root", `,"hops":0`, 0}, {"missing", "", 0}, {"root", `,"relations":[]`, 0},
		{"root", `,"relations":["fixes"]`, 1}, {"root", `,"relations":["worksFor"]`, 1}, {"root", `,"relations":["unknown"]`, 0}, {"wide", "", 128},
	} {
		if got := len(run(tt.entity, tt.extra)["entries"].([]any)); got != tt.count {
			t.Fatal(tt, got)
		}
	}
	if r := runPublicCommand(t, client, "ontology", `{"action":"walk","entity":"root","scope_context":true}`); strings.Contains(r["text"].(string), "current-code") {
		t.Fatal("missing scope admitted code", r)
	}
}

// Retains native ontology helper coverage at the enforcing Go owner.
func TestOntologyNames(t *testing.T) {
	for name, code := range map[string]int{"co_edited": 11, "co_discussed": 12, "fixes": 2, "depends_on": 0, "unknown_xyz": 99, "": 99} {
		if relationCode(name) != code {
			t.Fatal(name, relationCode(name))
		}
	}
	for code, name := range map[int]string{2: "fixes", 12: "co_discussed", 99: "other"} {
		if relationName(code) != name {
			t.Fatal(code, relationName(code))
		}
	}
	for name, code := range map[string]int{"file": 0, "commit": 5, "bogus": 99, "": 99} {
		if nodeCode(name) != code {
			t.Fatal(name, nodeCode(name))
		}
	}
}
