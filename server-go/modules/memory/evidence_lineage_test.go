package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/jackc/pgx/v5"
	"os"
	"reflect"
	"slices"
	"testing"
)

func lineageRoot(family, witness string) lineageNode {
	return lineageNode{Revision: "1", Complete: true, Origin: &lineageOrigin{Family: family, IndependenceSet: "verified-fixture-v1", Witness: witness}}
}
func lineageCopy(parents ...string) lineageNode {
	n := lineageNode{Revision: "1", Complete: true}
	for _, p := range parents {
		n.Edges = append(n.Edges, lineageEdge{Parent: p, Revision: "1", Claim: "claim", Relation: "derived_from"})
	}
	return n
}
func requireLineageCount(t *testing.T, p lineageSupport, count int, state string) {
	t.Helper()
	if p.IndependentSupportCount == nil || *p.IndependentSupportCount != count || p.IndependenceState != state {
		t.Fatalf("want %d/%s: %+v", count, state, p)
	}
}
func TestEvidenceLineageCopiesAndComposites(t *testing.T) {
	nodes := map[string]lineageNode{"a": lineageRoot("origin-a", "witness-a"), "b": lineageRoot("origin-b", "witness-b")}
	ids := []string{"a"}
	for i := 0; i < 30; i++ {
		id := fmt.Sprintf("copy-%02d", i)
		nodes[id] = lineageCopy("a")
		nodes[id+"-summary"] = lineageCopy(id)
		ids = append(ids, id, id+"-summary")
	}
	p := projectLineage("claim", ids, nodes, 256, 16)
	requireLineageCount(t, p, 1, "complete")
	if p.SupportCount != 61 || p.SourceFamilyCount != 1 || p.UnknownOriginCount != 0 {
		t.Fatal(p)
	}
	nodes["ab"] = lineageCopy("a", "b")
	ids = append(ids, "b", "ab", "ab")
	p = projectLineage("claim", ids, nodes, 256, 16)
	requireLineageCount(t, p, 2, "complete")
	if p.SupportCount != 63 || p.SourceFamilyCount != 2 {
		t.Fatal(p)
	}
	before, _ := json.Marshal(p)
	slices.Reverse(ids)
	after, _ := json.Marshal(projectLineage("claim", ids, nodes, 256, 16))
	if string(before) != string(after) {
		t.Fatalf("order changed projection: %s / %s", before, after)
	}
}
func TestEvidenceLineageUnknownAndClaimBoundaries(t *testing.T) {
	nodes := map[string]lineageNode{"a": lineageRoot("a", "a"), "unknown": {Revision: "1", Complete: true}, "copy": lineageCopy("a")}
	p := projectLineage("claim", []string{"a", "unknown"}, nodes, 32, 8)
	requireLineageCount(t, p, 1, "partial")
	if p.UnknownOriginCount != 1 || p.LineageState != "partial" {
		t.Fatal(p)
	}
	p = projectLineage("claim", []string{"unknown"}, nodes, 32, 8)
	if p.IndependentSupportCount != nil || p.IndependenceState != "unknown" {
		t.Fatal(p)
	}
	p = projectLineage("different-claim", []string{"copy"}, nodes, 32, 8)
	if p.SourceFamilyCount != 0 || p.IndependentSupportCount != nil {
		t.Fatalf("document supports an undeclared claim: %+v", p)
	}
	for _, rel := range []string{"related_to", "corrects", "contradicts", "invented"} {
		n := lineageCopy("a")
		n.Edges[0].Relation = rel
		nodes["copy"] = n
		p = projectLineage("claim", []string{"copy"}, nodes, 32, 8)
		if p.SourceFamilyCount != 0 || p.IndependentSupportCount != nil {
			t.Fatalf("%s became supporting evidence: %+v", rel, p)
		}
	}
	nodes["b"] = lineageRoot("b", "b")
	n := nodes["b"]
	n.Edges = []lineageEdge{{Parent: "a", Revision: "1", Claim: "claim", Relation: "related_to"}}
	nodes["b"] = n
	requireLineageCount(t, projectLineage("claim", []string{"a", "b"}, nodes, 32, 8), 2, "complete")
}
func TestEvidenceLineageIncompleteTraversal(t *testing.T) {
	for _, tc := range []struct {
		name         string
		nodes        map[string]lineageNode
		limit, depth int
		reason       string
	}{
		{"missing", map[string]lineageNode{"x": lineageCopy("absent")}, 32, 8, "missing"},
		{"cycle", map[string]lineageNode{"x": lineageCopy("y"), "y": lineageCopy("x")}, 32, 8, "cycle"},
		{"depth", map[string]lineageNode{"x": lineageCopy("y"), "y": lineageCopy("a"), "a": lineageRoot("a", "a")}, 32, 1, "truncated"},
		{"nodes", map[string]lineageNode{"x": lineageCopy("a"), "a": lineageRoot("a", "a")}, 1, 8, "truncated"},
		{"version", map[string]lineageNode{"x": lineageCopy("a"), "a": {Revision: "2", Complete: true, Origin: &lineageOrigin{Family: "a"}}}, 32, 8, "stale_version"},
		{"undeclared", map[string]lineageNode{"x": {Revision: "1", Origin: &lineageOrigin{Family: "a", IndependenceSet: "fixture", Witness: "a"}}}, 32, 8, "undeclared_inputs"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := projectLineage("claim", []string{"x"}, tc.nodes, tc.limit, tc.depth)
			if p.IndependentSupportCount != nil || p.LineageState != "partial" || !slices.Contains(p.Reasons, tc.reason) {
				t.Fatal(p)
			}
		})
	}
}
func TestEvidenceLineageIndependenceRequiresCertificate(t *testing.T) {
	a, b := lineageRoot("different-url-a", "a"), lineageRoot("different-url-b", "b")
	a.Origin.IndependenceSet = ""
	b.Origin.IndependenceSet = ""
	p := projectLineage("claim", []string{"a", "b"}, map[string]lineageNode{"a": a, "b": b}, 32, 8)
	if p.SourceFamilyCount != 2 || p.IndependentSupportCount != nil || p.LineageState != "complete" {
		t.Fatal(p)
	}
	a, b = lineageRoot("a", "a"), lineageRoot("b", "b")
	b.Origin.IndependenceSet = "unrelated-verification"
	requireLineageCount(t, projectLineage("claim", []string{"a", "b"}, map[string]lineageNode{"a": a, "b": b}, 32, 8), 1, "partial")
	b.Origin.IndependenceSet = a.Origin.IndependenceSet
	b.Origin.Witness = a.Origin.Witness
	requireLineageCount(t, projectLineage("claim", []string{"a", "b"}, map[string]lineageNode{"a": a, "b": b}, 32, 8), 1, "complete")
}
func TestEvidenceLineageSharedFixtureAndConflicts(t *testing.T) {
	a, b, c := lineageRoot("a", "a"), lineageRoot("b", "b"), lineageRoot("c", "c")
	a.Origin.CommonDependencies = []string{"fixture"}
	b.Origin.CommonDependencies = []string{"fixture", "upstream"}
	c.Origin.CommonDependencies = []string{"upstream"}
	nodes := map[string]lineageNode{"a": a, "b": b, "c": c, "composite": lineageCopy("a", "b", "c")}
	p := projectLineage("claim", []string{"a", "b", "c", "composite"}, nodes, 32, 8)
	requireLineageCount(t, p, 1, "complete")
	if p.SourceFamilyCount != 3 {
		t.Fatal(p)
	}
	b.Origin.Family = "a"
	nodes["b"] = b
	p = projectLineage("claim", []string{"composite"}, nodes, 32, 8)
	if !slices.Contains(p.Reasons, "conflicting_origin_certificate") || p.IndependenceState == "complete" {
		t.Fatal(p)
	}
	if !reflect.DeepEqual(nodes["a"].Origin.CommonDependencies, []string{"fixture"}) {
		t.Fatal("mutated canonical origin")
	}
}

