package memory

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

func TestMemoryIndexRebuildCannotBlockRecallPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_REPLAY_URL required")
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
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_REPLAY_URL required")
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
