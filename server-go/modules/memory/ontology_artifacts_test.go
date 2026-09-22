package memory

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestOntologyArtifactsValidation(t *testing.T) {
	for _, change := range []func(*relTypeDef){
		func(r *relTypeDef) { r.Category = "bad\x01" }, func(r *relTypeDef) { r.HeadKinds = []NodeKind{100} },
		func(r *relTypeDef) { r.TailKinds = nil }, func(r *relTypeDef) { r.Correction = "bad" },
		func(r *relTypeDef) { r.Sensitivity = 99 }, func(r *relTypeDef) { r.Inverse = "missing" },
		func(r *relTypeDef) { r.Symmetric = true },
	} {
		row := seedOntology[0]
		change(&row)
		if _, err := ontologyArtifacts([]relTypeDef{row}); err == nil {
			t.Fatal("invalid seed accepted", row)
		}
	}
	for _, seed := range [][]relTypeDef{nil, {seedOntology[0], seedOntology[0]}} {
		if _, err := ontologyArtifacts(seed); err == nil {
			t.Fatal("empty/duplicate seed accepted")
		}
	}
	row := seedOntology[0]
	row.Category = `quote's "category"`
	artifact, err := ontologyArtifacts([]relTypeDef{row})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(artifact["sql"], `'quote''s "category"'`) || !strings.Contains(artifact["native"], `"quote's \"category\""`) {
		t.Fatal(artifact)
	}
}

func TestOntologySeedPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	text := string(schema)
	a := strings.Index(text, "CREATE TABLE IF NOT EXISTS rel_types (")
	if a < 0 {
		t.Fatal("missing seed table")
	}
	b := strings.Index(text[a:], ");") + a + 2
	ddl := strings.Replace(text[a:b], "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE", 1)
	start, end := "-- BEGIN GO MEMORY ONTOLOGY SEED\n", "-- END GO MEMORY ONTOLOGY SEED"
	a, b = strings.Index(text, start), strings.Index(text, end)
	if a < 0 || b <= a {
		t.Fatal("missing generated seed")
	}
	sql := text[a+len(start) : b]
	artifacts, err := OntologyArtifacts()
	if err != nil || sql != artifacts["sql"] {
		t.Fatal("packaged seed drift", err)
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
	run := func(q string) {
		t.Helper()
		if _, err := tx.Exec(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	run(ddl)
	run(sql)
	ids := map[string]int64{}
	for _, r := range seedOntology {
		var id int64
		var head, tail, inverse, correction, category, sensitivity, status string
		var symmetric, hierarchy int
		err := tx.QueryRow(ctx, `SELECT id,head_kinds,tail_kinds,is_symmetric,inverse_rel_type,correction_behavior,category,sensitivity,is_hierarchy_rel,status FROM rel_types WHERE rel_type=$1`, r.RelType).Scan(&id, &head, &tail, &symmetric, &inverse, &correction, &category, &sensitivity, &hierarchy, &status)
		names := func(list []NodeKind) string {
			out := []string{}
			for _, k := range list {
				out = append(out, nodeName(int(k)))
			}
			return strings.Join(out, ",")
		}
		if err != nil || head != names(r.HeadKinds) || tail != names(r.TailKinds) || (symmetric == 1) != r.Symmetric || inverse != r.Inverse || correction != r.Correction || category != r.Category || sensitivity != []string{"normal", "pii", "secret"}[r.Sensitivity] || (hierarchy == 1) != r.Hierarchy || status != "active" {
			t.Fatalf("%s fields: %s %s %d %s %s %s %s %d %s; %v", r.RelType, head, tail, symmetric, inverse, correction, category, sensitivity, hierarchy, status, err)
		}
		ids[r.RelType] = id
	}
	run(sql)
	for name, id := range ids {
		var actual int64
		if err := tx.QueryRow(ctx, `SELECT id FROM rel_types WHERE rel_type=$1`, name).Scan(&actual); err != nil || actual != id {
			t.Fatal("replay changed identity", name, actual, id, err)
		}
	}
	run(`UPDATE rel_types SET category='operator',sensitivity='secret',head_kinds='device' WHERE rel_type='works_for'; DELETE FROM rel_types WHERE rel_type='born_in'; INSERT INTO rel_types(rel_type,category) VALUES('operator_relation','operator')`)
	run(sql)
	var category, sensitivity, head string
	if err := tx.QueryRow(ctx, `SELECT category,sensitivity,head_kinds FROM rel_types WHERE rel_type='works_for'`).Scan(&category, &sensitivity, &head); err != nil || category != "operator" || sensitivity != "secret" || head != "device" {
		t.Fatal("upgrade replaced operator data", category, sensitivity, head, err)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM rel_types`).Scan(&count); err != nil || count != len(seedOntology)+1 {
		t.Fatal("upgrade lost rows", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT head_kinds FROM rel_types WHERE rel_type='born_in'`).Scan(&head); err != nil || head != "person" {
		t.Fatal("missing seed not repaired", head, err)
	}
}

func TestSeedCorrectionPolicies(t *testing.T) {
	if seedLookup("also_known_as").Correction != "hard_delete" || seedLookup("born_in").Correction != "immutable" || seedLookup("works_for").Correction != "supersede" {
		t.Fatal("persisted correction behavior changed")
	}
}
