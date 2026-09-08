package families

import (
	"context"
	"fmt"
	"testing"
	"time"
)

// The upgrade runs the actual released identity schema and appended migration
// around an existing owner and certificate. A transaction-local schema keeps
// this fixture separate even when other integration tests share the database.
func TestClientDeviceMigrationPreservesExistingPairingLive(t *testing.T) {
	pool := livePool(t)
	ctx := context.Background()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	schema := fmt.Sprintf("pairing_upgrade_%d", time.Now().UnixNano())
	if _, err = tx.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, "SET LOCAL search_path="+schema); err != nil {
		t.Fatal(err)
	}
	migrations, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	apply := func(name string) {
		t.Helper()
		for _, m := range migrations {
			if m.Name == name {
				for _, statement := range m.Statements {
					if _, err := tx.Exec(ctx, statement); err != nil {
						t.Fatal(err)
					}
				}
				return
			}
		}
		t.Fatalf("missing migration %s", name)
	}
	apply("schema_identity.sql")
	if _, err = tx.Exec(ctx, firstUserInsertSQL, "webuser:owner", int64(100)); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, grantInsertSQL, testBearer, "webuser:owner", int64(100)); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, "UPDATE remote_client_grants SET cert_serial='AABB',bound_at=101"); err != nil {
		t.Fatal(err)
	}
	apply("schema_client_devices.sql")
	var principal, hash, serial, tier, name string
	var expiry int64
	var revoked bool
	if err = tx.QueryRow(ctx, grantForPrincipalSQL, "webuser:owner", int64(200)).Scan(&principal, &hash, &serial, &tier); err != nil {
		t.Fatal(err)
	}
	if principal != "webuser:owner" || hash != testBearer || serial != "AABB" || tier != "full" {
		t.Fatal("upgrade changed the existing grant")
	}
	if err = tx.QueryRow(ctx, "SELECT device_name,expires_at,revoked_at IS NOT NULL FROM remote_client_grants").Scan(&name, &expiry, &revoked); err != nil {
		t.Fatal(err)
	}
	if name != "First client" || expiry != 0 || revoked {
		t.Fatal("upgrade expired or revoked the existing device")
	}
	if err = tx.QueryRow(ctx, boundSerialSQL, testBearer, int64(200)).Scan(&serial); err != nil || serial != "AABB" {
		t.Fatal("existing certificate binding is not recoverable", err)
	}
	if err = tx.QueryRow(ctx, tierSQL, "AABB").Scan(&principal, &tier); err != nil || tier != "full" {
		t.Fatal("existing client lost write authority", err)
	}
}
