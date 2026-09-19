package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/jackc/pgx/v5"
)

func exerciseReembedReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SAVEPOINT reembed_fixture`)
	defer execSQL(`ROLLBACK TO SAVEPOINT reembed_fixture; RELEASE SAVEPOINT reembed_fixture`)
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true),set_config('aimee.authority','user',true),set_config('aimee.principal','test:reembed',true)`)
	var first, second, unit int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','version-first','original text','project','reembed-a') RETURNING id`).Scan(&first); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','version-second','second text','project','reembed-b') RETURNING id`).Scan(&second); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text) VALUES($1,'chunk','part-1','unit text') RETURNING id`, first).Scan(&unit); err != nil {
		t.Fatal(err)
	}
	execSQL(`UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index'`)
	var dim int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dim); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dim)
	for i := range vector {
		vector[i] = .01
	}
	old, _ := vectorLiteral(vector)
	execSQL(`INSERT INTO memory_embeddings(point_id,embedding,record_type) VALUES($1,$2::vector,'memory'),(9223372036854775806,$2::vector,'semantic_assertion')`, first, old)
	executor := &versionExecutor{}
	setVector := func(n float32) {
		for i := range vector {
			vector[i] = n
		}
		b, _ := json.Marshal(vector)
		executor.reply = string(b)
	}
	setVector(.02)
	handler := NewHandler(executor, WithDataStore(PlacementKB, backend))
	call := func(verb, args string, success bool) map[string]any {
		t.Helper()
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, verb, args)
		if success && (status != bus.ModuleStatusOK || result["status"] != "ok") {
			t.Fatal(verb, result, status)
		}
		if !success && status == bus.ModuleStatusOK && result["status"] == "ok" {
			t.Fatal("unexpected success", verb, result)
		}
		return result
	}
	active := func(want string) {
		t.Helper()
		var got string
		if err := tx.QueryRow(ctx, `SELECT COALESCE((SELECT version FROM memory_active_embedder WHERE id=1),'')`).Scan(&got); err != nil || got != want {
			t.Fatal(got, want, err)
		}
	}
	live := func(want string) {
		t.Helper()
		var got string
		if err := tx.QueryRow(ctx, `SELECT embedding::text FROM memory_embeddings WHERE point_id=$1`, first).Scan(&got); err != nil || got != want {
			t.Fatal("live vector changed", err)
		}
	}
	var previous string
	if err := tx.QueryRow(ctx, `SELECT COALESCE((SELECT version FROM memory_active_embedder WHERE id=1),'')`).Scan(&previous); err != nil {
		t.Fatal(err)
	}
	result := call("reembed_start", `{"version":"replay-v1","embedding_command":"http://old-provider"}`, true)
	if result["ready"] != true || result["done"].(float64) < 3 {
		t.Fatal(result)
	}
	live(old)
	active(previous)
	calls := executor.calls
	call("reembed_start", `{"version":"replay-v1","embedding_command":"http://old-provider"}`, true)
	if executor.calls != calls {
		t.Fatal("completed drafts regenerated")
	}
	call("reembed_start", `{"version":"replay-v1","embedding_command":"http://different-provider"}`, false)
	call("reembed_cutover", `{}`, true)
	active("replay-v1")
	firstVector, _ := vectorLiteral(vector)
	live(firstVector)
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings WHERE point_id IN($1,9223372036854775806)`, unitPointOffset+unit).Scan(&count); err != nil || count != 2 {
		t.Fatal("unit missing or semantic vector removed", count, err)
	}
	executor.reply = `[0.1,0.2]`
	result = call("reembed_start", `{"version":"replay-v2","embedding_command":"http://new-provider"}`, true)
	if result["ready"] != false || result["failed"].(float64) < 3 {
		t.Fatal(result)
	}
	call("reembed_cutover", `{}`, false)
	active("replay-v1")
	live(firstVector)
	setVector(.03)
	call("reembed_start", `{"version":"replay-v2","embedding_command":"http://new-provider"}`, true)
	execSQL(`UPDATE memories SET content='changed canonical text' WHERE id=$1`, first)
	call("reembed_cutover", `{}`, false)
	active("replay-v1")
	live(firstVector)
	execSQL(`UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index'`)
	call("reembed_cutover", `{}`, false)
	result = call("reembed_start", `{"version":"replay-v2","embedding_command":"http://new-provider"}`, true)
	if result["embedded"] != float64(2) {
		t.Fatal("changed parent and derived unit must both refresh", result)
	}
	call("reembed_cutover", `{}`, true)
	active("replay-v2")
	secondVector, _ := vectorLiteral(vector)
	live(secondVector)
	call("reembed_rollback", `{"version":"unknown"}`, false)
	// Historical vectors exist but no longer describe the edited parent.
	call("reembed_rollback", `{"version":"replay-v1"}`, false)
	active("replay-v2")
	live(secondVector)
	setVector(.02)
	call("reembed_start", `{"version":"replay-v1","embedding_command":"http://old-provider"}`, true)
	call("reembed_rollback", `{"version":"replay-v1"}`, true)
	active("replay-v1")
	live(firstVector)
	command, err := backend.embeddingCommand("")
	if err != nil || command != "http://old-provider" {
		t.Fatal(command, err)
	}
	// A failed final insertion rolls back the deletion and the active pointer.
	execSQL(`RESET ROLE; REVOKE INSERT ON memory_embeddings FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("reembed_rollback", `{"version":"replay-v2"}`, false)
	active("replay-v1")
	live(firstVector)
	execSQL(`RESET ROLE; GRANT INSERT ON memory_embeddings TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	// A scoped request cannot claim global readiness or change the active model.
	call("reembed_status", `{"scope_context":true,"project":"reembed-a"}`, false)
	// Ordinary indexing must not let a provider selected before cutover write into
	// the new version; the configured active provider can update its retained copy.
	result = call("repair", fmt.Sprintf(`{"memory_id":%d,"embedding_command":"http://new-provider"}`, first), true)
	if result["failed"] != float64(1) {
		t.Fatal(result)
	}
	result = call("repair", fmt.Sprintf(`{"memory_id":%d}`, first), true)
	if result["repaired"] != float64(1) {
		t.Fatal(result)
	}
	executor.identity = "changed-space"
	result = call("repair", fmt.Sprintf(`{"memory_id":%d}`, first), true)
	if result["failed"] != float64(1) {
		t.Fatal("serving identity drift accepted", result)
	}
	executor.identity = ""
	executor.flip = true
	executor.probes = 0
	result = call("repair", fmt.Sprintf(`{"memory_id":%d}`, first), true)
	if result["failed"] != float64(1) {
		t.Fatal("mid-request model swap accepted", result)
	}
	executor.flip = false
	call("embed", `{}`, false)
	call("embed", fmt.Sprintf(`{"memory_id":%d}`, first), true)
	call("embed", `{"all":true,"version":"unrelated"}`, false)
	execSQL(`DELETE FROM memory_embeddings WHERE point_id IN($1,$2)`, second, unitPointOffset+unit)
	result = call("embed", `{"all":true}`, true)
	if result["embedded"].(float64) < 2 || result["failed"] != float64(0) {
		t.Fatal(result)
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value','reembed-a',true)`)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embedding_versions WHERE memory_id=$1`, second).Scan(&count); err != nil || count != 0 {
		t.Fatal("hidden vector archive leaked", count, err)
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	call("rebuild", `{"version":"replay-v2"}`, false)
	active("replay-v1")
	call("rebuild", `{"version":"replay-v1"}`, true)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings WHERE point_id=9223372036854775806`).Scan(&count); err != nil || count != 1 {
		t.Fatal("rebuild erased semantic vectors", count, err)
	}
	status := call("reembed_status", `{}`, true)
	encoded, _ := json.Marshal(status)
	if strings.Contains(string(encoded), "http://") || strings.Contains(string(encoded), "command") {
		t.Fatal("status exposed provider configuration", status)
	}
}

type versionExecutor struct {
	batchExecutor
	identity string
	flip     bool
	probes   int
}

func (e *versionExecutor) Do(ctx context.Context, trace uint64, req egress.HTTPRequest) (egress.HTTPResponse, error) {
	if strings.HasSuffix(req.TargetURL, "/health") {
		e.probes++
		id := e.identity
		if id == "" {
			id = "space:" + strings.TrimSuffix(req.TargetURL, "/health")
		}
		if e.flip && e.probes%2 == 0 {
			id += "-changed"
		}
		raw, _ := json.Marshal(map[string]string{"serving_id": id})
		return egress.HTTPResponse{Status: 200, Body: raw}, nil
	}
	return e.batchExecutor.Do(ctx, trace, req)
}
