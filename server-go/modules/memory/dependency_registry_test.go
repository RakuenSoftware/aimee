package memory

import (
	"context"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

// Exercise the installed dependency owner, including its transaction and queue
// contracts. A revision-only change must invalidate a versioned observation even
// when a producer has no content hash to compare.
func TestMemoryDependencyRegistryPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	install := func(start, end string) {
		t.Helper()
		a, b := strings.Index(string(schema), start), strings.Index(string(schema), end)
		if a < 0 || b <= a {
			t.Fatal("missing schema section", start)
		}
		exec(string(schema[a:b]))
	}
	install("CREATE OR REPLACE FUNCTION knowledge_input_moved(", "CREATE OR REPLACE FUNCTION derived_memory_freshness_for(")
	install("CREATE OR REPLACE FUNCTION derived_memory_declare(", "CREATE OR REPLACE FUNCTION derived_memory_builtin_writer()")
	start := strings.LastIndex(string(schema), "CREATE OR REPLACE VIEW work_outcome_projection AS")
	end := strings.Index(string(schema)[start:], "CREATE OR REPLACE FUNCTION work_outcome_record(")
	if start < 0 || end < 0 {
		t.Fatal("outcome projection missing")
	}
	exec(string(schema)[start : start+end])
	var id, revision int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content) VALUES('mr04-registry-source','retained text') RETURNING id,record_revision`).Scan(&id, &revision); err != nil {
		t.Fatal(err)
	}
	declare := func(version int64) {
		t.Helper()
		exec(`SELECT derived_memory_declare('mr04_test',$1,jsonb_build_array(jsonb_build_object(
 'input_kind','memory','input_id',$1::text,'input_version',$2::text,'contribution','essential')),'suppress')`, fmt.Sprint(id), fmt.Sprint(version))
	}
	status := func(want string) {
		t.Helper()
		var actual, calculated string
		if err := tx.QueryRow(ctx, `SELECT r.current_status,f.status FROM derived_memory_registry r
 JOIN derived_memory_freshness_for('memory',$1) f USING(derived_kind,derived_memory_id)
 WHERE r.derived_kind='mr04_test' AND r.derived_memory_id=$1`, fmt.Sprint(id)).Scan(&actual, &calculated); err != nil || actual != want || calculated != want {
			t.Fatal("registry status", actual, calculated, want, err)
		}
	}
	exec(`INSERT INTO work_outcomes(outcome_id,retrieval_event_id,subject_kind,subject_id,authenticated_evaluator,outcome,occurred_at)
 VALUES('mr04-registry-outcome','mr04-registry-retrieval','memory',$1,'mr04-evaluator','useful',pg_now_text())`, fmt.Sprint(id))
	var stale bool
	if err := tx.QueryRow(ctx, `SELECT stale FROM work_outcome_projection WHERE subject_kind='memory' AND subject_id=$1 AND workflow='*'`, fmt.Sprint(id)).Scan(&stale); err != nil || stale {
		t.Fatal("code-generation default treated as memory revision", stale, err)
	}
	declare(revision)
	status("fresh")
	exec(`SAVEPOINT interrupted_producer`)
	exec(`UPDATE memories SET valid_until='2100-01-01T00:00:00Z' WHERE id=$1`, id)
	var moved bool
	if err := tx.QueryRow(ctx, `SELECT knowledge_input_moved('memory',$1,$2,'')`, fmt.Sprint(id), fmt.Sprint(revision)).Scan(&moved); err != nil || !moved {
		t.Fatal("record revision ignored", moved, err)
	}
	status("unsupported")
	exec(`ROLLBACK TO interrupted_producer; RELEASE interrupted_producer`)
	status("fresh")
	var queued int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM derived_rederivation_queue WHERE derived_kind='mr04_test' AND derived_memory_id=$1`, fmt.Sprint(id)).Scan(&queued); err != nil || queued != 0 {
		t.Fatal("rolled back queue entry survived", queued, err)
	}
	exec(`UPDATE memories SET valid_until='2100-01-01T00:00:00Z' WHERE id=$1`, id)
	status("unsupported")
	// An old producer replay cannot certify an observation of an older revision.
	declare(revision)
	status("unsupported")
	exec(`SELECT derived_memory_apply_status('memory',$1)`, fmt.Sprint(id))
	exec(`SELECT derived_memory_apply_status('memory',$1)`, fmt.Sprint(id))
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM derived_rederivation_queue WHERE derived_kind='mr04_test' AND derived_memory_id=$1 AND state='pending'`, fmt.Sprint(id)).Scan(&queued); err != nil || queued != 1 {
		t.Fatal("duplicate delivery queue", queued, err)
	}
	declare(revision + 1)
	status("fresh")
	exec(`DELETE FROM memories WHERE id=$1`, id)
	status("unsupported")
}
