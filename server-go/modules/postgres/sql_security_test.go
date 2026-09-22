package postgres

import (
	"context"
	"encoding/pem"
	"github.com/jackc/pgx/v5/pgxpool"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestParseStoreConfigRejectsInvalidUTF8WithoutLooping(t *testing.T) {
	done := make(chan error, 1)
	go func() {
		_, err := parseStoreConfig("postgres://user:pass@localhost/db?service=" + string([]byte{0xff, 0xfe}))
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("invalid UTF-8 in AIMEE_STORE_URL was accepted")
		}
		if !strings.Contains(err.Error(), "AIMEE_STORE_URL") {
			t.Fatalf("error did not identify the input boundary: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("invalid UTF-8 made DSN parsing fail to terminate")
	}
}

func TestParseMigrationConfigRequiresDifferentDatabaseRole(t *testing.T) {
	_, err := parseMigrationConfig(
		"postgres://shared:one@localhost/db?application_name=migrator",
		"postgres://shared:one@localhost/db?application_name=runtime")
	if err == nil || !strings.Contains(err.Error(), "must differ") {
		t.Fatalf("same database role was not rejected: %v", err)
	}

	config, err := parseMigrationConfig("postgres://migrator:one@localhost/db",
		"postgres://runtime:two@localhost/db")
	if err != nil {
		t.Fatalf("distinct roles were rejected: %v", err)
	}
	if config.ConnConfig.User != "migrator" {
		t.Fatalf("migration role = %q, want migrator", config.ConnConfig.User)
	}
}

func TestParseMigrationConfigRequiresSameTarget(t *testing.T) {
	runtime := "postgres://runtime:secret@localhost:5432/db?sslmode=disable&search_path=public"
	for _, migration := range []string{
		"postgres://migrator:secret@localhost:5432/other?sslmode=disable&search_path=public",
		"postgres://migrator:secret@other:5432/db?sslmode=disable&search_path=public",
		"postgres://migrator:secret@localhost:5433/db?sslmode=disable&search_path=public",
		"postgres://migrator:secret@localhost:5432/db?sslmode=disable&search_path=other",
		"postgres://migrator:secret@localhost:5432/db?sslmode=disable&search_path=public&options=-csearch_path%3Dother",
		"postgres://migrator:secret@localhost:5432,backup:5432/db?sslmode=disable&search_path=public",
	} {
		if _, err := parseMigrationConfig(migration, runtime); err == nil {
			t.Error("mismatched database configuration was accepted")
		} else if strings.Contains(err.Error(), "secret") {
			t.Fatal("configuration error disclosed credentials")
		}
	}
	if _, err := parseMigrationConfig(
		"postgres://migrator:secret@localhost:5432/db?sslmode=disable&search_path=public&application_name=migration", runtime); err != nil {
		t.Fatalf("same target with separate credentials and application name: %v", err)
	}
}

func TestInvalidDatabaseConfigDoesNotDiscloseCredentials(t *testing.T) {
	_, err := parseStoreConfig("postgres://runtime:do-not-disclose@localhost:not-a-port/db")
	if err == nil || strings.Contains(err.Error(), "do-not-disclose") {
		t.Fatal("invalid DSN must fail without disclosing credentials")
	}
}

// The TLS volume is populated by the PostgreSQL service after an upgraded
// application can already receive requests. Its absence must not poison the
// runtime pool for the lifetime of the Go process.
func TestSQLPoolRecoversWhenTLSCertificateArrives(t *testing.T) {
	tlsServer := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	defer tlsServer.Close()
	cert := filepath.Join(t.TempDir(), "store.crt")
	t.Setenv("AIMEE_STORE_URL", "host=127.0.0.1 user=runtime dbname=fixture sslmode=verify-full sslrootcert="+cert)
	t.Cleanup(func() {
		sqlPoolMu.Lock()
		defer sqlPoolMu.Unlock()
		if sqlPool != nil {
			sqlPool.Close()
			sqlPool = nil
		}
	})
	if pool, err := SQLPool(context.Background()); err == nil || pool != nil {
		t.Fatal("missing TLS trust file unexpectedly initialized the pool")
	}
	if err := os.WriteFile(cert, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: tlsServer.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	pools := make(chan *pgxpool.Pool, 8)
	failures := make(chan error, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); pool, err := SQLPool(context.Background()); pools <- pool; failures <- err }()
	}
	wg.Wait()
	close(pools)
	close(failures)
	for err := range failures {
		if err != nil {
			t.Fatal(err)
		}
	}
	var first *pgxpool.Pool
	for pool := range pools {
		if pool == nil {
			t.Fatal("nil recovered pool")
		}
		if first == nil {
			first = pool
		} else if pool != first {
			t.Fatal("concurrent callers created different pools")
		}
	}
}

func TestMigrationPoolRetriesConfigurationFailure(t *testing.T) {
	t.Setenv("AIMEE_HOME", "")
	t.Setenv("AIMEE_STORE_MIGRATION_URL", "")
	t.Setenv("AIMEE_STORE_URL", "host=localhost user=runtime dbname=fixture sslmode=disable")
	if _, err := MigrationPool(context.Background()); err == nil || !strings.Contains(err.Error(), "unset") {
		t.Fatalf("missing migration credential: %v", err)
	}
	t.Setenv("AIMEE_STORE_MIGRATION_URL", "host=localhost user=runtime dbname=fixture sslmode=disable")
	if _, err := MigrationPool(context.Background()); err == nil || !strings.Contains(err.Error(), "must differ") {
		t.Fatalf("migration must revalidate the newly available credential: %v", err)
	}
}
