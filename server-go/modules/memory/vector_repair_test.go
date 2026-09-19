package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseVectorRepairReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	t.Setenv("AIMEE_VECTOR_MAX_RETRY", "3")
	var id int64
	var dim int
	if err := tx.QueryRow(ctx, `SELECT id FROM memories WHERE lifecycle_state='active' ORDER BY id LIMIT 1`).Scan(&id); err != nil {
		t.Fatal(err)
	}
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
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "repair", args)
		if status != bus.ModuleStatusOK || result["status"] != "ok" {
			t.Fatal(result, status)
		}
		return result
	}
	single := fmt.Sprintf(`{"memory_id":%d,"embedding_command":"http://embedder"}`, id)
	result := call(single)
	if result["mode"] != "single" || result["repaired"] != float64(1) || executor.calls != 1 {
		t.Fatal(result, executor.calls)
	}
	var state, detail string
	var attempts int
	var persistedDim int
	if err := tx.QueryRow(ctx, `SELECT v.status,vector_dims(e.embedding) FROM vector_index_ops v JOIN memory_embeddings e USING(point_id) WHERE v.point_id=$1`, id).Scan(&state, &persistedDim); err != nil || state != "ok" || persistedDim != dim {
		t.Fatal(state, persistedDim, err)
	}

	// A syntactically valid vector of the wrong width fails in PostgreSQL.
	// The savepoint preserves the existing vector and lets the request persist
	// its retry state instead of leaving an aborted transaction behind.
	executor.reply = `[0.1,0.2]`
	result = call(single)
	if result["failed"] != float64(1) || result["repaired"] != float64(0) {
		t.Fatal(result)
	}
	if err := tx.QueryRow(ctx, `SELECT v.status,v.attempts,v.last_error,vector_dims(e.embedding) FROM vector_index_ops v JOIN memory_embeddings e USING(point_id) WHERE v.point_id=$1`, id).Scan(&state, &attempts, &detail, &persistedDim); err != nil || state != "failed" || detail == "" || persistedDim != dim {
		t.Fatal(state, attempts, detail, persistedDim, err)
	}
	executor.reply = string(encoded)
	if _, err := tx.Exec(ctx, `UPDATE vector_index_ops SET attempts=3 WHERE point_id=$1`, id); err != nil {
		t.Fatal(err)
	}
	before := executor.calls
	result = call(`{"failed_only":true,"embedding_command":"http://embedder"}`)
	if result["repaired"] != float64(0) || executor.calls != before {
		t.Fatal("retry cap ignored", result, executor.calls)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO code_index_ops(point_id,status,attempts) VALUES(9223372036854775806,'failed',3)`); err != nil {
		t.Fatal(err)
	}
	result = call(`{"reset_stuck":true}`)
	if result["mode"] != "reset_stuck" || result["reset_stuck"].(float64) < 2 {
		t.Fatal(result)
	}
	if err := tx.QueryRow(ctx, `SELECT attempts FROM code_index_ops WHERE point_id=9223372036854775806`).Scan(&attempts); err != nil || attempts != 0 {
		t.Fatal(attempts, err)
	}
	result = call(`{"failed_only":true,"embedding_command":"http://embedder"}`)
	if result["repaired"] != float64(1) || result["failed"] != float64(0) {
		t.Fatal(result)
	}
	result = call(`{"limit":1,"embedding_command":"http://embedder"}`)
	if result["mode"] != "all" || result["repaired"] != float64(1) {
		t.Fatal(result)
	}
	result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "repair", `{"memory_id":-1}`)
	if status != bus.ModuleStatusOK || result["kind"] != "invalid_argument" {
		t.Fatal(result, status)
	}
}

func TestRepairUsesOwnerConfiguration(t *testing.T) {
	t.Setenv("EMBEDDER_URL", "http://environment")
	backend := &postgresDataStore{settings: func() (map[string]any, error) {
		return map[string]any{"embedder_url": "http://configured", "embedder_command": "ignored"}, nil
	}}
	if got, err := backend.embeddingCommand(""); err != nil || got != "http://configured" {
		t.Fatal(got, err)
	}
	if got, err := backend.embeddingCommand("http://explicit"); err != nil || got != "http://explicit" {
		t.Fatal(got, err)
	}
	backend.settings = nil
	if got, err := backend.embeddingCommand(""); err != nil || got != "http://environment" {
		t.Fatal(got, err)
	}
	for _, value := range []string{"0", "-1", "1025", "broken"} {
		t.Setenv("AIMEE_VECTOR_MAX_RETRY", value)
		if vectorRetryLimit() != 8 {
			t.Fatal(value)
		}
	}
}
