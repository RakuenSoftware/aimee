package memory

import (
	"context"
	"encoding/json"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestTypedEpisodeSourceProjection(t *testing.T) {
	const id = "9007199254743001"
	const owner = "00000000-0000-0000-0000-000000000001"
	r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{"enable_episodes":true}`)})
	r.add("episodes", typedItem{id: id, text: "episode", value: map[string]string{"stable_id": id, "excerpt": "episode"}, source: &typedSourceVersion{
		Kind: "memory_episode", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: id, RecordRevision: "7"},
		MemoryParentState: "observed", MemoryParents: []MemoryRecordVersion{{SchemaVersion: 1, OwnerID: owner, RecordID: "9007199254743000", RecordRevision: "3"}},
	}})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	encode := func() string {
		raw, err := json.Marshal(r)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	decoded, err := decodeTypedProjection(encode())
	if err != nil || len(decoded.Retained) != 1 || decoded.SourceVersionState != "record_versions_observed" || decoded.Retained[0].Source.Version.RecordID != id || decoded.Retained[0].Source.MemoryParents[0].RecordRevision != "3" {
		t.Fatal("episode versions lost through host round trip", decoded, err)
	}
	if err := decoded.fitProjectionBytes(len(r.Rendered)); err != nil || decoded.SelectionDigest != r.SelectionDigest {
		t.Fatal("retained episode binding changed", err)
	}
	if err := decoded.fitProjectionBytes(0); err != nil || len(decoded.Retained) != 0 || decoded.SourceVersionState != "unavailable" {
		t.Fatal("dropped episode retains version claim", err)
	}
	for _, mutate := range []func(*typedProjectionRef){
		func(ref *typedProjectionRef) { ref.Channel = "current_assertions" },
		func(ref *typedProjectionRef) { ref.Source.MemoryParentState = "unavailable" },
		func(ref *typedProjectionRef) { ref.Source.MemoryParents = nil },
		func(ref *typedProjectionRef) {
			ref.Source.MemoryParents = append(ref.Source.MemoryParents, ref.Source.MemoryParents[0])
		},
		func(ref *typedProjectionRef) {
			ref.Source.MemoryParents[0].OwnerID = "00000000-0000-0000-0000-000000000002"
		},
	} {
		copy, err := decodeTypedProjection(encode())
		if err != nil {
			t.Fatal(err)
		}
		mutate(&copy.Retained[0])
		if validTypedSource(copy.Retained[0]) {
			t.Fatal("invalid episode provenance accepted", copy.Retained)
		}
	}
	ref := r.Retained[0]
	if validTypedSourceItem(ref, json.RawMessage(`{"stable_id":"9007199254743000"}`)) {
		t.Fatal("episode metadata attached to different record")
	}
	r.Retained[0].Source.Version.RecordRevision = "8"
	if _, err := decodeTypedProjection(encode()); err == nil {
		t.Fatal("episode revision changed under old selection commitment")
	}
}

func TestTypedSourceVersionBinding(t *testing.T) {
	const id = "9007199254742999"
	makeProjection := func(owner string) *typedContextResult {
		t.Helper()
		h := assertionHit{ID: 9007199254742999, StableID: id, Version: 7, ownerID: owner,
			Subject: "service", Relation: "uses", Object: "cache", Rendered: "service uses cache"}
		r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
		r.add("current_assertions", typedItem{id: id, text: h.Rendered, value: h, source: h.sourceVersion()})
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		return r
	}
	r := makeProjection("00000000-0000-0000-0000-000000000001")
	if len(r.Retained) != 1 || r.SourceVersionState != "record_versions_observed" ||
		r.Retained[0].Source.Version.RecordID != id || r.Retained[0].Source.Version.RecordRevision != "7" {
		t.Fatal("exact owner version missing", r.Retained, r.SourceVersionState)
	}
	rotated := makeProjection("00000000-0000-0000-0000-000000000002")
	if r.Rendered != rotated.Rendered || r.ProjectionDigest != rotated.ProjectionDigest || r.SelectionDigest == rotated.SelectionDigest {
		t.Fatal("owner replacement reused source binding")
	}
	encode := func(r *typedContextResult) string {
		t.Helper()
		raw, err := json.Marshal(r)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	decoded, err := decodeTypedProjection(encode(r))
	if err != nil || decoded.Retained[0].Source.Version != r.Retained[0].Source.Version {
		t.Fatal("opaque host round trip lost source version", decoded, err)
	}
	if err := decoded.fitProjectionBytes(len(r.Rendered)); err != nil || decoded.SelectionDigest != r.SelectionDigest {
		t.Fatal("repacking changed retained source identity", decoded, err)
	}
	if err := decoded.fitProjectionBytes(0); err != nil || len(decoded.Retained) != 0 || decoded.SourceVersionState != "unavailable" {
		t.Fatal("omitted source still claimed as retained", decoded, err)
	}
	for _, mutate := range []func(*typedProjectionRef){
		func(ref *typedProjectionRef) { ref.Source.Version.OwnerID = "00000000-0000-0000-0000-000000000003" },
		func(ref *typedProjectionRef) { ref.Source.Version.RecordRevision = "8" },
		func(ref *typedProjectionRef) { ref.Source.Version.RecordID = "9007199254742998" },
	} {
		probe := makeProjection("00000000-0000-0000-0000-000000000001")
		mutate(&probe.Retained[0])
		if _, err := decodeTypedProjection(encode(probe)); err == nil {
			t.Fatal("changed source accepted under old selection binding")
		}
	}
	// Rehashing metadata cannot reconcile a version with different selected
	// assertion bytes; this checks consistency, not producer authentication.
	r.Retained[0].Source.Version.RecordRevision = "8"
	r.SelectionDigest = typedSelectionDigest(r.ProjectionDigest, r.Retained)
	if _, err := decodeTypedProjection(encode(r)); err == nil {
		t.Fatal("source revision contradicted selected assertion")
	}
	partial := makeProjection("00000000-0000-0000-0000-000000000001")
	partial.add("observations", typedItem{id: "observation", text: "derived", value: map[string]string{"summary": "derived"}})
	if err := partial.finish(); err != nil || partial.SourceVersionState != "partial" {
		t.Fatal("unversioned owner row gained version certainty", partial, err)
	}
	legacy := makeProjection("00000000-0000-0000-0000-000000000001")
	legacy.Retained[0].Source.MemoryParentState = ""
	legacy.SelectionDigest = typedSelectionDigest(legacy.ProjectionDigest, legacy.Retained)
	decoded, err = decodeTypedProjection(encode(legacy))
	if err != nil || decoded.Retained[0].Source.MemoryParentState != "" {
		t.Fatal("legacy metadata gained a parent completeness claim", decoded, err)
	}
	parent := MemoryRecordVersion{SchemaVersion: 1, OwnerID: legacy.Retained[0].Source.Version.OwnerID, RecordID: "1", RecordRevision: "2"}
	for _, state := range []string{"", "unavailable", "observed", "unknown-schema"} {
		ref := legacy.Retained[0]
		source := *ref.Source
		ref.Source = &source
		ref.Source.MemoryParentState = state
		ref.Source.MemoryParents = []MemoryRecordVersion{parent}
		if validTypedSource(ref) != (state == "observed") {
			t.Fatal("parent evidence contradicted its observation state", state)
		}
	}
}

// This measures the parent-eligibility query under a non-owner role. It is a
// storage microbenchmark, not an end-to-end request or retrieval-quality gate.
func BenchmarkMemoryEvidenceParentLookup(b *testing.B) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		b.Skip("set AIMEE_MEMORY_EVAL_URL for the PostgreSQL parent lookup benchmark")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		b.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		b.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE memories(id bigint PRIMARY KEY,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,
 valid_from text DEFAULT '',valid_until text DEFAULT '',scope_type text DEFAULT 'project',scope_value text DEFAULT 'parent-bench');
 INSERT INTO memories(id) SELECT n FROM generate_series(1,10000) n;
 CREATE TEMP TABLE fact_evidence(assertion_id bigint,source_kind text,source_id text,invalidated_at text DEFAULT '');
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id) SELECT 1,'memory','memory:'||(n*500)::text FROM generate_series(1,16)n;
 CREATE INDEX ON fact_evidence(assertion_id);
 CREATE TEMP TABLE entity_edges(id bigint PRIMARY KEY); INSERT INTO entity_edges VALUES(1);
 ANALYZE memories; ANALYZE fact_evidence; ANALYZE entity_edges;
 CREATE ROLE memory_parent_benchmark NOINHERIT NOBYPASSRLS;
 GRANT SELECT ON memories,fact_evidence,entity_edges TO memory_parent_benchmark;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY benchmark_scope ON memories USING(scope_type='project' AND scope_value=current_setting('aimee.memory_project',true));
 SELECT set_config('aimee.memory_project','parent-bench',true);
 SET LOCAL ROLE memory_parent_benchmark`)
	if err != nil {
		b.Fatal(err)
	}
	old := `NOT EXISTS(SELECT 1 FROM fact_evidence f LEFT JOIN memories m
 ON f.source_id='memory:'||m.id::text AND ` + currentMemorySQL("m.") + `
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND m.id IS NULL)`
	parsed := `NOT EXISTS(SELECT 1 FROM fact_evidence f LEFT JOIN memories m
 ON m.id=` + memoryLocatorIDSQL("f.source_id") + ` AND ` + currentMemorySQL("m.") + `
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND m.id IS NULL)`
	for _, tc := range []struct{ name, predicate string }{
		{"concatenated_parent_id", old},
		{"parsed_evidence_id", parsed},
		{"bounded_primary_key_lookup", currentMemoryEvidenceSQL("e", "", false)},
	} {
		query := `SELECT count(*) FROM entity_edges e WHERE e.id=1 AND ` + tc.predicate
		b.Run(tc.name, func(b *testing.B) {
			var count int
			// Compile and warm the same query before measuring steady-state work.
			for range 5 {
				if err := tx.QueryRow(ctx, query).Scan(&count); err != nil || count != 1 {
					b.Fatal(count, err)
				}
			}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if err := tx.QueryRow(ctx, query).Scan(&count); err != nil || count != 1 {
					b.Fatal(count, err)
				}
			}
		})
	}
}
