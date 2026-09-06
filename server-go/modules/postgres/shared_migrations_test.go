package postgres

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestSharedDatabaseRuntimeCannotRewriteMigrationHistory(t *testing.T) {
	admin := sharedTestDatabase(t)
	ctx := context.Background()
	// Unique cluster roles, used by actual authenticated connections (not SET
	// ROLE on an admin connection). No production role is altered or reused.
	base := admin.Config().ConnConfig.Database
	migratorName, runtimeName := base+"_m", base+"_r"
	migratorID, runtimeID := pgx.Identifier{migratorName}.Sanitize(), pgx.Identifier{runtimeName}.Sanitize()
	for _, role := range []string{migratorID, runtimeID} {
		if _, err := admin.Exec(ctx, "CREATE ROLE "+role+" LOGIN NOINHERIT NOBYPASSRLS NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD 'disposable-test-only'"); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() {
			cleanup, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			if _, err := admin.Exec(cleanup, "DROP OWNED BY "+role); err != nil {
				t.Error(err)
			}
			if _, err := admin.Exec(cleanup, "DROP ROLE "+role); err != nil {
				t.Error(err)
			}
		})
	}
	for _, sql := range []string{
		"REVOKE CREATE ON SCHEMA public FROM PUBLIC",
		"GRANT USAGE, CREATE ON SCHEMA public TO " + migratorID,
		"GRANT USAGE ON SCHEMA public TO " + runtimeID,
		"ALTER DEFAULT PRIVILEGES FOR ROLE " + migratorID + " IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO " + runtimeID,
		"ALTER DEFAULT PRIVILEGES FOR ROLE " + migratorID + " IN SCHEMA public GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO " + runtimeID,
	} {
		if _, err := admin.Exec(ctx, sql); err != nil {
			t.Fatal(err)
		}
	}
	openRole := func(name string) *pgxpool.Pool {
		config := admin.Config().Copy()
		config.ConnConfig.User, config.ConnConfig.Password = name, "disposable-test-only"
		pool, err := pgxpool.NewWithConfig(ctx, config)
		if err != nil {
			t.Fatal("cannot initialize disposable role pool")
		}
		t.Cleanup(pool.Close)
		if err := pool.Ping(ctx); err != nil {
			t.Fatal(err)
		}
		return pool
	}
	migration, runtime := openRole(migratorName), openRole(runtimeName)
	if err := validateStoreNamespace(ctx, runtime, migration); err != nil {
		t.Fatal(err)
	}
	store := sharedTestStoreWithPools(t, runtime, migration)
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	assertProtected := func() {
		t.Helper()
		for _, sql := range []string{
			"DELETE FROM schema_migrations",
			"UPDATE schema_migrations SET checksum='forged'",
			"INSERT INTO schema_migrations(owner,version,checksum) VALUES('forged',1,'forged')",
			"TRUNCATE schema_migrations",
			"DROP TABLE schema_migrations",
			"CREATE TABLE unauthorized_ddl(id int)",
		} {
			_, err := runtime.Exec(ctx, sql)
			var pgerr *pgconn.PgError
			if !errors.As(err, &pgerr) || pgerr.Code != "42501" {
				t.Fatalf("runtime authority check for %s: want 42501, got %v", sql, err)
			}
		}
	}
	assertProtected()
	// Simulate the previous deploy's blanket upgrade grants. Reconciliation
	// must protect an EXISTING ledger, not just fresh table creation.
	if _, err := migration.Exec(ctx, "GRANT ALL ON TABLE schema_migrations TO "+runtimeID); err != nil {
		t.Fatal(err)
	}
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	assertProtected()
	if _, err := migration.Exec(ctx, "GRANT ALL ON TABLE schema_migrations TO "+runtimeID); err != nil {
		t.Fatal(err)
	}
	history, err := families.Migrations()
	if err != nil {
		t.Fatal(err)
	}
	first := history[0]
	if err := store.Migrate(ctx, db.MigrationRequest{Owner: first.Owner, Version: first.Version, Checksum: first.Checksum, Statements: first.Statements}); err != nil {
		t.Fatal(err)
	}
	assertProtected()
	var version int
	if err := migration.QueryRow(ctx, "SELECT count(*) FROM schema_migrations").Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != families.SchemaFileCount() {
		t.Fatalf("ledger rows = %d", version)
	}
	// Ordinary domain traffic remains usable over the restricted wire pool.
	if _, err := store.Exec(ctx, "INSERT INTO memory_runtime_state(state_key,state_value) VALUES('role-test','retained')"); err != nil {
		t.Fatal(err)
	}
	var retained string
	if err := store.QueryRow(ctx, "SELECT state_value FROM memory_runtime_state WHERE state_key='role-test'").Scan(&retained); err != nil || retained != "retained" {
		t.Fatalf("restricted runtime round trip: %q: %v", retained, err)
	}
	// Identical DSN search_path text is insufficient: the implicit $user entry
	// resolves differently once a role's own schema exists.
	if _, err := admin.Exec(ctx, "CREATE SCHEMA "+migratorID+" AUTHORIZATION "+migratorID); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if _, err := admin.Exec(context.Background(), "DROP SCHEMA "+migratorID); err != nil {
			t.Error(err)
		}
	})
	if err := validateStoreNamespace(ctx, runtime, migration); err == nil {
		t.Fatal("different role-resolved namespaces were accepted")
	}
}

