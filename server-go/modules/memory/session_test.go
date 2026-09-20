package memory

import (
	"context"
	"encoding/json"
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
