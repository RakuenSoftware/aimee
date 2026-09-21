package memory

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseDerivedRelationsReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	var id, other, oldEpisode, customEpisode, authored int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,source_session,valid_from,valid_until)
 VALUES('L2','fact','relations-owner','Robert: Robert deployed infrastructure.','project','relations-visible','relation-session','2026-01-01','2027-01-01') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','private-target','Private linked contents','project','relations-hidden') RETURNING id`).Scan(&other); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'related_to')`, id, other); err != nil {
		t.Fatal(err)
	}

	var public int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content) VALUES('L2','fact','shared-target','Shared link context') RETURNING id`).Scan(&public); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'related_to')`, id, public); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session)
 VALUES($1,'relations-owner','Alice visited park.','relation-session') RETURNING id`, id).Scan(&oldEpisode); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text)
 VALUES($1,$2,'Alice','visited','park','Alice visited park.')`, id, oldEpisode); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session)
 VALUES($1,'curated-episode','Authored episode','relation-session') RETURNING id`, id).Scan(&customEpisode); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
 VALUES($1,'operator','approved','release','Authored approval') RETURNING id`, id).Scan(&authored); err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	call := func() {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", `{}`)
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(r, status)
		}
	}
	snapshot := func() string {
		t.Helper()
		var text string
		if err := tx.QueryRow(ctx, `SELECT concat_ws('|',
 (SELECT string_agg(id::text,',' ORDER BY id) FROM memory_episodes WHERE memory_id=$1),
 (SELECT string_agg(id::text,',' ORDER BY id) FROM memory_relations WHERE memory_id=$1),
 (SELECT string_agg(id::text,',' ORDER BY id) FROM memory_summaries WHERE memory_id=$1))`, id).Scan(&text); err != nil {
			t.Fatal(err)
		}
		return text
	}
	// A parent-key episode belongs to the generator and is replaced. Authored
	// fixtures must use a separate key to survive asynchronous refresh.
	beforeEpisode, err := backend.EpisodeGet(ctx, "relations-owner")
	if err != nil || beforeEpisode.ID != oldEpisode {
		t.Fatal("legacy parent-key episode missing before refresh", beforeEpisode, err)
	}
	call()
	parentEpisode, err := backend.EpisodeGet(ctx, "relations-owner")
	if err != nil || parentEpisode.ID == oldEpisode || parentEpisode.Text != "Robert: Robert deployed infrastructure." {
		t.Fatal("parent-key lookup did not advance to generated episode", parentEpisode, err)
	}
	authoredEpisode, err := backend.EpisodeGet(ctx, "curated-episode")
	if err != nil || authoredEpisode.ID != customEpisode || authoredEpisode.Text != "Authored episode" {
		t.Fatal("distinct authored episode changed during refresh", authoredEpisode, err)
	}
	first := snapshot()
	call()
	if after := snapshot(); after != first {
		t.Fatal("derived identities changed", first, after)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_episodes WHERE id=$1)+
 (SELECT count(*) FROM memory_relations WHERE memory_id=$2 AND src_entity='Alice')`, oldEpisode, id).Scan(&count); err != nil || count != 0 {
		t.Fatal("old C derivation survived", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_episodes WHERE id=$1)+
 (SELECT count(*) FROM memory_relations WHERE id=$2)`, customEpisode, authored).Scan(&count); err != nil || count != 2 {
		t.Fatal("authored data lost", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND
 (dst_entity='private-target' OR fact_text LIKE '%Private linked contents%')`, id).Scan(&count); err != nil || count != 0 {
		t.Fatal("private link copied", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND relation='deployed'
 AND valid_at='2026-01-01' AND invalid_at='2027-01-01'`, id).Scan(&count); err != nil || count != 1 {
		t.Fatal("event relation validity lost", count, err)
	}

	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND relation='related_to' AND src_entity='robert' AND dst_entity='shared-target'`, id).Scan(&count); err != nil || count != 1 {
		t.Fatal("primary actor priority lost", count, err)
	}
	if _, err := tx.Exec(ctx, `UPDATE memories SET content='Carol configured databases.' WHERE id=$1`, id); err != nil {
		t.Fatal(err)
	}
	call()
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND relation='deployed'`, id).Scan(&count); err != nil || count != 0 {
		t.Fatal("obsolete generated relation survived", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM derived_memory_registry r WHERE r.derived_kind='summary'
 AND NOT EXISTS(SELECT 1 FROM memory_summaries s WHERE s.id::text=r.derived_memory_id)`).Scan(&count); err != nil || count != 0 {
		t.Fatal("orphan summary dependency registry", count, err)
	}
	// A failure late in relation generation must restore the previous summaries,
	// units, relations and lineage instead of publishing a partial new snapshot.
	before := snapshot()
	if _, err := tx.Exec(ctx, `RESET ROLE; REVOKE INSERT ON memory_relations FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	if _, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", `{}`); status == bus.ModuleStatusOK {
		t.Fatal("accepted denied relation write")
	}
	if after := snapshot(); after != before {
		t.Fatal("partial rebuild committed", before, after)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; GRANT INSERT ON memory_relations TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
}
