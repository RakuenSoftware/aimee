package families

import (
	"context"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"sync"
	"testing"
	"time"
)

func TestSessionCachePostgresConcurrentNonceCASIsolationDeletion(t *testing.T) {
	dsn := os.Getenv("AIMEE_TEST_PG_URL")
	if dsn == "" {
		t.Skip("requires owned AIMEE_TEST_PG_URL")
	}
	ctx := context.Background()
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	schema := fmt.Sprintf("session_cache_%d", time.Now().UnixNano())
	if _, err = pool.Exec(ctx, "CREATE SCHEMA "+pgx.Identifier{schema}.Sanitize()); err != nil {
		t.Fatal(err)
	}
	defer pool.Exec(ctx, "DROP SCHEMA "+pgx.Identifier{schema}.Sanitize()+" CASCADE")
	cfg.ConnConfig.RuntimeParams["search_path"] = schema + ",pg_catalog"
	scoped, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer scoped.Close()
	migrations, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range migrations {
		for _, sql := range m.Statements {
			if _, err = scoped.Exec(ctx, sql); err != nil {
				t.Fatal(err)
			}
		}
	}
	call := func(f func(context.Context, store.Queryer, []string) (uint32, []string, error), args ...string) (uint32, []string, error) {
		tx, e := scoped.Begin(ctx)
		if e != nil {
			return 0, nil, e
		}
		defer tx.Rollback(ctx)
		s, c, e := f(ctx, cacheReplayQueryer{traceReplayQueryer{tx}}, args)
		if e == nil {
			e = tx.Commit(ctx)
		}
		return s, c, e
	}
	var wg sync.WaitGroup
	errors := make(chan error, 32)
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s, c, e := call(sessionCacheCAS, "session-a", "opaque-key", "0", "nonce-a", "alpha")
			if e != nil || s != store.StatusOK || len(c) != 1 || c[0] != "1" {
				errors <- fmt.Errorf("concurrent CAS status=%d cells=%v err=%v", s, c, e)
			}
		}()
	}
	wg.Wait()
	close(errors)
	for e := range errors {
		t.Error(e)
	}
	check := func(f func(context.Context, store.Queryer, []string) (uint32, []string, error), want uint32, args ...string) []string {
		t.Helper()
		s, c, e := call(f, args...)
		if e != nil || s != want {
			t.Fatalf("status=%d want=%d err=%v", s, want, e)
		}
		return c
	}
	check(sessionCacheCAS, store.StatusOK, "session-a", "opaque-key", "1", "nonce-b", "beta")
	c := check(sessionCacheReceipt, store.StatusOK, "session-a", "opaque-key", "nonce-a")
	if len(c) != 2 || c[0] != "alpha" || c[1] != "1" {
		t.Fatal("nonce result changed after intervening CAS")
	}
	check(sessionCacheCAS, store.StatusOK, "session-a", "opaque-key", "0", "nonce-a", "alpha")
	c = check(sessionCacheGet, store.StatusOK, "session-a", "opaque-key")
	if len(c) != 2 || c[0] != "beta" || c[1] != "2" {
		t.Fatal("retry reverted current state or advanced twice")
	}
	check(sessionCacheCAS, store.StatusInvalid, "session-a", "opaque-key", "0", "nonce-a", "forged")
	check(sessionCacheGet, store.StatusMissing, "session-b", "opaque-key")
	check(sessionCacheReceipt, store.StatusMissing, "session-b", "opaque-key", "nonce-a")
	check(sessionCacheCAS, store.StatusMissing, "session-b", "opaque-key", "0", "nonce-c", "foreign")
	check(sessionCacheDelete, store.StatusOK, "session-b")
	check(sessionCacheGet, store.StatusOK, "session-a", "opaque-key")
	check(sessionCacheDelete, store.StatusOK, "session-a")
	check(sessionCacheGet, store.StatusMissing, "session-a", "opaque-key")
	check(sessionCacheReceipt, store.StatusMissing, "session-a", "opaque-key", "nonce-a")
}
func TestSessionCacheRejectsMalformedBeforeDatabase(t *testing.T) {
	for _, f := range [][]string{{"", "key", "0", "nonce", "v"}, {"s", "k", "01", "n", "v"}, {"s", "k", "-1", "n", "v"}, {"s", "k", "0", "", "v"}} {
		s, _, e := sessionCacheCAS(t.Context(), nil, f)
		if s != store.StatusInvalid || e != nil {
			t.Fatal("malformed CAS reached DB")
		}
	}
}
