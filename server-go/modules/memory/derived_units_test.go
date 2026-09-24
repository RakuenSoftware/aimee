package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestDerivedUnitKind(t *testing.T) {
	for _, tc := range []struct {
		explicit, kind, typ string
		events              bool
		want                string
	}{
		{"semantic", "fact", "summary", true, "semantic"},
		{"procedural", "fact", "chunk", true, "procedural"},
		{"semantic", "fact", "event", true, "episodic"},
		{"", "workflow", "entity", false, "procedural"},
		{"", "fact", "entity", false, "semantic"},
		{"", "fact", "chunk", true, "episodic"},
	} {
		if got := derivedUnitKind(tc.explicit, tc.kind, tc.typ, tc.events); got != tc.want {
			t.Fatal(tc, got)
		}
	}
}

func exerciseDerivedUnitsReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	seed := func(key, scope string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,source_session,cognified_memory_kind)
 VALUES('L2','fact',$1,'Alice visited office yesterday.','project',$2,'derived-unit-session','semantic') RETURNING id`, key, scope).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	first := seed("unit-first", "units-visible")
	second := seed("unit-second", "units-visible")
	hidden := seed("unit-hidden", "units-hidden")
	// Preserve the legacy unobserved card on its own parent. Such a card must
	// not authorize serving or embedding an otherwise ordinary source record.
	cardParent := seed("unit-preserved-card", "units-visible")
	var card int64
	if err := tx.QueryRow(ctx, `INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,is_episode_card)
 VALUES($1,'episode_card','preserved','Preserve this episode card.',1) RETURNING id`, cardParent).Scan(&card); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref) VALUES('unit',$1,'memory','memory:original')`, card); err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	reindex := func(args string) {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args)
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(r, status)
		}
	}
	reindex(`{}`)
	unitIDs := func() string {
		t.Helper()
		var ids string
		if err := tx.QueryRow(ctx, `SELECT string_agg(id::text,',' ORDER BY id) FROM memory_units WHERE memory_id=$1`, first).Scan(&ids); err != nil {
			t.Fatal(err)
		}
		return ids
	}
	before := unitIDs()
	reindex(`{}`)
	if after := unitIDs(); before != after {
		t.Fatal("unit identities changed", before, after)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_unit_edges e JOIN memory_units a ON a.id=e.src_unit_id
 JOIN memory_units b ON b.id=e.dst_unit_id WHERE a.memory_id IN ($1,$2) AND b.memory_id=$3`, first, second, hidden).Scan(&count); err != nil || count != 0 {
		t.Fatal("cross-scope session edge", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_unit_edges e JOIN memory_units a ON a.id=e.src_unit_id
 JOIN memory_units b ON b.id=e.dst_unit_id WHERE a.memory_id=$1 AND b.memory_id=$2 AND e.edge_type='same_episode'`, first, second).Scan(&count); err != nil || count != 1 {
		t.Fatal("missing or repeated same-session edge", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_lineage l JOIN memory_units u ON u.id=l.object_id
 WHERE l.object_type='unit' AND u.id=$1 AND u.is_episode_card=1`, card).Scan(&count); err != nil || count != 1 {
		t.Fatal("episode lineage lost", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_units WHERE memory_id=$1 AND unit_type IN ('summary','entity','chunk') AND memory_kind<>'semantic'`, first).Scan(&count); err != nil || count != 0 {
		t.Fatal("cognified kind lost", count, err)
	}
	var unit, hiddenUnit int64
	if err := tx.QueryRow(ctx, `SELECT id FROM memory_units WHERE memory_id=$1 AND unit_type='chunk' ORDER BY id LIMIT 1`, first).Scan(&unit); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT id FROM memory_units WHERE memory_id=$1 AND unit_type='chunk' ORDER BY id LIMIT 1`, hidden).Scan(&hiddenUnit); err != nil {
		t.Fatal(err)
	}
	var dimension int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dimension); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dimension)
	for i := range vector {
		vector[i] = .01
	}
	raw, _ := json.Marshal(vector)
	executor := &batchExecutor{reply: string(raw)}
	handler = NewHandler(executor, WithDataStore(PlacementKB, backend))
	repair := func(point int64) map[string]any {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "repair", fmt.Sprintf(`{"memory_id":%d,"embedding_command":"http://embedder","scope_context":true,"project":"units-visible"}`, point))
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(r, status)
		}
		return r
	}
	point := unitPointOffset + unit
	if r := repair(point); r["repaired"] != float64(1) {
		t.Fatal(r)
	}
	var recordType, project, payload, state string
	var width, attempts int
	if err := tx.QueryRow(ctx, `SELECT e.record_type,e.project,e.payload_json,vector_dims(e.embedding),v.status FROM memory_embeddings e
 JOIN vector_index_ops v USING(point_id) WHERE e.point_id=$1`, point).Scan(&recordType, &project, &payload, &width, &state); err != nil || recordType != "unit" || project != "units-visible" || width != dimension || state != "ok" {
		t.Fatal(recordType, project, width, state, err)
	}
	var metadata map[string]any
	if err := json.Unmarshal([]byte(payload), &metadata); err != nil || metadata["memory_id"] != float64(first) || metadata["unit_id"] != float64(unit) {
		t.Fatal(metadata, err)
	}
	queryVector := make([]float64, len(vector))
	for i, value := range vector {
		queryVector[i] = float64(value)
	}
	visiblePoint := func(want bool) {
		t.Helper()
		hits, err := backend.searchVectors(ctx, queryVector, "unit", "", "units-visible", false, 256, Scope{Type: "project", Value: "units-visible"})
		if err != nil {
			t.Fatal(err)
		}
		found := false
		for _, hit := range hits {
			if hit.ID == point {
				found = true
			}
		}
		if found != want {
			t.Fatal("stale raw unit vector eligibility", found, want)
		}
	}
	visiblePoint(true)
	for _, change := range []string{
		`UPDATE memories SET content='changed unit parent' WHERE id=$1`,
		`UPDATE memory_units SET unit_text='independent unit edit' WHERE id=$2`,
		`DELETE FROM memory_lineage WHERE object_type='unit' AND object_id=$2 AND source_kind='memory-unit-input-v1'`,
	} {
		if _, err := tx.Exec(ctx, `SAVEPOINT unit_input_change`); err != nil {
			t.Fatal(err)
		}
		// Bind both parameters even when a mutation uses only one identity.
		if _, err := tx.Exec(ctx, `WITH ids AS (SELECT $1::bigint,$2::bigint) `+change, first, unit); err != nil {
			t.Fatal(err)
		}
		visiblePoint(false)
		var candidates int
		if err := tx.QueryRow(ctx, embeddingInputs+`SELECT count(*) FROM inputs WHERE point_id=$1`, point).Scan(&candidates); err != nil || candidates != 0 {
			t.Fatal("stale reembedding input", candidates, err)
		}
		calls := executor.calls
		if r := repair(point); r["failed"] != float64(1) || executor.calls != calls {
			t.Fatal("stale unit sent for embedding", r, executor.calls, calls)
		}
		if _, err := tx.Exec(ctx, `ROLLBACK TO SAVEPOINT unit_input_change; RELEASE SAVEPOINT unit_input_change`); err != nil {
			t.Fatal(err)
		}
		visiblePoint(true)
	}
	if _, err := tx.Exec(ctx, `SAVEPOINT unit_summary_change`); err != nil {
		t.Fatal(err)
	}
	var summaryPoint int64
	if err := tx.QueryRow(ctx, `SELECT $2::bigint+id FROM memory_units WHERE memory_id=$1 AND unit_type='summary' AND unit_key='headline'`, first, unitPointOffset).Scan(&summaryPoint); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `UPDATE memory_summaries SET summary='edited intermediate summary' WHERE memory_id=$1 AND scope='headline'`, first); err != nil {
		t.Fatal(err)
	}
	var summaryCandidates int
	if err := tx.QueryRow(ctx, embeddingInputs+`SELECT count(*) FROM inputs WHERE point_id=$1`, summaryPoint).Scan(&summaryCandidates); err != nil || summaryCandidates != 0 {
		t.Fatal("stale summary unit indexed", summaryCandidates, err)
	}
	visiblePoint(true)
	if _, err := tx.Exec(ctx, `ROLLBACK TO SAVEPOINT unit_summary_change; RELEASE SAVEPOINT unit_summary_change`); err != nil {
		t.Fatal(err)
	}
	executor.reply = `[0.1,0.2]`
	if r := repair(point); r["failed"] != float64(1) {
		t.Fatal(r)
	}
	if err := tx.QueryRow(ctx, `SELECT vector_dims(e.embedding),v.status,v.attempts FROM memory_embeddings e JOIN vector_index_ops v USING(point_id) WHERE e.point_id=$1`, point).Scan(&width, &state, &attempts); err != nil || width != dimension || state != "failed" || attempts != 1 {
		t.Fatal(width, state, attempts, err)
	}
	// Repair must retain the request scope even between its prepare and write calls.
	calls := executor.calls
	if r := repair(unitPointOffset + hiddenUnit); r["failed"] != float64(1) || executor.calls != calls {
		t.Fatal("hidden unit embedded", r, executor.calls)
	}
	executor.reply = string(raw)
	r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "repair", `{"failed_only":true,"embedding_command":"http://embedder","scope_context":true,"project":"units-visible"}`)
	if status != bus.ModuleStatusOK || r["repaired"] != float64(1) {
		t.Fatal("failed unit was not retried", r, status)
	}
	// A changed chunk retires its old point and queue entry, while unchanged
	// entities and episode cards retain their identities and lineage.
	if _, err := tx.Exec(ctx, `UPDATE memories SET content='Robert deployed infrastructure today.' WHERE id=$1`, first); err != nil {
		t.Fatal(err)
	}
	reindex(`{"scope_context":true,"project":"units-visible"}`)
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_units WHERE id=$1)+
 (SELECT count(*) FROM memory_embeddings WHERE point_id=$2)+(SELECT count(*) FROM vector_index_ops WHERE point_id=$2)`, unit, point).Scan(&count); err != nil || count != 0 {
		t.Fatal("stale unit data survived", count, err)
	}
	// Rebuilding the vector collection must queue derived units as well as parents.
	r, status = invokeContextCommand(t, handler, 0, bus.CommandContext{}, "rebuild", `{"version":"unit-rebuild","scope_context":true,"project":"units-visible"}`)
	if status != bus.ModuleStatusOK || r["kind"] != "invalid_argument" {
		t.Fatal("scoped collection reset accepted", r, status)
	}
	r, status = invokeContextCommand(t, handler, 0, bus.CommandContext{}, "rebuild", `{"version":"unit-rebuild"}`)
	if status != bus.ModuleStatusOK || r["status"] != "ok" {
		t.Fatal(r, status)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_units u JOIN memories m ON m.id=u.memory_id WHERE m.lifecycle_state='active'
 AND NOT EXISTS(SELECT 1 FROM vector_index_ops v WHERE v.point_id=$1+u.id AND v.status='pending')`, unitPointOffset).Scan(&count); err != nil || count != 0 {
		t.Fatal("unit not requeued", count, err)
	}
}
