package memory

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseVectorVerifyReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	var dim int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dim); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dim)
	for i := range vector {
		vector[i] = 0.01
	}
	encoded, _ := json.Marshal(vector)
	executor := &batchExecutor{reply: string(encoded)}
	handler := NewHandler(executor, WithDataStore(PlacementKB, backend))
	call := func(args string) map[string]any {
		t.Helper()
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "verify", args)
		if status != bus.ModuleStatusOK || result["status"] != "ok" {
			t.Fatal(result, status)
		}
		return result
	}
	result := call(`{"detail":true,"timings":true,"embedding_command":"http://embedder"}`)
	if result["schema"].(map[string]any)["expected"] != vectorSchemaVersion || result["schema"].(map[string]any)["stored"] != vectorSchemaVersion {
		t.Fatal(result)
	}
	if result["embedder"].(map[string]any)["expected_dim"] != float64(dim) || result["embedder"].(map[string]any)["ok"] != true {
		t.Fatal(result)
	}
	if result["timings"].(map[string]any)["trials"] != float64(10) || executor.calls != 11 {
		t.Fatal(result, executor.calls)
	}
	var actualIndexes string
	if err := tx.QueryRow(ctx, `SELECT COALESCE(string_agg(DISTINCT a.attname,',' ORDER BY a.attname),'') FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am am ON am.oid=c.relam JOIN pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=ANY(i.indkey) WHERE i.indrelid='memory_embeddings'::regclass AND i.indisvalid AND am.amname='btree'`).Scan(&actualIndexes); err != nil {
		t.Fatal(err)
	}
	if result["memory"].(map[string]any)["indexed_fields"] != actualIndexes {
		t.Fatal(result)
	}
	// The earlier rebuild in this transaction owns the same lock. Verification
	// must observe it, not consult the retired timestamp lease.
	if result["rebuild_lock_held"] != true {
		t.Fatal("lost Go rebuild lock", result)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error) SELECT id,'memory',id,'failed',9,'private diagnostic' FROM memories WHERE scope_value='runtime-project-b' LIMIT 1 ON CONFLICT(point_id) DO UPDATE SET status='failed',attempts=9,last_error='private diagnostic'`); err != nil {
		t.Fatal(err)
	}
	result = call(`{"detail":true,"scope_context":true,"project":"runtime-project-a","embedding_command":"http://embedder"}`)
	for _, row := range result["failed_ops"].([]any) {
		if row.(map[string]any)["last_error"] == "private diagnostic" {
			t.Fatal("scope leaked diagnostic", result)
		}
	}
	executor.reply = `[0.1,0.2]`
	result = call(`{"embedding_command":"http://embedder"}`)
	if result["ok"] != false || result["embedder"].(map[string]any)["ok"] != false {
		t.Fatal("wrong dimension reported healthy", result)
	}
	// Catalog query failures cannot become a successful empty health report.
	if _, err := tx.Exec(ctx, `RESET ROLE; REVOKE SELECT ON kb_embeddings FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	_, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "verify", `{}`)
	if status != bus.ModuleStatusInternal {
		t.Fatalf("missing grant became status %d", status)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; GRANT SELECT ON kb_embeddings TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
}
