package memory

import (
	"context"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

func TestMemoryIndexRebuildCannotBlockRecallPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_KB_STORE_REPLAY_URL required")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	writer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close(context.Background())
	holder, err := writer.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer holder.Rollback(context.Background())
	if _, err := holder.Exec(ctx, `SELECT pg_advisory_xact_lock($1)`, vectorRebuildLock); err != nil {
		t.Fatal(err)
	}
	reader, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close(context.Background())
	tx, err := reader.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(context.Background())
	if _, err := tx.Exec(ctx, `CREATE ROLE memory_index_freshness_test NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA public TO memory_index_freshness_test; SET LOCAL ROLE memory_index_freshness_test`); err != nil {
		t.Fatal(err)
	}
	data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	backend := data.(*postgresDataStore)
	model := &sharedRecallExecutor{fail: true}
	backend.recallExecutor = model
	request := DataRequest{Operation: "search", Query: "bounded recall", Limit: 10}
	observed := withRetrievalCapabilities(ctx, PlacementKB, request)
	baseline := []Record{{ID: 42, Content: "already authorized lexical result"}}
	started := time.Now()
	got, err := backend.fuseSharedSemantic(observed, request, false, baseline)
	if err != nil || len(got) != 1 || got[0].ID != 42 || time.Since(started) > time.Second {
		t.Fatal("rebuild consumed recall deadline or lost lexical fallback", got, err, time.Since(started))
	}
	capability := observedRetrievalCapabilities(observed)
	if capability == nil || len(capability.Arms["dense"]) != 1 || capability.Arms["dense"][0].IndexReadiness != "rebuilding" {
		t.Fatal("rebuild fallback unreported", capability)
	}
	// The optional lane must not cancel SQL and leave the outer request's
	// transaction aborted; its lexical result still needs to commit/revalidate.
	var one int
	if err := tx.QueryRow(ctx, `SELECT 1`).Scan(&one); err != nil || one != 1 {
		t.Fatal("fallback aborted outer transaction", err)
	}
}

func TestAssertionGenerationCleanupIgnoresTempShadowPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_KB_STORE_REPLAY_URL required")
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
	exec(`INSERT INTO public.memory_embedder_versions(version,command,dimension) VALUES('mr11-temp-shadow','fixture',384)`)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO public.entity_edges(source,relation,target) VALUES('mr11-temp-shadow','uses','fixture') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO public.memory_assertion_embedding_versions(version,assertion_id,assertion_revision,input_hash,embedding) VALUES('mr11-temp-shadow',$1,1,'fixture','[1,0,0]'::vector)`, id)
	exec(`CREATE TEMP TABLE memory_assertion_embedding_versions(assertion_id bigint); INSERT INTO pg_temp.memory_assertion_embedding_versions SELECT id FROM public.entity_edges WHERE source='mr11-temp-shadow'`)
	exec(`UPDATE public.entity_edges SET target=target WHERE id=$1`, id)
	var actual, shadow int
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM public.memory_assertion_embedding_versions WHERE assertion_id=$1),(SELECT count(*) FROM pg_temp.memory_assertion_embedding_versions WHERE assertion_id=$1)`, id).Scan(&actual, &shadow); err != nil || actual != 0 || shadow != 1 {
		t.Fatal("cleanup resolved caller temporary relation", actual, shadow, err)
	}
}

// A canonical edit owns its parent before its dependency trigger enqueues the
// assertion. The index worker already owns that job when it stabilizes parents.
// It must yield on a busy parent, without model work or losing queued work.
func TestAssertionIndexYieldsToParentMutationPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_KB_STORE_REPLAY_URL required")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	writer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close(context.Background())
	worker, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer worker.Close(context.Background())
	key := fmt.Sprintf("assertion-lock-%d", time.Now().UnixNano())
	var parent, assertion int64
	exec := func(q string, args ...any) {
		t.Helper()
		if _, e := writer.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`INSERT INTO memory_embedder_versions(version,command,dimension) VALUES($1,'http://assertion-fixture',3)`, key)
	defer func() {
		_, _ = writer.Exec(context.Background(), "ROLLBACK")
		_, _ = worker.Exec(context.Background(), "ROLLBACK")
		_, e := writer.Exec(context.Background(), `DELETE FROM fact_evidence WHERE assertion_id=$1`, assertion)
		if e != nil {
			t.Error(e)
		}
		_, e = writer.Exec(context.Background(), `DELETE FROM entity_edges WHERE id=$1`, assertion)
		if e != nil {
			t.Error(e)
		}
		_, e = writer.Exec(context.Background(), `DELETE FROM memories WHERE id=$1`, parent)
		if e != nil {
			t.Error(e)
		}
		_, e = writer.Exec(context.Background(), `DELETE FROM kb_async_jobs WHERE (kind='memory_assertion_index' AND document_id=$1) OR (kind IN ('memory_index','memory_facts','memory_cognify') AND document_id=$2)`, assertion, parent)
		if e != nil {
			t.Error(e)
		}
		_, e = writer.Exec(context.Background(), `DELETE FROM memory_embedder_versions WHERE version=$1`, key)
		if e != nil {
			t.Error(e)
		}
	}()
	if err = writer.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content) VALUES('L2','fact',$1,'before') RETURNING id`, key).Scan(&parent); err != nil {
		t.Fatal(err)
	}
	// A code edge suffices for the common dependency-lock path and permits fixture
	// cleanup without manufacturing an irreversible semantic erasure commit.
	if err = writer.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target) VALUES($1,'uses','fixture') RETURNING id`, key).Scan(&assertion); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) VALUES($1,'memory','memory:'||$2::bigint::text,'supports')`, assertion, parent)
	exec(`BEGIN`)
	exec(`SELECT id FROM memories WHERE id=$1 FOR NO KEY UPDATE`, parent)
	tx, err := worker.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(context.Background())
	if _, err = tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	// Reproduce the production queue claim before assertionReembedPoint.
	if _, err = tx.Exec(ctx, `SELECT id FROM entity_edges WHERE id=$1 FOR UPDATE`, assertion); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, `SELECT id FROM kb_async_jobs WHERE kind='memory_assertion_index' AND document_id=$1 FOR UPDATE`, assertion); err != nil {
		t.Fatal(err)
	}
	data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	executor := &assertionEgressFixture{dim: 3}
	bounded, stop := context.WithTimeout(ctx, time.Second)
	defer stop()
	response, err := data.(*postgresDataStore).assertionReembedPoint(bounded, 0, executor, key, assertion)
	var pgerr *pgconn.PgError
	if !errors.As(err, &pgerr) || pgerr.Code != "55P03" {
		t.Fatalf("busy parent must yield before waiting on the writer: %v", err)
	}
	if response.Embedded || len(executor.seen) != 0 {
		t.Fatal("busy parent disclosed assertion text")
	}
	if err = tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	exec(`UPDATE memories SET content='after' WHERE id=$1`, parent)
	exec(`COMMIT`)
	var pending bool
	if err = writer.QueryRow(ctx, `SELECT status='pending' AND generation>1 FROM kb_async_jobs WHERE kind='memory_assertion_index' AND document_id=$1`, assertion).Scan(&pending); err != nil || !pending {
		t.Fatal("parent mutation lost retry work", pending, err)
	}
}
