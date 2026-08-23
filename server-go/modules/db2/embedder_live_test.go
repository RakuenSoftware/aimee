package db2

import (
	"context"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// One memory and two indexed vectors for it, recorded at different embedder
// versions. Everything else about the two rows is identical, so anything that
// separates them is the version predicate and nothing else.
const embedderProbeSeed = `
INSERT INTO memories (id, tier, kind, key, content, created_at, updated_at)
 VALUES (900201, 'L2', 'fact', 'embedder-probe', 'embedded', '2026-01-01 00:00:00',
         '2026-01-01 00:00:00');
INSERT INTO vector_index_ops
 (point_id, collection, memory_id, status, attempts, indexed_at, embedding_version)
 VALUES
 (900201, 'memory_units', 900201, 'ok', 1, '2026-01-01 00:00:00', 'embedder-probe-v1'),
 (900202, 'memory_units', 900201, 'ok', 1, '2026-01-01 00:00:00', 'embedder-probe-v2');
-- A failed attempt at v1. It produced no vector, so it must not be counted:
-- counting it is the same lie the version column was added to stop.
INSERT INTO vector_index_ops
 (point_id, collection, memory_id, status, attempts, last_error, embedding_version)
 VALUES (900203, 'memory_units', 900201, 'failed', 3, 'boom', 'embedder-probe-v1');`

// TestLiveEmbeddingCountDiscriminatesByVersion proves the fix to the defect
// count_embeddings_for_version carried.
//
// It answered the same number whatever version it was asked about, because the
// statement never mentioned the version. Its caller is the embedder rollback,
// which refuses when the count is zero and otherwise makes the requested
// version active -- so a rollback to a version nothing had been embedded at
// passed the gate and left vector search reading a version with no vectors.
//
// The C replay cannot show this: it needs a countable row, and no operation on
// that wire creates the memories row the foreign key requires.
func TestLiveEmbeddingCountDiscriminatesByVersion(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	direct, ok := store.(*PoolStore)
	if !ok {
		// Seeds fixtures through the pool directly, which the storage wire
		// does not expose. Widening the wire so a test can reach past it would
		// be widening it for something no operation needs.
		t.Skip("this test uses the pool directly; run it without AIMEE_DB2_STORE=bus")
	}
	pool := direct.pool

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, embedderProbeSeed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	handler := NewDispatchHandler(&rollbackStore{tx: tx})
	count := func(t *testing.T, version string) uint32 {
		t.Helper()
		request, err := db2contract.EncodeCountEmbeddingsForVersionRequest(version)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(
			invocation(db2contract.StageCountEmbeddingsForVersion), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		answered, err := db2contract.DecodeCountEmbeddingsForVersionReply(body)
		if err != nil {
			t.Fatalf("decode reply: %v", err)
		}
		return answered
	}

	atV1 := count(t, "embedder-probe-v1")
	atV2 := count(t, "embedder-probe-v2")
	atNothing := count(t, "embedder-probe-never-used")

	if atV1 != 1 {
		t.Errorf("count at v1 = %d, want the one successful vector at that version "+
			"and not the failed attempt beside it", atV1)
	}
	if atV2 != 1 {
		t.Errorf("count at v2 = %d, want 1", atV2)
	}
	if atNothing != 0 {
		t.Errorf("count at a version nothing was embedded under = %d, want 0 -- "+
			"this is the answer the rollback gate acts on", atNothing)
	}
	// The assertion the old behaviour would fail: the answers have to differ.
	if atV1 == atNothing {
		t.Fatal("every version gets the same count; the version is not being bound")
	}
}

// TestLiveVectorIndexRecordStampsTheActiveVersion proves the other half: the
// column is populated by the write path, not only readable by the count.
//
// Without this the count would be correct and always zero, which is a different
// failure wearing the same face -- the rollback gate would refuse every
// rollback rather than passing every one.
func TestLiveVectorIndexRecordStampsTheActiveVersion(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	direct, ok := store.(*PoolStore)
	if !ok {
		// Seeds fixtures through the pool directly, which the storage wire
		// does not expose. Widening the wire so a test can reach past it would
		// be widening it for something no operation needs.
		t.Skip("this test uses the pool directly; run it without AIMEE_DB2_STORE=bus")
	}
	pool := direct.pool

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var column string
	err = tx.QueryRow(ctx, `SELECT column_name FROM information_schema.columns
 WHERE table_schema = current_schema() AND table_name = 'vector_index_ops'
   AND column_name = 'embedding_version'`).Scan(&column)
	if err != nil {
		t.Fatalf("vector_index_ops has no embedding_version column: %v", err)
	}

	// NOT NULL with an empty default, so an older row reads as "unknown version"
	// rather than as NULL. The count compares it by equality, and comparing
	// against NULL would silently answer zero for every version.
	var nullable string
	if err := tx.QueryRow(ctx, `SELECT is_nullable FROM information_schema.columns
 WHERE table_schema = current_schema() AND table_name = 'vector_index_ops'
   AND column_name = 'embedding_version'`).Scan(&nullable); err != nil {
		t.Fatalf("read nullability: %v", err)
	}
	if nullable != "NO" {
		t.Fatalf("embedding_version is nullable; equality against NULL answers "+
			"zero for every version (is_nullable = %q)", nullable)
	}
}
