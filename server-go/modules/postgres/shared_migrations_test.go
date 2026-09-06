package postgres

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/db"
)

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
