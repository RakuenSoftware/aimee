package postgres

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Each test creates its own database: the knowledge schema deliberately pins
// security-definer functions to public, so a search_path-only fixture would
// touch the wrong objects. Never apply it to the operator-supplied database.
func sharedTestDatabase(t *testing.T) *pgxpool.Pool {
	t.Helper()
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL is required for the shared database gate")
		}
		t.Skip("set AIMEE_DB_TEST_URL to an explicit test PostgreSQL admin DSN")
	}
	ctx := context.Background()
	config, err := pgxpool.ParseConfig(url)
	if err != nil {
		t.Fatal("invalid shared database test DSN")
	}
	admin, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal("cannot initialize test admin connection")
	}
	t.Cleanup(admin.Close)
	var nonce [12]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		t.Fatal(err)
	}
	name := "aimee_shared_test_" + hex.EncodeToString(nonce[:])
	quoted := pgx.Identifier{name}.Sanitize()
	if _, err := admin.Exec(ctx, "CREATE DATABASE "+quoted); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cleanup, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if _, err := admin.Exec(cleanup, "DROP DATABASE "+quoted); err != nil {
			t.Error(err)
		}
	})
	config = config.Copy()
	config.ConnConfig.Database = name
	config.MaxConns = 16
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal("cannot initialize isolated test database")
	}
	t.Cleanup(pool.Close)
	return pool
}

// Only the bus boundary is in-process. Both sides of the production binary SQL
// wire, the provider's transaction/ledger code, and PostgreSQL itself are real.
type sharedStoreCaller struct{ handler bus.ModuleHandler }

func (c sharedStoreCaller) Call(ctx context.Context, kind, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if kind != db.EventPostgresSQL || stage != db.StagePostgresSQL {
		return nil, fmt.Errorf("unexpected database route %d/%d", kind, stage)
	}
	body, status := c.handler(bus.ModuleInvocation{StageID: stage}, request)
	if status != bus.ModuleStatusOK {
		return nil, fmt.Errorf("database transport status %d", status)
	}
	return body, nil
}

func sharedTestStore(t *testing.T, pool *pgxpool.Pool) db.Store {
	return sharedTestStoreWithPools(t, pool, pool)
}

func sharedTestStoreWithPools(t *testing.T, runtime, migration *pgxpool.Pool) db.Store {
	t.Helper()
	handler := &sqlHandler{txs: newTxRegistry(),
		poolFn:          func(context.Context) (*pgxpool.Pool, error) { return runtime, nil },
		migrationPoolFn: func(context.Context) (*pgxpool.Pool, error) { return migration, nil }}
	store, err := db.NewStore(sharedStoreCaller{handler.handle})
	if err != nil {
		t.Fatal(err)
	}
	return store
}

func sharedParallel(t *testing.T, count int, run func(int) error) {
	t.Helper()
	start := make(chan struct{})
	errs := make(chan error, count)
	var workers sync.WaitGroup
	for i := 0; i < count; i++ {
		workers.Add(1)
		go func() { defer workers.Done(); <-start; errs <- run(i) }()
	}
	close(start)
	workers.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Error(err)
		}
	}
}

func TestSharedDatabaseConcurrentFirstMigration(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx := context.Background()
	// Separate provider instances model independent KB/server process starts.
	stores := []db.Store{sharedTestStore(t, pool), sharedTestStore(t, pool)}
	if _, _, err := stores[0].CurrentSchemaVersion(ctx, "runtime-test"); err != nil {
		t.Fatal(err)
	}
	migration := db.MigrationRequest{Owner: "runtime-test", Version: 1,
		Statements: []string{"SELECT pg_sleep(0.1); CREATE TABLE boot_once (id integer PRIMARY KEY); INSERT INTO boot_once VALUES (1)"}}
	migration.Checksum = db.StoreChecksum(migration.Statements)
	sharedParallel(t, 12, func(i int) error { return stores[i%2].Migrate(ctx, migration) })
	var count int
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM boot_once").Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("migration ran %d times", count)
	}
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM schema_migrations").Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("recorded %d migration rows", count)
	}
}

func TestSharedDatabaseConcurrentLedgerBootstrap(t *testing.T) {
	pool := sharedTestDatabase(t)
	stores := make([]db.Store, 12)
	for i := range stores {
		stores[i] = sharedTestStore(t, pool)
	}
	sharedParallel(t, len(stores), func(i int) error {
		version, checksum, err := stores[i].CurrentSchemaVersion(context.Background(), "fresh")
		if err == nil && (version != 0 || checksum != "") {
			return fmt.Errorf("fresh ledger returned %d/%s", version, checksum)
		}
		return err
	})
}

