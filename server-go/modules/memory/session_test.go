package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseSessionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	client := clientForHandler(t, handler)
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	seed := func(session, project, content, state string, n int) {
		t.Helper()
		execSQL(`INSERT INTO memories(tier,kind,key,content,source_session,scope_type,scope_value,lifecycle_state)
 SELECT 'L0','scratch','fold-fixture-'||n,$3,$1,'project',$2,$4 FROM generate_series(1,$5::int)n`, session, project, content, state, n)
	}
	seed("go-fold-success", "fold-private", strings.Repeat("full digest \"with quotes\" ", 30), "active", 3)
	seed("go-fold-rejected", "fold-private", "rejected source", "rejected", 1)
	seed("go-fold-rejected", "fold-private", "active source", "active", 1)
	seed("go-fold-bounded", "fold-private", "over the bound", "active", 65)
	seed("go-fold-mixed", "fold-private", "private", "active", 1)
	seed("go-fold-mixed", "different-private", "different", "active", 1)
	run := func(session string) map[string]any {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"session_id": session})
		return runPublicCommand(t, client, "fold_session", string(args))
	}
	result := run("go-fold-success")
	if result["status"] != "ok" || result["count"] != float64(3) {
		t.Fatal(result)
	}
	var sources, lineage, artifacts, embeddings, synthesis int
	var scope, content, evidence string
	if err := tx.QueryRow(ctx, `SELECT m.scope_value,m.content,a.payload->>'content',
 (SELECT count(*) FROM memories WHERE source_session='go-fold-success' AND tier='L0'),
 (SELECT count(*) FROM memory_lineage WHERE object_type='memory' AND object_id=m.id),
 (SELECT count(*) FROM artifacts WHERE payload->>'session_id'='go-fold-success'),
 (SELECT count(*) FROM evidence_index_ops WHERE artifact_id=a.id),
 (SELECT count(*) FROM learning_synth_ops WHERE artifact_id=a.id)
 FROM memories m JOIN artifacts a ON (a.payload->>'memory_id')::bigint=m.id
 WHERE m.source_session='go-fold-success' AND m.tier='L1'`).Scan(&scope, &content, &evidence, &sources, &lineage, &artifacts, &embeddings, &synthesis); err != nil {
		t.Fatal(err)
	}
	if scope != "fold-private" || len(content) <= 255 || evidence != content || sources != 0 || lineage != 3 || artifacts != 1 || embeddings != 1 || synthesis != 1 {
		t.Fatal(scope, len(content), sources, lineage, artifacts, embeddings, synthesis)
	}
	var checkpointCurrent bool
	if err := tx.QueryRow(ctx, `SELECT `+currentMemorySQL("m.")+` FROM memories m WHERE source_session='go-fold-success' AND tier='L1' AND lifecycle_state='active'`).Scan(&checkpointCurrent); err != nil || !checkpointCurrent {
		t.Fatalf("healthy folded checkpoint is unavailable: current=%v error=%v", checkpointCurrent, err)
	}
	for _, session := range []string{"go-fold-success", "go-fold-rejected", "go-fold-bounded", "go-fold-mixed", "absent"} {
		if r := run(session); r["status"] != "error" || r["count"] != float64(0) {
			t.Fatal(session, r)
		}
	}
	for session, want := range map[string]int{"go-fold-rejected": 2, "go-fold-bounded": 65, "go-fold-mixed": 2} {
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE source_session=$1 AND tier='L0'`, session).Scan(&sources); err != nil || sources != want {
			t.Fatal(session, sources, err)
		}
	}
	for _, args := range []string{`{}`, `{"session_id":" "}`, `{"session_id":42}`} {
		if r := runPublicCommand(t, client, "fold_session", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	// The evidence queue is required, so a failure must retain all source rows
	// and leave neither a checkpoint nor a partial artifact behind.
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	seed("go-fold-rollback", "fold-private", "retain on failure", "active", 2)
	execSQL(`RESET ROLE; REVOKE INSERT ON learning_synth_ops FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	if r := run("go-fold-rollback"); r["status"] == "ok" {
		t.Fatal("queue error was swallowed", r)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE source_session='go-fold-rollback' AND tier='L0'`).Scan(&sources); err != nil || sources != 2 {
		t.Fatal(sources, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM artifacts WHERE payload->>'session_id'='go-fold-rollback'`).Scan(&artifacts); err != nil || artifacts != 0 {
		t.Fatal(artifacts, err)
	}
	execSQL(`RESET ROLE; GRANT INSERT ON learning_synth_ops TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
}

func TestFoldOriginsRevocationAndErase(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("requires packaged PostgreSQL")
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`SET LOCAL jit=off; SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	schema, e := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if e != nil {
		t.Fatal(e)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("runtime grants missing")
	}
	exec(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	ids := []int64{}
	for i := 0; i < 3; i++ {
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,source_session,scope_type,scope_value)
 VALUES('L0','scratch',$1,$1,'fold-origin-test','project','fold-origin-test') RETURNING id`, fmt.Sprintf("fold retained source %d", i)).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
		exec(`INSERT INTO memory_units(memory_id,unit_type,unit_text) VALUES($1,'fact','copied original payload');
`, id)
		exec(`INSERT INTO memory_summaries(memory_id,scope,summary) VALUES($1,'headline','copied summary payload')`, id)
	}
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	before, err := s.memoryEvidence(ctx, ids[0])
	if err != nil {
		t.Fatal(err)
	}
	// A crash-equivalent error after child cleanup must restore the entire fold.
	exec(`RESET ROLE; SAVEPOINT failed_fold; CREATE FUNCTION pg_temp.fail_fold_certificate() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
 IF NEW.source_kind='memory-compaction-origin-v1' THEN RAISE EXCEPTION 'injected fold interruption'; END IF; RETURN NEW; END $$;
 CREATE TRIGGER fail_fold_certificate BEFORE INSERT ON memory_lineage FOR EACH ROW EXECUTE FUNCTION pg_temp.fail_fold_certificate(); SET LOCAL ROLE aimee_store_runtime`)
	if _, _, e := s.FoldSession(ctx, "fold-origin-test"); e == nil {
		t.Fatal("injected fold interruption was swallowed")
	}
	var retained int
	if e := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE source_session='fold-origin-test' AND tier='L0' AND content<>''`).Scan(&retained); e != nil || retained != 3 {
		t.Fatal("partial fold committed", retained, e)
	}
	if e := tx.QueryRow(ctx, `SELECT count(*) FROM memory_units WHERE memory_id=ANY($1)`, ids).Scan(&retained); e != nil || retained != 3 {
		t.Fatal("fold rollback lost child payloads", retained, e)
	}
	exec(`RESET ROLE; ROLLBACK TO SAVEPOINT failed_fold; RELEASE SAVEPOINT failed_fold; SET LOCAL ROLE aimee_store_runtime`)
	n, _, err := s.FoldSession(ctx, "fold-origin-test")
	if err != nil || n != 3 {
		t.Fatal(n, err)
	}
	var checkpoint int64
	if err = tx.QueryRow(ctx, `SELECT id FROM memories WHERE source_session='fold-origin-test' AND lifecycle_state='active'`).Scan(&checkpoint); err != nil {
		t.Fatal(err)
	}
	current := func(want bool) {
		t.Helper()
		var got bool
		if e := tx.QueryRow(ctx, `SELECT `+currentMemorySQL("m.")+` FROM memories m WHERE id=$1`, checkpoint).Scan(&got); e != nil || got != want {
			t.Fatalf("checkpoint eligibility=%v want %v: %v", got, want, e)
		}
	}
	current(true)
	evidence, err := s.memoryEvidence(ctx, checkpoint)
	if err != nil {
		t.Fatal(err)
	}
	if evidence["lineage_state"] != "complete" || evidence["source_family_count"] != 3 {
		t.Fatalf("fold minted or lost origins: %#v", evidence)
	}
	oldFamilies := before["origin_families"].([]string)
	found := false
	for _, family := range evidence["origin_families"].([]string) {
		if family == oldFamilies[0] {
			found = true
		}
	}
	if !found {
		t.Fatal("original ingestion family disappeared", before, evidence)
	}
	var payloads int
	if err = tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memories WHERE source_session='fold-origin-test' AND lifecycle_state='archived' AND content<>'')+
 (SELECT count(*) FROM memory_units WHERE memory_id=ANY($1))+(SELECT count(*) FROM memory_summaries WHERE memory_id=ANY($1))`, ids).Scan(&payloads); err != nil || payloads != 0 {
		t.Fatal("compaction retained source payloads", payloads, err)
	}
	exec(`SAVEPOINT reject_origin`)
	if changed, e := s.Reject(ctx, ids[0], "origin withdrawn"); e != nil || !changed {
		t.Fatal(changed, e)
	}
	current(false)
	if changed, e := s.Restore(ctx, ids[0], "operator"); e != nil || changed {
		t.Fatal("restored content-free identity", changed, e)
	}
	exec(`ROLLBACK TO SAVEPOINT reject_origin; RELEASE SAVEPOINT reject_origin`)
	current(true)
	exec(`SAVEPOINT missing_fold_inputs`)
	exec(`DELETE FROM memory_lineage WHERE object_type='memory' AND object_id=$1`, checkpoint)
	current(false)
	exec(`ROLLBACK TO SAVEPOINT missing_fold_inputs; RELEASE SAVEPOINT missing_fold_inputs`)
	exec(`SAVEPOINT remove_certificate`)
	exec(`DELETE FROM memory_lineage WHERE object_type='memory' AND object_id=$1 AND source_kind='memory-compaction-origin-v1'`, ids[0])
	current(false)
	exec(`ROLLBACK TO SAVEPOINT remove_certificate; RELEASE SAVEPOINT remove_certificate`)
	if changed, e := s.DeleteAs(ctx, ids[0], AuthorityUser); e != nil || !changed {
		t.Fatal(changed, e)
	}
	current(false)
	exec(`RESET ROLE`)
	var correctDigest bool
	if err = tx.QueryRow(ctx, `SELECT payload_digest=encode(sha256(convert_to('fold retained source 0','UTF8')),'hex') FROM memory_erasure_intents WHERE memory_id=$1`, ids[0]).Scan(&correctDigest); err != nil || !correctDigest {
		t.Fatal("erasure lost pre-compaction digest", correctDigest, err)
	}
	exec(`SAVEPOINT restore_origin`)
	_, err = tx.Exec(ctx, `INSERT INTO memories(key,content,scope_type,scope_value) VALUES('restored renamed source','fold retained source 0','project','fold-origin-test')`)
	exec(`ROLLBACK TO SAVEPOINT restore_origin; RELEASE SAVEPOINT restore_origin`)
	if err == nil {
		t.Fatal("restored erased source payload under a new ID")
	}
	// The empty archived value must not become a scope-wide prohibition on blanks.
	exec(`INSERT INTO memories(key,content,scope_type,scope_value) VALUES('new blank source','','project','fold-origin-test')`)
}