func TestSharedDatabaseMigrationFailureAndReplay(t *testing.T) {
	pool := sharedTestDatabase(t)
	store := sharedTestStore(t, pool)
	ctx := context.Background()
	makeMigration := func(version int64, sql string) db.MigrationRequest {
		m := db.MigrationRequest{Owner: "runtime-test", Version: version, Statements: []string{sql}}
		m.Checksum = db.StoreChecksum(m.Statements)
		return m
	}
	first := makeMigration(1, "CREATE TABLE retained (id integer PRIMARY KEY); INSERT INTO retained VALUES (1)")
	if err := store.Migrate(ctx, first); err != nil {
		t.Fatal(err)
	}
	failed := makeMigration(2, "CREATE TABLE rolled_back (id integer); INSERT INTO missing_table VALUES (1)")
	if err := store.Migrate(ctx, failed); err == nil {
		t.Fatal("invalid migration succeeded")
	}
	var absent bool
	if err := pool.QueryRow(ctx, "SELECT to_regclass('rolled_back') IS NULL").Scan(&absent); err != nil {
		t.Fatal(err)
	}
	if !absent {
		t.Fatal("failed migration left partial DDL")
	}
	version, checksum, err := store.CurrentSchemaVersion(ctx, first.Owner)
	if err != nil || version != 1 || checksum != first.Checksum {
		t.Fatalf("failed migration advanced history: %d/%s: %v", version, checksum, err)
	}
	second := makeMigration(2, "ALTER TABLE retained ADD COLUMN note text NOT NULL DEFAULT 'kept'")
	if err := store.Migrate(ctx, second); err != nil {
		t.Fatal(err)
	}
	// A replay must not execute non-idempotent DDL or insert the data twice.
	if err := store.Migrate(ctx, first); err != nil {
		t.Fatal(err)
	}
	if err := store.Migrate(ctx, second); err != nil {
		t.Fatal(err)
	}
	changed := makeMigration(1, "DROP TABLE retained")
	if err := store.Migrate(ctx, changed); err == nil {
		t.Fatal("changed installed checksum was accepted")
	}
	if err := store.Migrate(ctx, makeMigration(4, "DROP TABLE retained")); err == nil {
		t.Fatal("migration history gap was accepted")
	}
	badChecksum := makeMigration(3, "DROP TABLE retained")
	badChecksum.Checksum = strings.Repeat("0", 64)
	if err := store.Migrate(ctx, badChecksum); err == nil {
		t.Fatal("forged checksum was accepted")
	}
	var count int
	var note string
	if err := pool.QueryRow(ctx, "SELECT count(*), min(note) FROM retained").Scan(&count, &note); err != nil {
		t.Fatal(err)
	}
	if count != 1 || note != "kept" {
		t.Fatal("migration replay damaged installed data")
	}
	version, checksum, err = store.CurrentSchemaVersion(ctx, first.Owner)
	if err != nil || version != 2 || checksum != second.Checksum {
		t.Fatalf("replay changed history: %d/%s: %v", version, checksum, err)
	}
}

