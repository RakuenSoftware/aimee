package postgres

import (
	"strings"
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