func TestSharedDatabaseBothDomainSchemas(t *testing.T) {
	for _, order := range []string{"knowledge-first", "runtime-first", "concurrent"} {
		t.Run(order, func(t *testing.T) {
			pool := sharedTestDatabase(t)
			ctx := context.Background()
			body, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
			if err != nil {
				t.Fatal(err)
			}
			knowledgeSQL := strings.ReplaceAll(string(body), "__EMBED_DIM__", "3")
			applyKnowledge := func() error { _, err := pool.Exec(ctx, knowledgeSQL); return err }
			store := sharedTestStore(t, pool)
			sibling := sharedTestStore(t, pool)
			applyRuntime := func() error { return families.ApplySchema(ctx, store) }
			switch order {
			case "knowledge-first":
				if err := applyKnowledge(); err != nil {
					t.Fatal(err)
				}
				if err := applyRuntime(); err != nil {
					t.Fatal(err)
				}
			case "runtime-first":
				if err := applyRuntime(); err != nil {
					t.Fatal(err)
				}
				if err := applyKnowledge(); err != nil {
					t.Fatal(err)
				}
			case "concurrent":
				sharedParallel(t, 3, func(i int) error {
					if i == 0 {
						return applyKnowledge()
					}
					if i == 1 {
						return applyRuntime()
					}
					return families.ApplySchema(ctx, sibling)
				})
			}
			if t.Failed() {
				return
			}
			var dimension int
			if err := pool.QueryRow(ctx, "SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'").Scan(&dimension); err != nil {
				t.Fatal(err)
			}
			if dimension != 3 {
				t.Fatalf("knowledge vector schema was not applied: dimension %d", dimension)
			}
			scope := memory.Scope{Type: "global", Value: "_global"}
			dataStores := make([]memory.DataStore, 0, 2)
			records := make([]memory.Record, 0, 2)
			for _, placement := range []memory.Placement{memory.PlacementKB, memory.PlacementServer} {
				data, err := memory.NewPostgresDataStore(store, placement)
				if err != nil {
					t.Fatal(err)
				}
				record, err := data.Put(ctx, scope, memory.Record{Tier: "L0", Kind: "fact", Key: "shared-memory-probe", Content: string(placement) + " retained content", Confidence: 0.8})
				if err != nil {
					t.Fatal(err)
				}
				dataStores = append(dataStores, data)
				records = append(records, record)
			}
			// Exercise both domains through the same shared database client, then
			// replay both bootstraps and prove the existing rows survive.
			for _, sql := range []string{
				"INSERT INTO user_memories(kind,key,content,created_at,updated_at) VALUES ('fact','shared-runtime','runtime-value','2026-01-01','2026-01-01')",
				"INSERT INTO kb_meta(key,value) VALUES ('shared-database-probe','knowledge-value')",
			} {
				if _, err := store.Exec(ctx, sql); err != nil {
					t.Fatal(err)
				}
			}
			if err := applyKnowledge(); err != nil {
				t.Fatal(err)
			}
			if err := applyRuntime(); err != nil {
				t.Fatal(err)
			}
			var runtime, knowledge string
			if err := store.QueryRow(ctx, "SELECT content FROM user_memories WHERE key='shared-runtime'").Scan(&runtime); err != nil {
				t.Fatal(err)
			}
			if err := store.QueryRow(ctx, "SELECT value FROM kb_meta WHERE key='shared-database-probe'").Scan(&knowledge); err != nil {
				t.Fatal(err)
			}
			if runtime != "runtime-value" || knowledge != "knowledge-value" {
				t.Fatal("bootstrap changed existing domain data")
			}
			for i, data := range dataStores {
				got, err := data.Get(ctx, scope, records[i].ID)
				if err != nil || got.Content != records[i].Content {
					t.Fatalf("placement data changed across bootstrap: %+v: %v", got, err)
				}
				found, err := data.Search(ctx, scope, "shared-memory-probe", "", "", 10)
				if err != nil || len(found) != 1 || found[0].Content != records[i].Content {
					t.Fatalf("placement search crossed domains: %+v: %v", found, err)
				}
				if deleted, err := data.Delete(ctx, scope, got.ID); err != nil || !deleted {
					t.Fatalf("delete did not persist: %t: %v", deleted, err)
				}
				if _, err := data.Get(ctx, scope, got.ID); !errors.Is(err, memory.ErrMemoryNotFound) {
					t.Fatalf("deleted memory remained visible: %v", err)
				}
			}
		})
	}
}
