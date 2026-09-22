package memory

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
)

type failingVectorDB struct{ store.DB }
type failingVectorTx struct{ store.Tx }

func (db failingVectorDB) Begin(ctx context.Context) (store.Tx, error) {
	tx, err := db.DB.Begin(ctx)
	if err != nil {
		return nil, err
	}
	return failingVectorTx{tx}, nil
}
func (tx failingVectorTx) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	if strings.Contains(sql, "memory_vector_rebuild_version") {
		return nil, errors.New("fixture: stamp failed")
	}
	return tx.Tx.Exec(ctx, sql, args...)
}

func exerciseVectorMaintenanceReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	var dim int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dim); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_embeddings(point_id,embedding,record_type)
 SELECT id,array_fill(0.01::real,ARRAY[$1::int])::vector,'memory' FROM memories WHERE lifecycle_state='active' ORDER BY id LIMIT 1
 ON CONFLICT(point_id) DO UPDATE SET embedding=EXCLUDED.embedding`, dim); err != nil {
		t.Fatal(err)
	}
	var initial int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings`).Scan(&initial); err != nil || initial < 1 {
		t.Fatal(initial, err)
	}
	unchanged := func() {
		t.Helper()
		var count int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings`).Scan(&count); err != nil || count != initial {
			t.Fatalf("vectors changed on failed rebuild: %d != %d, %v", count, initial, err)
		}
	}
	// A second database session holds the same transaction-scoped lock. The
	// rejected rebuild must neither clear rows nor queue any partial work.
	other, err := pgx.Connect(ctx, os.Getenv("AIMEE_DB2_REPLAY_URL"))
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close(ctx)
	lock, err := other.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Rollback(ctx)
	if _, err := lock.Exec(ctx, `SELECT pg_advisory_xact_lock($1)`, vectorRebuildLock); err != nil {
		t.Fatal(err)
	}
	if _, _, err := backend.RebuildVectorIndex(ctx, "contender"); err == nil || !strings.Contains(err.Error(), "another vector rebuild") {
		t.Fatal(err)
	}
	unchanged()
	if err := lock.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if err := backend.RecreateVectorCollection(ctx, dim+1); err == nil {
		t.Fatal("accepted mismatched dimension")
	}
	unchanged()
	failed := *backend
	failed.db = failingVectorDB{backend.db.(store.DB)}
	if _, _, err := failed.RebuildVectorIndex(ctx, "failure"); err == nil {
		t.Fatal("injected late failure ignored")
	}
	unchanged()
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "rebuild", `{"version":"vector-fixture"}`)
	if status != bus.ModuleStatusOK || result["status"] != "ok" || result["version"] != "vector-fixture" || result["failed"] != float64(0) {
		t.Fatal(result, status)
	}
	var remaining, active, pending int
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_embeddings),
 (SELECT count(*) FROM memories WHERE lifecycle_state='active'),
 (SELECT count(*) FROM vector_index_ops v JOIN memories m ON m.id=v.memory_id WHERE v.collection='memory' AND v.status='pending' AND m.lifecycle_state='active')`).Scan(&remaining, &active, &pending); err != nil || remaining != 0 || active != pending || result["rebuilt"] != float64(active) {
		t.Fatal(remaining, active, pending, result, err)
	}
	var after int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&after); err != nil || after != dim {
		t.Fatal("rebuild changed schema", after, err)
	}
	if exists, err := backend.VectorCollectionExists(ctx); err != nil || !exists {
		t.Fatal("rebuild lost index", exists, err)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; INSERT INTO memory_active_embedder(id,version) VALUES(1,'active-fixture') ON CONFLICT(id) DO UPDATE SET version=EXCLUDED.version; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	result, status = invokeContextCommand(t, handler, 0, bus.CommandContext{}, "rebuild", `{}`)
	if status != bus.ModuleStatusOK || result["version"] != "active-fixture" {
		t.Fatal("active version not resolved", result, status)
	}
	result, status = invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", `{"limit":1}`)
	if status != bus.ModuleStatusOK || result["status"] != "ok" {
		t.Fatal(result, status)
	}
	var stamp string
	if err := tx.QueryRow(ctx, `SELECT value FROM kb_meta WHERE key='vector_schema_version'`).Scan(&stamp); err != nil || stamp != "v4" {
		t.Fatal(stamp, err)
	}
}