// This exercises the same SQL predicate used before retrieval limits and source
// release checks, under a non-owner role, with a three-generation derivative.
func TestEvidenceLineageCurrentReleasePostgres(t *testing.T) {
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
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SET LOCAL jit=off; CREATE ROLE memory_lineage_test;
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,scope_value text DEFAULT 'visible',lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '', scope_type text DEFAULT 'project');
 CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
 CREATE TEMP TABLE rules(id bigint,record_revision bigint DEFAULT 1,domain text DEFAULT '',expires_at text DEFAULT '');
GRANT SELECT ON rules TO PUBLIC;
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
 CREATE TEMP TABLE memory_collection_owner(id int,owner_id text);
 INSERT INTO memory_collection_owner VALUES(1,'fixture-owner');
 INSERT INTO memories(id) VALUES(1),(2),(3);
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY fixture_scope ON memories USING(scope_value='visible');
 GRANT SELECT ON memories,memory_units,memory_lineage,memory_collection_owner TO memory_lineage_test`)
	for _, pair := range [][2]int{{2, 1}, {3, 2}} {
		exec(`INSERT INTO memory_lineage VALUES('memory',$1,'memory','memory:'||$2::bigint::text),
 ('memory',$1,'memory-cognify-input-v1',jsonb_build_object('schema_version',1,'owner_id','fixture-owner','record_id',$2::bigint::text,'record_revision','1','derived_revision','1')::text)`, pair[0], pair[1])
	}
	check := func(want bool) {
		t.Helper()
		exec(`SET LOCAL ROLE memory_lineage_test`)
		var ok bool
		err := tx.QueryRow(ctx, `SELECT `+currentMemorySQL("m.")+` FROM memories m WHERE id=3`).Scan(&ok)
		exec(`RESET ROLE`)
		if err != nil || ok != want {
			t.Fatalf("transitive current release want %v: %v %v", want, ok, err)
		}
	}
	check(true)
	for _, change := range []string{"lifecycle_state='revoked'", "lifecycle_state='deleted'", "scope_value='hidden'", "record_revision=2", "valid_until='2000-01-01'"} {
		exec(`SAVEPOINT ancestor_change`)
		exec(`UPDATE memories SET ` + change + ` WHERE id=1`)
		check(false)
		exec(`ROLLBACK TO ancestor_change; RELEASE ancestor_change`)
	}
	for _, change := range []string{
		`DELETE FROM memory_lineage WHERE object_id=2 AND source_kind='memory-cognify-input-v1'`,
		`UPDATE memory_lineage SET source_ref=jsonb_set(source_ref::jsonb,'{record_id}','"3"')::text WHERE object_id=2 AND source_kind='memory-cognify-input-v1'`,
		`UPDATE memory_lineage SET source_ref=jsonb_set(source_ref::jsonb,'{derived_revision}','"2"')::text WHERE object_id=2 AND source_kind='memory-cognify-input-v1'`,
		`UPDATE memory_lineage SET source_ref=jsonb_set(source_ref::jsonb,'{owner_id}','"other-owner"')::text WHERE object_id=2 AND source_kind='memory-cognify-input-v1'`,
	} {
		exec(`SAVEPOINT broken_lineage`)
		exec(change)
		check(false)
		exec(`ROLLBACK TO broken_lineage; RELEASE broken_lineage`)
	}
	exec(`DELETE FROM memories WHERE id=1`)
	check(false)
}
