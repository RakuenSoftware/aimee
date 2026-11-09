package postgres

import (
	"context"
	"crypto/sha256"
	"fmt"
	"strings"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	domain "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const historicalRuntime16SHA = "4acb2d3cde8a2c8416a47826e6b7e070b3e60d40a6324066744c356ecddea892"
const historicalRuntime16Checksum = "2fb3ca4622fc18a498d7b19b8cac8b5828493bfeff7c647f25f661e2fbf855f6"

func TestSessionCacheMigrationHistoryAppendOnly(t *testing.T) {
	history, err := families.Migrations()
	if err != nil {
		t.Fatal(err)
	}
	if len(history) < 56 {
		t.Fatalf("expected appended version56, got only %d", len(history))
	}
	old := history[15]
	if old.Version != 16 || old.Name != "schema_runtime.sql" || len(old.Statements) != 1 || fmt.Sprintf("%x", sha256.Sum256([]byte(old.Statements[0]))) != historicalRuntime16SHA || old.Checksum != historicalRuntime16Checksum {
		t.Fatal("historical runtime migration16 bytes/checksum changed")
	}
	latest := history[55]
	if latest.Version != 56 || latest.Name != "schema_session_cache.sql" || !strings.Contains(strings.Join(latest.Statements, "\n"), "ALTER TABLE context_cache ADD COLUMN IF NOT EXISTS revision") {
		t.Fatal("session cache extension must be appended at version56")
	}
	for _, m := range history[:55] {
		if strings.Contains(strings.Join(m.Statements, "\n"), "CREATE TABLE IF NOT EXISTS session_cache_receipts") {
			t.Fatal("new cache receipts were inserted into historical migration")
		}
	}
}

func TestSharedSessionCacheUpgradeVerifiesHistoryAndRetainsLegacyRows(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	store := sharedTestStore(t, pool)
	history, err := families.Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range history {
		if m.Version >= 56 {
			break
		}
		if err = store.Migrate(ctx, domain.MigrationRequest{Owner: m.Owner, Version: m.Version, Checksum: m.Checksum, Statements: m.Statements}); err != nil {
			t.Fatal(err)
		}
	}
	var checksum string
	if err = pool.QueryRow(ctx, `SELECT checksum FROM schema_migrations WHERE owner='db1' AND version=16`).Scan(&checksum); err != nil || checksum != historicalRuntime16Checksum {
		t.Fatal("legacy runtime checksum was not recorded", err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO context_cache(hash,output,session_id) VALUES('retained-key','retained-value','retained-session')`); err != nil {
		t.Fatal(err)
	}
	var columns int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM information_schema.columns WHERE table_schema='public' AND table_name='context_cache' AND column_name='revision'`).Scan(&columns); err != nil || columns != 0 {
		t.Fatal("fixture did not retain legacy table shape", err)
	}
	// The real production migrator must reject a changed ledger before adding56.
	if _, err = pool.Exec(ctx, `UPDATE schema_migrations SET checksum='forged' WHERE owner='db1' AND version=16`); err != nil {
		t.Fatal(err)
	}
	if err = families.ApplySchema(ctx, store); err == nil {
		t.Fatal("changed migration16 history was accepted")
	}
	var latest int64
	if err = pool.QueryRow(ctx, `SELECT max(version) FROM schema_migrations WHERE owner='db1'`).Scan(&latest); err != nil || latest != 55 {
		t.Fatal("upgrade advanced after historical mismatch", err)
	}
	if _, err = pool.Exec(ctx, `UPDATE schema_migrations SET checksum=$1 WHERE owner='db1' AND version=16`, historicalRuntime16Checksum); err != nil {
		t.Fatal(err)
	}
	var historicalAppliedAt time.Time
	if err = pool.QueryRow(ctx, `DELETE FROM schema_migrations WHERE owner='db1' AND version=16 RETURNING applied_at`).Scan(&historicalAppliedAt); err != nil {
		t.Fatal(err)
	}
	if err = families.ApplySchema(ctx, store); err == nil {
		t.Fatal("missing migration16 history was accepted")
	}
	if err = pool.QueryRow(ctx, `SELECT max(version) FROM schema_migrations WHERE owner='db1'`).Scan(&latest); err != nil || latest != 55 {
		t.Fatal("upgrade advanced after missing historical row", err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO schema_migrations(owner,version,checksum,applied_at) VALUES('db1',16,$1,$2)`, historicalRuntime16Checksum, historicalAppliedAt); err != nil {
		t.Fatal(err)
	}
	if err = families.ApplySchema(ctx, store); err != nil {
		t.Fatal("actual existing-history upgrade", err)
	}
	if err = families.ApplySchema(ctx, store); err != nil {
		t.Fatal("exact migration replay", err)
	}
	if err = pool.QueryRow(ctx, `SELECT max(version) FROM schema_migrations WHERE owner='db1'`).Scan(&latest); err != nil || latest != history[len(history)-1].Version {
		t.Fatal("migration56 not recorded", err)
	}
	if err = pool.QueryRow(ctx, `SELECT checksum FROM schema_migrations WHERE owner='db1' AND version=16`).Scan(&checksum); err != nil || checksum != historicalRuntime16Checksum {
		t.Fatal("upgrade rewrote historical checksum", err)
	}
	var value string
	var revision int64
	if err = pool.QueryRow(ctx, `SELECT output,revision FROM context_cache WHERE hash='retained-key' AND session_id='retained-session'`).Scan(&value, &revision); err != nil || value != "retained-value" || revision != 1 {
		t.Fatal("upgrade lost legacy cache value/revision", err)
	}
	// Use the real family dispatcher, transactional SQL wire and receipt path.
	call := func(op uint32, args ...string) []string {
		t.Helper()
		raw, e := wire.EncodeFields(op, args)
		if e != nil {
			t.Fatal(e)
		}
		reply, status := families.Runtime.Handler(store)(ring.ModuleInvocation{StageID: families.StageRuntime}, raw)
		if status != ring.ModuleStatusOK {
			t.Fatal("runtime transport failed", status)
		}
		s, c, e := wire.DecodeFields(reply)
		if e != nil || s != domain.StatusOK {
			t.Fatalf("runtime op%d status%d err%v", op, s, e)
		}
		return c
	}
	if c := call(63, "retained-session", "retained-key"); len(c) != 2 || c[0] != "retained-value" || c[1] != "1" {
		t.Fatal("retained row unavailable through upgraded API")
	}
	if c := call(64, "retained-session", "retained-key", "1", "upgrade-nonce", "replacement"); len(c) != 1 || c[0] != "2" {
		t.Fatal("upgraded CAS failed")
	}
	call(64, "retained-session", "retained-key", "1", "upgrade-nonce", "replacement")
	if c := call(63, "retained-session", "retained-key"); len(c) != 2 || c[0] != "replacement" || c[1] != "2" {
		t.Fatal("CAS replay advanced twice")
	}
	if c := call(66, "retained-session", "retained-key", "upgrade-nonce"); len(c) != 2 || c[0] != "replacement" || c[1] != "2" {
		t.Fatal("upgraded receipt unavailable")
	}
}
