package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/jackc/pgx/v5/pgconn"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

func exerciseSharedIndexReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true);
 UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index'; UPDATE vector_index_ops SET status='ok'`)
	original := backend.settings
	defer func() { backend.settings = original }()
	backend.settings = func() (map[string]any, error) {
		return map[string]any{"embedder_url": "http://embedder", "memory_coref_mode": "off"}, nil
	}
	var dimension int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dimension); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dimension)
	for i := range vector {
		vector[i] = .02
	}
	raw, _ := json.Marshal(vector)
	executor := &batchExecutor{reply: string(raw)}
	batch := func(n int) {
		t.Helper()
		if err := backend.sharedIndexBatch(ctx, executor, n); err != nil {
			t.Fatal(err)
		}
	}
	seed := func(scope string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','shared-worker','Alice deployed servers.','project',$1) RETURNING id`, scope).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	first, second := seed("index-a"), seed("index-b")
	checkJob := func(id int64, status string, generation, attempts int) {
		t.Helper()
		var state string
		var gen, n int
		if err := tx.QueryRow(ctx, `SELECT status,generation,attempts FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, id).Scan(&state, &gen, &n); err != nil || state != status || gen != generation || n != attempts {
			t.Fatal(state, gen, n, err)
		}
	}
	checkJob(first, "pending", 1, 0)
	batch(64)
	checkJob(first, "done", 1, 0)
	checkJob(second, "done", 1, 0)
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(DISTINCT memory_id) FROM memory_aliases WHERE memory_id IN($1,$2)`, first, second).Scan(&count); err != nil || count != 2 {
		t.Fatal("automatic metadata missing", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings WHERE point_id IN($1,$2)`, first, second).Scan(&count); err != nil || count != 2 {
		t.Fatal("automatic parent vectors missing", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings e JOIN memory_units u ON e.point_id=$2+u.id WHERE u.memory_id=$1`, first, unitPointOffset).Scan(&count); err != nil || count == 0 {
		t.Fatal("automatic unit vectors missing", count, err)
	}
	calls := executor.calls
	batch(64)
	if executor.calls != calls {
		t.Fatal("completed work repeated")
	}
	execSQL(`UPDATE memories SET confidence=0.7 WHERE id=$1`, first)
	checkJob(first, "done", 1, 0)
	// In-place content changes must requeue completed jobs and existing vectors.
	execSQL(`SELECT set_config('aimee.authority','user',true)`)
	execSQL(`UPDATE memories SET content='Bob configured databases.' WHERE id=$1`, first)
	checkJob(first, "pending", 2, 0)
	// A late failure rolls back earlier aliases/summaries too, retaining old index.
	execSQL(`RESET ROLE; REVOKE INSERT ON memory_chunks FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	batch(64)
	checkJob(first, "failed", 2, 1)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_chunks WHERE memory_id=$1 AND chunk_text LIKE '%Alice%'`, first).Scan(&count); err != nil || count == 0 {
		t.Fatal("failed job destroyed previous index", count, err)
	}
	batch(64)
	checkJob(first, "failed", 2, 1)
	execSQL(`RESET ROLE; GRANT INSERT ON memory_chunks TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	execSQL(`UPDATE kb_async_jobs SET next_attempt_at='' WHERE kind='memory_index' AND document_id=$1`, first)
	executor.reply = `[0.1,0.2]`
	batch(64)
	checkJob(first, "done", 2, 0)
	var attempts int
	if err := tx.QueryRow(ctx, `SELECT attempts FROM vector_index_ops WHERE point_id=$1 AND status='failed'`, first).Scan(&attempts); err != nil || attempts != 1 {
		t.Fatal("vector failure missing", attempts, err)
	}
	if err := tx.QueryRow(ctx, `SELECT vector_dims(embedding) FROM memory_embeddings WHERE point_id=$1`, first).Scan(&count); err != nil || count != dimension {
		t.Fatal("old vector lost", count, err)
	}
	batch(64)
	if err := tx.QueryRow(ctx, `SELECT attempts FROM vector_index_ops WHERE point_id=$1`, first).Scan(&attempts); err != nil || attempts != 1 {
		t.Fatal("retry ignored backoff", attempts, err)
	}
	resetBreaker(t)
	executor.reply = string(raw)
	execSQL(`UPDATE vector_index_ops SET updated_at='2000-01-01' WHERE memory_id=$1`, first)
	batch(64)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM vector_index_ops WHERE memory_id=$1 AND status<>'ok'`, first).Scan(&count); err != nil || count != 0 {
		t.Fatal("vector retry failed", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_chunks WHERE memory_id=$1 AND chunk_text LIKE '%Bob%'`, first).Scan(&count); err != nil || count == 0 {
		t.Fatal("retry kept stale metadata", count, err)
	}
	execSQL(`UPDATE memories SET lifecycle_state='superseded' WHERE id=$1`, first)
	batch(64)
	checkJob(first, "done", 3, 0)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings WHERE point_id=$1 OR point_id IN(SELECT $2+id FROM memory_units WHERE memory_id=$1)`, first, unitPointOffset).Scan(&count); err != nil || count != 0 {
		t.Fatal("retired vectors retained", count, err)
	}
	execSQL(`DELETE FROM memories WHERE id=$1`, second)
	batch(64)
	checkJob(second, "done", 2, 0)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM vector_index_ops WHERE memory_id=$1`, second).Scan(&count); err != nil || count != 0 {
		t.Fatal("deleted parent jobs retained", count, err)
	}
}

// Two connections exercise actual row locks, not nested savepoints. The replay
// database is disposable; all committed test records are removed on completion.
func TestSharedIndexConcurrentClaims(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL to the packaged DB2 replay database")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	a, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close(context.Background())
	b, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close(context.Background())
	var existing int
	if err = a.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND status IN('pending','failed')`).Scan(&existing); err != nil {
		t.Fatal(err)
	}
	if existing != 0 {
		t.Skip("concurrency fixture requires a replay database with no existing indexing backlog")
	}
	ids := []int64{}
	defer func() {
		for _, id := range ids {
			_, _ = a.Exec(context.Background(), `WITH summaries AS MATERIALIZED (SELECT id::text FROM memory_summaries WHERE memory_id=$1),
            deps AS (DELETE FROM derived_memory_dependencies WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id FROM summaries)),
            queue AS (DELETE FROM derived_rederivation_queue WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id FROM summaries))
            DELETE FROM derived_memory_registry WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id FROM summaries)`, id)
			_, _ = a.Exec(context.Background(), `DELETE FROM memory_lineage WHERE source_kind=$1 AND source_ref=$2`, derivedIndexSource, fmt.Sprint(id))
			_, _ = a.Exec(context.Background(), `DELETE FROM memories WHERE id=$1`, id)
			_, _ = a.Exec(context.Background(), `DELETE FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, id)
			_, _ = a.Exec(context.Background(), `DELETE FROM memory_embeddings WHERE point_id IN(SELECT point_id FROM vector_index_ops WHERE memory_id=$1)`, id)
			_, _ = a.Exec(context.Background(), `DELETE FROM vector_index_ops WHERE memory_id=$1`, id)
		}
	}()
	for _, key := range []string{"worker-lock-a", "worker-lock-b"} {
		var id int64
		if err = a.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content) VALUES('L2','fact',$1,'Alice deployed systems.') RETURNING id`, key).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
	}
	ta, err := a.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer ta.Rollback(context.Background())
	tb, err := b.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tb.Rollback(context.Background())
	makeOwner := func(tx pgx.Tx) *postgresDataStore {
		data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementKB)
		if err != nil {
			t.Fatal(err)
		}
		s := data.(*postgresDataStore)
		s.settings = func() (map[string]any, error) { return map[string]any{"memory_coref_mode": "off"}, nil }
		if err = sharedIndexContext(ctx, evalQueryer{tx}); err != nil {
			t.Fatal(err)
		}
		return s
	}
	sa, sb := makeOwner(ta), makeOwner(tb)
	if worked, err := sa.indexSharedRecord(ctx); err != nil || !worked {
		t.Fatal(worked, err)
	}
	if worked, err := sb.indexSharedRecord(ctx); err != nil || !worked {
		t.Fatal("second owner did not skip the locked parent", worked, err)
	}
	for i, tx := range []pgx.Tx{ta, tb} {
		var state string
		if err = tx.QueryRow(ctx, `SELECT status FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, ids[i]).Scan(&state); err != nil || state != "done" {
			t.Fatal("owners claimed same job", state, err)
		}
	}
	// A process crash is a rollback. Its work is immediately claimable, without a
	// stale-running lease timeout, and its derived writes never become visible.
	if err = ta.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if worked, err := sb.indexSharedRecord(ctx); err != nil || !worked {
		t.Fatal("abandoned claim not recoverable", worked, err)
	}
	if err = tb.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	// An embedding result holds the parent lock through persistence. An edit must
	// wait, then enqueue a new generation rather than accepting a stale result.
	ta, err = a.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer ta.Rollback(context.Background())
	sa = makeOwner(ta)
	var dimension int
	if err = ta.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dimension); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dimension)
	for i := range vector {
		vector[i] = .1
	}
	raw, _ := json.Marshal(vector)
	started, release := make(chan struct{}), make(chan struct{})
	executor := &blockingIndexExecutor{batchExecutor: batchExecutor{reply: string(raw)}, started: started, release: release}
	done := make(chan EmbedResponse, 1)
	resetBreaker(t)
	go func() { done <- EmbedRecord(ctx, 0, executor, sa, ids[0], "http://embedder", dimension) }()
	completed := false
	defer func() {
		select {
		case <-release:
		default:
			close(release)
		}
		if !completed {
			<-done
		}
	}()
	select {
	case <-started:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if _, err = b.Exec(ctx, `SET lock_timeout='100ms'; SELECT set_config('aimee.authority','user',false)`); err != nil {
		t.Fatal(err)
	}
	_, err = b.Exec(ctx, `UPDATE memories SET content='new generation' WHERE id=$1`, ids[0])
	var pgerr *pgconn.PgError
	if !errors.As(err, &pgerr) || pgerr.Code != "55P03" {
		t.Fatal("edit bypassed embedding lock", err)
	}
	close(release)
	result := <-done
	completed = true
	if !result.Embedded {
		t.Fatal(result)
	}
	if err = ta.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err = b.Exec(ctx, `UPDATE memories SET content='new generation' WHERE id=$1`, ids[0]); err != nil {
		t.Fatal(err)
	}
	var generation int
	var state string
	if err = b.QueryRow(ctx, `SELECT generation,status FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, ids[0]).Scan(&generation, &state); err != nil || generation != 2 || state != "pending" {
		t.Fatal(generation, state, err)
	}
}

type blockingIndexExecutor struct {
	batchExecutor
	started chan struct{}
	release chan struct{}
}

func (e *blockingIndexExecutor) Do(ctx context.Context, trace uint64, request egress.HTTPRequest) (egress.HTTPResponse, error) {
	close(e.started)
	select {
	case <-e.release:
		return e.batchExecutor.Do(ctx, trace, request)
	case <-ctx.Done():
		return egress.HTTPResponse{}, ctx.Err()
	}
}
