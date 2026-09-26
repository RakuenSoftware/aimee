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
