package postgres

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

func TestEvaluationRequiresExplicitDisposableURL(t *testing.T) {
	t.Setenv("AIMEE_DB2_EVAL_URL", "")
	t.Setenv("AIMEE_STORE_URL", "postgres://live:secret@localhost/live")
	if _, err := OpenEvaluationStore(context.Background(), ""); err == nil || !strings.Contains(err.Error(), "AIMEE_DB2_EVAL_URL") {
		t.Fatal("evaluation accepted live store fallback", err)
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", "postgres://user:secret@host:invalid/database")
	if _, err := OpenEvaluationStore(context.Background(), ""); err == nil || strings.Contains(err.Error(), "secret") {
		t.Fatal("invalid evaluation DSN was accepted or disclosed", err)
	}
}

func evaluationAdmin(t *testing.T) *pgxpool.Pool {
	t.Helper()
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("set AIMEE_DB_TEST_URL to an explicit disposable PostgreSQL admin DSN")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	pool, err := pgxpool.New(context.Background(), url)
	if err != nil {
		t.Fatal("cannot open test admin pool")
	}
	t.Cleanup(pool.Close)
	return pool
}

func TestEvaluationIsolationAndAbandonedTransactionCleanup(t *testing.T) {
	admin := evaluationAdmin(t)
	ctx := context.Background()
	var parent string
	if err := admin.QueryRow(ctx, "SELECT current_database()").Scan(&parent); err != nil {
		t.Fatal(err)
	}
	const schema = "CREATE TABLE public.eval_fixture (id bigint PRIMARY KEY, content text)"
	first, err := OpenEvaluationStore(ctx, schema)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := first.Close(); err != nil {
			t.Error(err)
		}
	})
	second, err := OpenEvaluationStore(ctx, schema)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := second.Close(); err != nil {
			t.Error(err)
		}
	})
	var one, two string
	if err := first.QueryRow(ctx, "SELECT current_database()").Scan(&one); err != nil {
		t.Fatal(err)
	}
	if err := second.QueryRow(ctx, "SELECT current_database()").Scan(&two); err != nil {
		t.Fatal(err)
	}
	if one == two || one == parent || two == parent {
		t.Fatal("evaluation reused another database", one, two, parent)
	}
	if _, err := first.Exec(ctx, "INSERT INTO public.eval_fixture VALUES (9007199254740993, 'first')"); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := second.QueryRow(ctx, "SELECT count(*) FROM public.eval_fixture").Scan(&count); err != nil || count != 0 {
		t.Fatal("fixture leaked between sessions", count, err)
	}
	var id int64
	if err := first.QueryRow(ctx, "SELECT id FROM public.eval_fixture").Scan(&id); err != nil || id != 9007199254740993 {
		t.Fatal("SQL wire lost fixture identity", id, err)
	}
	// Leave every allowed transaction open. The cap must refuse before pool
	// starvation, and Close must release all leases before waiting on the pool.
	for i := 0; i < first.handler.txs.maxOpen; i++ {
		tx, err := first.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, "INSERT INTO public.eval_fixture VALUES ($1, 'abandoned')", i+1); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := first.Begin(ctx); err == nil {
		t.Fatal("open transaction cap not enforced")
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	if err := first.Close(); err != nil {
		t.Fatal("second close", err)
	}
	if _, err := first.Exec(ctx, "SELECT 1"); err == nil {
		t.Fatal("closed store accepted work")
	}
	if err := admin.QueryRow(ctx, "SELECT count(*) FROM pg_database WHERE datname=$1", one).Scan(&count); err != nil || count != 0 {
		t.Fatal("closed evaluation database retained", count, err)
	}
	if err := second.QueryRow(ctx, "SELECT count(*) FROM public.eval_fixture").Scan(&count); err != nil || count != 0 {
		t.Fatal("cleanup changed sibling", count, err)
	}
}

func TestEvaluationCancellationAndFailedBootstrapCleanup(t *testing.T) {
	admin := evaluationAdmin(t)
	ctx := context.Background()
	var before, after int
	const countSQL = "SELECT count(*) FROM pg_database WHERE datname LIKE 'aimee_memory_eval_%'"
	if err := admin.QueryRow(ctx, countSQL).Scan(&before); err != nil {
		t.Fatal(err)
	}
	if _, err := OpenEvaluationStore(ctx, "CREATE TABLE eval_partial(id int); SELECT missing_eval_function()"); err == nil {
		t.Fatal("invalid schema accepted")
	}
	if err := admin.QueryRow(ctx, countSQL).Scan(&after); err != nil || after != before {
		t.Fatal("failed bootstrap leaked a database", before, after, err)
	}
	lifetime, cancel := context.WithCancel(ctx)
	defer cancel()
	s, err := OpenEvaluationStore(lifetime, "SELECT 1")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	query, stop := context.WithTimeout(ctx, 50*time.Millisecond)
	defer stop()
	start := time.Now()
	if _, err := s.Exec(query, "SELECT pg_sleep(10)"); err == nil {
		t.Fatal("query ignored cancellation")
	}
	if time.Since(start) > time.Second {
		t.Fatal("local SQL wire ignored caller deadline")
	}
	if _, err := s.Exec(ctx, "SELECT 1"); err != nil {
		t.Fatal("cancelled query poisoned next connection", err)
	}
	cancel()
	if _, err := s.Exec(ctx, "SELECT 1"); !errors.Is(err, context.Canceled) {
		t.Fatal("session lifetime cancellation ignored", err)
	}
	if err := s.Close(); err != nil {
		t.Fatal("cancelled session could not clean up", err)
	}
}
