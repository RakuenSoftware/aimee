package memory

import (
	"context"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestDerivedTextIndexes(t *testing.T) {
	d := deriveText("team-trip", "Alice: Alice visited office on Sep 12. She met Robert yesterday.", "2026-09-17 12:00:00")
	has := func(terms []derivedTerm, text, role string) bool {
		for _, v := range terms {
			if v.Text == text && v.Role == role {
				return true
			}
		}
		return false
	}
	if !has(d.Aliases, "team-trip", "") || !has(d.Entities, "alice", "actor") || !has(d.Temporal, "2026 09 12", "absolute_day") || !has(d.Temporal, "2026 09 16", "absolute_day") {
		t.Fatalf("derived indexes: %+v", d)
	}
	if len(d.Frames) != 2 || d.Frames[0].Action != "visited" || d.Frames[0].Location != "office" || d.Frames[0].Time != "sep 12" || d.Frames[1].Evidence != "chunk" {
		t.Fatal(d.Frames)
	}
	if d.Headline == "" || d.Signals == "" || len(d.Chunks) == 0 {
		t.Fatal(d)
	}
	if derivedNormalize("[12-Sept-2026] the team's deployment") != "12 sept 2026 team deployment" {
		t.Fatal("date/key normalization")
	}
	if derivedMonth("jun") != 6 || derivedMonth("sept") != 9 || derivedMonth("dec") != 12 {
		t.Fatal("month abbreviation")
	}
	// The C chunker could repeat the early punctuation until the eight-chunk
	// limit, losing the rest of the note. Every iteration must advance.
	text := "First. " + strings.Repeat("🦊", 150) + " finish"
	chunks := deriveChunks(text)
	if len(chunks) < 2 || !strings.Contains(chunks[len(chunks)-1], "finish") {
		t.Fatal(chunks)
	}
	for i, chunk := range chunks {
		if !utf8.ValidString(chunk) || len(chunk) > 200 {
			t.Fatalf("chunk %d: %q", i, chunk)
		}
	}
	if len(deriveChunks(strings.Repeat("x", 5000))) != 8 {
		t.Fatal("chunk bound")
	}
	// An invalid calendar date must not become a fabricated date in March.
	invalid := deriveTemporal("", "February 31", "2026-09-17")
	for _, ref := range invalid {
		if ref.Role == "absolute_day" {
			t.Fatal(invalid)
		}
	}
}

func exerciseDerivedTextReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	var id, hidden, retired int64
	seed := func(key, scope, state string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,created_at)
 VALUES('L2','fact',$1,'Alice: Alice visited office on Sep 12. She met Robert yesterday.','project',$2,$3,'2026-09-17 12:00:00') RETURNING id`, key, scope, state).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	id = seed("derived-owner", "derived-visible", "active")
	hidden = seed("derived-hidden", "derived-hidden", "active")
	retired = seed("derived-retired", "derived-visible", "archived")
	if _, err := tx.Exec(ctx, `INSERT INTO memory_summaries(memory_id,scope,summary) VALUES($1,'curated','Keep the authored summary');
`, id); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_event_frames(memory_id,actor,action,evidence_kind) VALUES($1,'human','approved','authored')`, id); err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	args := `{"limit":100000,"scope_context":true,"project":"derived-visible"}`
	counts := func() string {
		t.Helper()
		var counts string
		if err := tx.QueryRow(ctx, `SELECT concat_ws(',',
 (SELECT count(*) FROM memory_aliases WHERE memory_id=$1),
 (SELECT count(*) FROM memory_entities WHERE memory_id=$1),
 (SELECT count(*) FROM memory_temporal_refs WHERE memory_id=$1),
 (SELECT count(*) FROM memory_event_frames WHERE memory_id=$1),
 (SELECT count(*) FROM memory_summaries WHERE memory_id=$1),
 (SELECT count(*) FROM memory_chunks WHERE memory_id=$1))`, id).Scan(&counts); err != nil {
			t.Fatal(err)
		}
		return counts
	}
	var expected int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE lifecycle_state='active' AND (scope_type='global' OR scope_type='project' AND scope_value='derived-visible')`).Scan(&expected); err != nil {
		t.Fatal(err)
	}
	firstCounts := ""
	for i := 0; i < 2; i++ {
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args)
		if status != bus.ModuleStatusOK || r["status"] != "ok" || r["rebuilt"] != float64(expected) {
			t.Fatal(r, status)
		}
		if i == 0 {
			firstCounts = counts()
		} else if counts() != firstCounts {
			t.Fatal("rebuild not idempotent", firstCounts, counts())
		}
		for _, n := range strings.Split(counts(), ",") {
			if n == "0" {
				t.Fatal("index missing", counts())
			}
		}
	}
	var hiddenRows, kept int
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_summaries WHERE memory_id IN ($1,$2)`, hidden, retired).Scan(&hiddenRows); err != nil || hiddenRows != 0 {
		t.Fatal(hiddenRows, err)
	}
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_summaries WHERE memory_id=$1 AND scope='curated')+
 (SELECT count(*) FROM memory_event_frames WHERE memory_id=$1 AND evidence_kind='authored')`, id).Scan(&kept); err != nil || kept != 2 {
		t.Fatal(kept, err)
	}
	// Only generated summaries have observed producer inputs. The authored row
	// remains stored, but its unknown derivation must not masquerade as fresh.
	summaries, err := backend.Summaries(ctx, id, 10)
	if err != nil || len(summaries) != 2 {
		t.Fatal("generated summary observation missing", summaries, err)
	}
	if _, err := tx.Exec(ctx, `SAVEPOINT summary_source_change`); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `UPDATE memories SET content='Changed canonical source must replace the old headline.' WHERE id=$1`, id); err != nil {
		t.Fatal(err)
	}
	summaries, err = backend.Summaries(ctx, id, 10)
	if err != nil || len(summaries) != 0 {
		t.Fatal("stale summaries survived parent edit", summaries, err)
	}
	r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args)
	if status != bus.ModuleStatusOK || r["status"] != "ok" {
		t.Fatal(r, status)
	}
	summaries, err = backend.Summaries(ctx, id, 10)
	if err != nil || len(summaries) == 0 || !strings.Contains(summaries[0].Summary, "changed canonical source") {
		t.Fatal("producer did not rebuild observed summary", summaries, err)
	}
	if _, err := tx.Exec(ctx, `ROLLBACK TO SAVEPOINT summary_source_change; RELEASE SAVEPOINT summary_source_change`); err != nil {
		t.Fatal(err)
	}
	before := counts()
	if _, err := tx.Exec(ctx, `RESET ROLE; REVOKE INSERT ON memory_chunks FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	if _, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args); status == bus.ModuleStatusOK {
		t.Fatal("rebuild accepted denied chunk write")
	}
	if after := counts(); after != before {
		t.Fatal("failed rebuild changed index", before, after)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; GRANT INSERT ON memory_chunks TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
}