func TestSharedDatabaseStartupRejectsHistoryDrift(t *testing.T) {
	for _, corruption := range []string{"checksum", "missing-version", "newer-binary"} {
		t.Run(corruption, func(t *testing.T) {
			pool := sharedTestDatabase(t)
			store := sharedTestStore(t, pool)
			ctx := context.Background()
			if err := families.ApplySchema(ctx, store); err != nil {
				t.Fatal(err)
			}
			if _, err := store.Exec(ctx, "INSERT INTO memory_runtime_state(state_key,state_value) VALUES('keep','unchanged')"); err != nil {
				t.Fatal(err)
			}
			var sql string
			switch corruption {
			case "checksum":
				sql = "UPDATE schema_migrations SET checksum='changed' WHERE version=1"
			case "missing-version":
				sql = "DELETE FROM schema_migrations WHERE version=1"
			case "newer-binary":
				sql = "INSERT INTO schema_migrations(owner,version,checksum) SELECT owner,max(version)+1,'future' FROM schema_migrations GROUP BY owner"
			}
			if _, err := pool.Exec(ctx, sql); err != nil {
				t.Fatal(err)
			}
			if err := families.ApplySchema(ctx, store); err == nil {
				t.Fatal("startup accepted incompatible migration history")
			}
			var retained string
			if err := store.QueryRow(ctx, "SELECT state_value FROM memory_runtime_state WHERE state_key='keep'").Scan(&retained); err != nil || retained != "unchanged" {
				t.Fatalf("rejected startup changed domain data: %q: %v", retained, err)
			}
		})
	}
}

func TestSharedDatabaseSchemaLockCancellation(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx := context.Background()
	blocker, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer rollbackSchemaTransaction(blocker)
	if _, err := blocker.Exec(ctx, schemaMigrationLockSQL); err != nil {
		t.Fatal(err)
	}
	blocked, cancel := context.WithTimeout(ctx, 50*time.Millisecond)
	defer cancel()
	if tx, err := beginSchemaTransaction(blocked, pool); err == nil {
		rollbackSchemaTransaction(tx)
		t.Fatal("schema transaction bypassed a held bootstrap lock")
	} else if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("lock wait did not respect cancellation: %v", err)
	}
	if err := blocker.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	// Cancellation must return the connection and release the transaction so a
	// later startup succeeds, rather than leaving the module permanently stuck.
	retry, stop := context.WithTimeout(ctx, 2*time.Second)
	defer stop()
	tx, err := beginSchemaTransaction(retry, pool)
	if err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(retry); err != nil {
		t.Fatal(err)
	}
	if got := pool.Stat().AcquiredConns(); got != 0 {
		t.Fatalf("schema work retained %d pool leases", got)
	}
}

func TestSharedDatabaseNativeAndGoBootstrapShareLock(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx := context.Background()
	body, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	blocker, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer rollbackSchemaTransaction(blocker)
	if _, err := blocker.Exec(ctx, schemaMigrationLockSQL); err != nil {
		t.Fatal(err)
	}
	store := sharedTestStore(t, pool)
	done := make(chan error, 2)
	go func() {
		// Same single-command application used by db_apply_schema_postgres.
		_, err := pool.Exec(ctx, strings.ReplaceAll(string(body), "__EMBED_DIM__", "3"))
		done <- err
	}()
	go func() { _, _, err := store.CurrentSchemaVersion(ctx, "runtime-test"); done <- err }()
	deadline, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	tick := time.NewTicker(10 * time.Millisecond)
	defer tick.Stop()
	for {
		var waiters int
		if err := pool.QueryRow(deadline, `SELECT count(*) FROM pg_stat_activity
WHERE datname=current_database() AND wait_event='advisory'`).Scan(&waiters); err != nil {
			t.Fatal(err)
		}
		if waiters == 2 {
			break
		}
		select {
		case err := <-done:
			t.Fatalf("bootstrap bypassed the shared lock: %v", err)
		case <-deadline.Done():
			t.Fatal("native and Go bootstraps did not both wait on the same lock")
		case <-tick.C:
		}
	}
	if err := blocker.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if err := <-done; err != nil {
			t.Fatal(err)
		}
	}
}
